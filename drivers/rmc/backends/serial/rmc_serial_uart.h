/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC Serial UART layer: RS-485 TX, interrupt-driven RX, brace framing.
 *
 * TX: hands the whole frame to the Calixto rs-485 driver (rs485_transmit()),
 * which asserts DE, drains the bytes, and releases DE. Blocking, but only ever
 * called from a thread (the P4 outbound worker), never an ISR.
 *
 * RX: the rs-485 driver registers our ISR on the underlying UART and enables
 * RX. The ISR reads one byte per uart_fifo_read() call into a ring buffer;
 * framing happens afterwards in a system workqueue job, never in the ISR.
 *
 * Framing: a frame is the raw application payload wrapped as { <payload> }
 * (0x7B .. 0x7D). Everything before a 0x7B is discarded; the bytes between the
 * braces are the payload delivered to the frame callback the instant the
 * closing 0x7D is seen. The 3-byte packets carried here never contain a 0x7B /
 * 0x7D byte, so no escaping is needed.
 */

#ifndef DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_UART_H_
#define DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_UART_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/** Frame delimiters: the application payload is wrapped as { <payload> }. */
#define RMC_SERIAL_FRAME_START 0x7Bu   /* '{' */
#define RMC_SERIAL_FRAME_END   0x7Du   /* '}' */

/** Max application payload this layer will assemble (between the braces). */
#define RMC_SERIAL_UART_MAX_FRAME_LEN CONFIG_RMC_SERIAL_DATA_MAX_LEN

/** RX ring buffer between the ISR and the framing work item. */
#define RMC_SERIAL_UART_RX_RB_SIZE CONFIG_RMC_SERIAL_RX_RING_SIZE

/**
 * @brief Called (from system workqueue context) once a complete
 * { <payload> } frame has been extracted. @p payload points at the raw
 * application bytes (braces stripped); valid only for the duration of the call.
 */
typedef void (*rmc_serial_frame_cb_t)(const uint8_t *payload, size_t len, void *user_data);

struct rmc_serial_uart {
	const struct device *rs485;	/* rs-485 device (owns DE + the UART) */

	struct ring_buf rx_rb;
	uint8_t rx_rb_buf[RMC_SERIAL_UART_RX_RB_SIZE];

	struct k_work rx_work;

	/* Framer state (system workqueue context only). */
	uint8_t frame_buf[RMC_SERIAL_UART_MAX_FRAME_LEN];
	size_t frame_len;
	bool frame_active;
	uint32_t frame_overflow_count;

	rmc_serial_frame_cb_t frame_cb;
	void *frame_cb_user_data;
};

/**
 * @brief Initialize the RX path and register the ISR via the rs-485 driver.
 *
 * @p u must remain valid for the life of the link (backend-instance storage,
 * not stack). Returns 0 on success, negative errno otherwise.
 */
int rmc_serial_uart_init(struct rmc_serial_uart *u, const struct device *rs485,
			 rmc_serial_frame_cb_t frame_cb, void *user_data);

/** Stop delivering frames (cancels the framing work item). */
void rmc_serial_uart_deinit(struct rmc_serial_uart *u);

/**
 * @brief Send one already-framed byte buffer over RS-485 (blocking).
 *
 * @retval 0 on success, negative errno otherwise.
 */
int rmc_serial_uart_send(struct rmc_serial_uart *u, const uint8_t *frame, size_t len);

#endif /* DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_UART_H_ */
