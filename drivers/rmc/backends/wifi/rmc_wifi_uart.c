/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rmc_wifi_uart.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(rmc_wifi, CONFIG_RMC_LOG_LEVEL);

/* -------------------------------------------------------------------- */
/* Framer: consumes one byte at a time, brace-depth + JSON-string aware */
/*                                                                       */
/* A frame is only ever considered valid if it starts with '{' and its  */
/* matching '}' is seen (brace_depth returns to 0). Anything before the */
/* first '{' is discarded as noise. Because the frame is emitted the    */
/* instant its closing '}' is found and the state machine immediately  */
/* resets, back-to-back packets with no gap between them are each       */
/* extracted correctly, and a packet split across multiple UART         */
/* interrupts/ring-buffer drains is simply resumed on the next byte -   */
/* framer state (frame_active/brace_depth/in_string/escape_next)        */
/* persists across calls for exactly this reason.                       */
/* -------------------------------------------------------------------- */

static void framer_reset(struct rmc_wifi_uart *u)
{
	u->frame_len = 0;
	u->brace_depth = 0;
	u->in_string = false;
	u->escape_next = false;
	u->frame_active = false;
}

static void framer_feed(struct rmc_wifi_uart *u, uint8_t byte)
{
	if (!u->frame_active) {
		if (byte == '{') {
			/* Start of a new packet. */
			u->frame_active = true;
			u->brace_depth = 1;
			u->frame_len = 0;
			u->frame_buf[u->frame_len++] = byte;
		}
		/* Anything before the first '{' (stray bytes, line noise,
		 * leftovers from a previous malformed packet) is discarded.
		 */
		return;
	}

	if (u->frame_len >= sizeof(u->frame_buf) - 1) {
		/* Frame too large - abandon it and resync on the next '{'. */
		u->frame_overflow_count++;
		LOG_WRN("RX frame exceeded %zu bytes, dropping", sizeof(u->frame_buf) - 1);
		framer_reset(u);
		return;
	}

	u->frame_buf[u->frame_len++] = byte;

	if (u->in_string) {
		if (u->escape_next) {
			u->escape_next = false;
		} else if (byte == '\\') {
			u->escape_next = true;
		} else if (byte == '"') {
			u->in_string = false;
		}
		return;
	}

	switch (byte) {
	case '"':
		u->in_string = true;
		break;
	case '{':
		u->brace_depth++;
		break;
	case '}':
		u->brace_depth--;
		if (u->brace_depth == 0) {
			/* Complete, valid {...} packet: starts with '{' (guaranteed
			 * on entry above) and its matching '}' was just seen.
			 */
			u->frame_buf[u->frame_len] = '\0';
			if (u->frame_cb != NULL) {
				u->frame_cb(u->frame_buf, u->frame_len, u->frame_cb_user_data);
			}
			framer_reset(u);
			/* Ready immediately for the next packet, so back-to-back
			 * packets already sitting in the ring buffer are picked up
			 * on the very next byte fed to this function.
			 */
		}
		break;
	default:
		break;
	}
}

static void rmc_wifi_uart_rx_work_handler(struct k_work *work)
{
	struct rmc_wifi_uart *u = CONTAINER_OF(work, struct rmc_wifi_uart, rx_work);
	uint8_t byte;

	/* Drain everything currently buffered; framer_feed() handles partial
	 * packets (state persists until the next call) and multiple
	 * complete packets in the same drain transparently.
	 */
	while (ring_buf_get(&u->rx_rb, &byte, 1) == 1) {
		framer_feed(u, byte);
	}
}

/* -------------------------------------------------------------------- */
/* ISR - RX only, one byte per uart_fifo_read() call                    */
/* -------------------------------------------------------------------- */

static void rmc_wifi_uart_isr(const struct device *dev, void *user_data)
{
	struct rmc_wifi_uart *u = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t byte;

			/* Explicitly one byte per uart_fifo_read() call (not a bulk
			 * multi-byte read) - looped until the FIFO is drained.
			 */
			while (uart_fifo_read(dev, &byte, 1) == 1) {
				if (ring_buf_put(&u->rx_rb, &byte, 1) != 1) {
					u->frame_overflow_count++;
					LOG_WRN("RX ring buffer full, byte dropped");
				}
			}

			k_work_submit(&u->rx_work);
		}
	}
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */

int rmc_wifi_uart_init(struct rmc_wifi_uart *u, const struct device *uart_dev,
			rmc_wifi_frame_cb_t frame_cb, void *user_data)
{
	int ret;

	if (u == NULL || uart_dev == NULL || frame_cb == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	memset(u, 0, sizeof(*u));
	u->dev = uart_dev;
	u->frame_cb = frame_cb;
	u->frame_cb_user_data = user_data;

	ring_buf_init(&u->rx_rb, sizeof(u->rx_rb_buf), u->rx_rb_buf);
	k_work_init(&u->rx_work, rmc_wifi_uart_rx_work_handler);
	framer_reset(u);

	uart_irq_rx_disable(uart_dev);
	uart_irq_tx_disable(uart_dev);

	ret = uart_irq_callback_user_data_set(uart_dev, rmc_wifi_uart_isr, u);
	if (ret < 0) {
		LOG_ERR("Failed to set UART IRQ callback: %d", ret);
		return ret;
	}

	uart_irq_rx_enable(uart_dev);

	return 0;
}

void rmc_wifi_uart_deinit(struct rmc_wifi_uart *u)
{
	if (u == NULL || u->dev == NULL) {
		return;
	}

	uart_irq_rx_disable(u->dev);
	k_work_cancel(&u->rx_work);
}

int rmc_wifi_uart_send(struct rmc_wifi_uart *u, const char *json, size_t len, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (u == NULL || json == NULL) {
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(u->dev, (unsigned char)json[i]);
	}

	return 0;
}
