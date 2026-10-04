/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rmc_serial_uart.h"
#include "drivers/rs_485.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(rmc_serial, CONFIG_RMC_LOG_LEVEL);

/* -------------------------------------------------------------------- */
/* Framer: consumes one byte at a time, { <payload> } delimited.        */
/*                                                                       */
/* A frame starts at 0x7B and ends at the next 0x7D; the bytes between   */
/* are the payload. Anything before a 0x7B is discarded as noise. A      */
/* second 0x7B before a 0x7D restarts the frame (resync). The small      */
/* fixed packets carried here never contain a 0x7B/0x7D byte, so no      */
/* escaping is needed. Framer state persists across calls so a packet    */
/* split across UART interrupts/ring-buffer drains simply resumes.       */
/* -------------------------------------------------------------------- */

static void framer_reset(struct rmc_serial_uart *u)
{
	u->frame_len = 0;
	u->frame_active = false;
}

static void framer_feed(struct rmc_serial_uart *u, uint8_t byte)
{
	if (byte == RMC_SERIAL_FRAME_START) {
		/* Start (or restart) a frame. */
		u->frame_active = true;
		u->frame_len = 0;
		return;
	}

	if (!u->frame_active) {
		/* Noise before the first '{' — discard. */
		return;
	}

	if (byte == RMC_SERIAL_FRAME_END) {
		/* Complete frame: frame_buf[0..frame_len) is the payload. */
		if (u->frame_cb != NULL && u->frame_len > 0) {
			u->frame_cb(u->frame_buf, u->frame_len, u->frame_cb_user_data);
		}
		framer_reset(u);
		return;
	}

	if (u->frame_len >= sizeof(u->frame_buf)) {
		/* Payload too large — abandon and resync on the next '{'. */
		u->frame_overflow_count++;
		LOG_WRN("RX frame exceeded %zu bytes, dropping", sizeof(u->frame_buf));
		framer_reset(u);
		return;
	}

	u->frame_buf[u->frame_len++] = byte;
}

static void rmc_serial_uart_rx_work_handler(struct k_work *work)
{
	struct rmc_serial_uart *u = CONTAINER_OF(work, struct rmc_serial_uart, rx_work);
	uint8_t byte;
	uint8_t dbg[32];
	size_t  n = 0;

	while (ring_buf_get(&u->rx_rb, &byte, 1) == 1) {
		if (n < sizeof(dbg)) {
			dbg[n++] = byte;
		}
		framer_feed(u, byte);
	}

	/* TEMP DEBUG: exactly what arrived on the RS-485 wire, framed or not.
	 * Nothing here when you send from Hercules => bytes aren't reaching the
	 * UART (wiring / DE / wrong COM). Bytes here but no frame delivered =>
	 * the framing is wrong (must be 7B <MTP> <SID> <MSG> 7D). */
	if (n > 0) {
		LOG_HEXDUMP_INF(dbg, n, "serial RX wire bytes");
	}
}

/* -------------------------------------------------------------------- */
/* ISR - RX only, one byte per uart_fifo_read() call. Registered by the */
/* rs-485 driver on the UNDERLYING uart, so `dev` here is that uart.     */
/* -------------------------------------------------------------------- */

static void rmc_serial_uart_isr(const struct device *dev, void *user_data)
{
	struct rmc_serial_uart *u = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t byte;

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

int rmc_serial_uart_init(struct rmc_serial_uart *u, const struct device *rs485,
			 rmc_serial_frame_cb_t frame_cb, void *user_data)
{
	if (u == NULL || rs485 == NULL || frame_cb == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(rs485)) {
		LOG_ERR("RS-485 device not ready");
		return -ENODEV;
	}

	memset(u, 0, sizeof(*u));
	u->rs485 = rs485;
	u->frame_cb = frame_cb;
	u->frame_cb_user_data = user_data;

	ring_buf_init(&u->rx_rb, sizeof(u->rx_rb_buf), u->rx_rb_buf);
	k_work_init(&u->rx_work, rmc_serial_uart_rx_work_handler);
	framer_reset(u);

	/* The rs-485 driver parks DE at the RX level, enables the underlying
	 * UART's RX interrupt, and installs our ISR on that UART.
	 */
	return rs485_callback_register(rs485, rmc_serial_uart_isr, u);
}

void rmc_serial_uart_deinit(struct rmc_serial_uart *u)
{
	if (u == NULL) {
		return;
	}

	/* The rs-485 driver exposes no RX-disable/unregister hook, so the UART
	 * RX IRQ + our ISR stay live and can re-arm rx_work after we cancel it.
	 * Clear frame_cb FIRST: framer_feed() is guarded on frame_cb != NULL, so
	 * any post-deinit frame is dropped and nothing reaches the app once this
	 * returns. (A full teardown would need an RX-disable hook on the rs-485
	 * driver; not required while no RMC_Deinit(SERIAL) caller exists.)
	 */
	u->frame_cb = NULL;
	k_work_cancel(&u->rx_work);
	framer_reset(u);
}

int rmc_serial_uart_send(struct rmc_serial_uart *u, const uint8_t *frame, size_t len)
{
	if (u == NULL || frame == NULL || len == 0) {
		return -EINVAL;
	}

	return rs485_transmit(u->rs485, frame, len);
}
