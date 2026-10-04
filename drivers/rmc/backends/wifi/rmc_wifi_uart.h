/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC WiFi UART layer: polling TX, interrupt-driven RX, JSON framing.
 *
 * TX: polling mode (uart_poll_out()), one byte at a time. Simple and
 * robust for this link's small, infrequent packets; no TX ring buffer,
 * no TX interrupt.
 *
 * RX: interrupt-driven. The ISR reads exactly one byte per
 * uart_fifo_read() call (looping until the FIFO is drained), never a
 * bulk multi-byte read, and pushes each byte into a ring buffer. All
 * framing work happens afterwards in a system workqueue job, never in
 * interrupt context.
 *
 * Framing: a packet is valid only if it starts with '{' and ends with
 * the matching '}' (brace-depth and JSON-string aware, so braces inside
 * quoted payload text don't break framing). Bytes outside a `{...}`
 * span (stray noise, partial packets still arriving, back-to-back
 * packets) are handled correctly: anything before the next '{' is
 * discarded, and each complete object is delivered to the frame
 * callback the instant its closing '}' is seen, so multiple packets
 * queued back-to-back in the ring buffer are each extracted in turn.
 */

#ifndef DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_UART_H_
#define DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_UART_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/** Max size of one JSON frame this layer will assemble (DAT payload + JSON overhead). */
#define RMC_WIFI_UART_MAX_FRAME_LEN 320

/** RX ring buffer between the ISR and the framing work item. */
#define RMC_WIFI_UART_RX_RB_SIZE 512

/**
 * @brief Called (from system workqueue context) once a complete `{...}`
 * JSON frame has been extracted from the UART byte stream.
 *
 * @param frame NUL-terminated JSON object. Valid only for the duration
 *              of the call - the framer reuses this buffer immediately after.
 * @param len Length of @p frame, excluding the NUL.
 * @param user_data As passed to rmc_wifi_uart_init().
 */
typedef void (*rmc_wifi_frame_cb_t)(uint8_t *frame, size_t len, void *user_data);

struct rmc_wifi_uart {
	const struct device *dev;

	struct ring_buf rx_rb;
	uint8_t rx_rb_buf[RMC_WIFI_UART_RX_RB_SIZE];

	struct k_work rx_work;

	/* Framer state (system workqueue context only). */
	uint8_t frame_buf[RMC_WIFI_UART_MAX_FRAME_LEN];
	size_t frame_len;
	int brace_depth;
	bool in_string;
	bool escape_next;
	bool frame_active;
	uint32_t frame_overflow_count;

	rmc_wifi_frame_cb_t frame_cb;
	void *frame_cb_user_data;
};

/**
 * @brief Initialize and start the UART layer.
 *
 * Configures @p uart_dev for interrupt-driven RX and enables RX
 * interrupts; TX uses polling mode and needs no setup here. @p u must
 * remain valid for as long as the UART is in use (typically backend-
 * instance-lifetime storage, not stack-allocated).
 */
int rmc_wifi_uart_init(struct rmc_wifi_uart *u, const struct device *uart_dev,
			rmc_wifi_frame_cb_t frame_cb, void *user_data);

/** Disable RX interrupts and release the UART. Safe to call after init failure. */
void rmc_wifi_uart_deinit(struct rmc_wifi_uart *u);

/**
 * @brief Send a JSON frame via polling TX.
 *
 * Blocks until every byte has been handed to the UART (uart_poll_out()
 * is inherently synchronous per byte), so @p timeout is currently
 * unused - kept in the signature so callers and call sites don't need
 * to change if a future revision reintroduces a non-blocking path.
 *
 * @retval 0 on success.
 * @retval -EINVAL invalid arguments.
 */
int rmc_wifi_uart_send(struct rmc_wifi_uart *u, const char *json, size_t len, k_timeout_t timeout);

#endif /* DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_UART_H_ */
