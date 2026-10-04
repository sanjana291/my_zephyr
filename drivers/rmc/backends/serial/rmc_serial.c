/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC Serial (RS-485) backend: no connection state, READY on init.
 *
 * The simplest backend: bring up the RX framer over the rs-485 driver, mark
 * the link READY, and from then on forward every received { <payload> } frame
 * to the application as RMC_EVENT_RX_DATA and frame every outbound payload the
 * same way. No CHIP_EN, no config handshake, no heartbeat, no retries — the
 * end-to-end ACK/NACK is an application-level concern (P1/P4), not this layer.
 */

#include "rmc_serial.h"
#include "rmc_serial_uart.h"
#include "../../rmc_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(rmc_serial, CONFIG_RMC_LOG_LEVEL);

/* Max application payload carried in one frame. */
#define RMC_SERIAL_DATA_MAX_LEN   CONFIG_RMC_SERIAL_DATA_MAX_LEN

/* Notify buffer: 1 (event code) + payload + 1 guard. */
#define RMC_SERIAL_NOTIFY_BUF_SIZE (1U + RMC_SERIAL_DATA_MAX_LEN + 1U)

/* TX frame buffer: 0x7B + payload + 0x7D. */
#define RMC_SERIAL_TX_BUF_SIZE     (RMC_SERIAL_DATA_MAX_LEN + 2U)

/** Backend-private runtime state. One active Serial link (one RS-485 bus). */
struct rmc_serial_data {
	const struct device *dev;
	const struct rmc_serial_hw *hw;
	struct rmc_serial_uart uart;
	atomic_t ready;
};

static struct rmc_serial_data rmc_serial_singleton;
static bool rmc_serial_singleton_used;

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
/* -------------------------------------------------------------------- */

static void rmc_serial_notify(const struct device *dev, uint8_t event_type, uint8_t event_code,
			      const uint8_t *payload, uint16_t payload_len)
{
	struct rmc_data *rdata = dev->data;
	rmc_callback_t cb = rdata->callback;
	uint8_t buf[RMC_SERIAL_NOTIFY_BUF_SIZE];
	uint8_t total_len;

	if (cb == NULL) {
		return;
	}

	if (payload_len > RMC_SERIAL_DATA_MAX_LEN) {
		payload_len = RMC_SERIAL_DATA_MAX_LEN;
	}

	buf[0] = event_code;
	if (payload != NULL && payload_len > 0) {
		memcpy(&buf[1], payload, payload_len);
	}
	total_len = (uint8_t)(1U + payload_len);

	cb(dev, event_type, buf, total_len);
}

/** Framer callback (system workqueue context): deliver the raw payload up. */
static void rmc_serial_on_frame(const uint8_t *payload, size_t len, void *user_data)
{
	struct rmc_serial_data *d = user_data;

	if (len == 0) {
		return;
	}
	rmc_serial_notify(d->dev, RMC_EVENT_RX_DATA, RMC_RX_DATA_RECEIVED,
			  payload, (uint16_t)len);
}

/* -------------------------------------------------------------------- */
/* Backend API                                                           */
/* -------------------------------------------------------------------- */

static int rmc_serial_init(const struct device *dev, void *cfg)
{
	struct rmc_data *rdata = dev->data;
	const struct rmc_config *rconfig = dev->config;
	struct rmc_serial_data *d;
	int ret;

	ARG_UNUSED(cfg);   /* Serial needs no app config; HW is from Devicetree. */

	if (!rconfig->has_serial) {
		LOG_ERR("Devicetree node has no Serial resource (serial-uart)");
		return -ENODEV;
	}
	if (rmc_serial_singleton_used) {
		LOG_ERR("RMC Serial backend supports only one active instance");
		return -ENOMEM;
	}

	d = &rmc_serial_singleton;
	memset(d, 0, sizeof(*d));
	d->dev = dev;
	d->hw = &rconfig->serial_hw;

	ret = rmc_serial_uart_init(&d->uart, rconfig->serial_hw.rs485,
				   rmc_serial_on_frame, d);
	if (ret != 0) {
		LOG_ERR("Failed to init Serial UART layer: %d", ret);
		return ret;
	}

	rdata->backend_data = d;
	rmc_serial_singleton_used = true;

	/* No connection phase: the link is usable immediately. */
	atomic_set(&d->ready, 1);
	LOG_INF("RMC Serial backend up (RS-485)");
	return 0;
}

static int rmc_serial_deinit(const struct device *dev)
{
	struct rmc_data *rdata = dev->data;
	struct rmc_serial_data *d = rdata->backend_data;

	if (d == NULL) {
		return -EALREADY;
	}

	atomic_set(&d->ready, 0);
	rmc_serial_uart_deinit(&d->uart);

	rdata->backend_data = NULL;
	rmc_serial_singleton_used = false;
	return 0;
}

static int rmc_serial_transmit(const struct device *dev, uint8_t msg_type,
			       const uint8_t *msg, uint8_t msg_len)
{
	struct rmc_data *rdata = dev->data;
	struct rmc_serial_data *d = rdata->backend_data;
	uint8_t buf[RMC_SERIAL_TX_BUF_SIZE];

	/* msg_type is the application MTP (already inside the payload); the
	 * framing is identical regardless, so it is accepted but not used.
	 */
	ARG_UNUSED(msg_type);

	if (d == NULL) {
		return -ENODEV;
	}
	if (msg == NULL || msg_len == 0 || msg_len > RMC_SERIAL_DATA_MAX_LEN) {
		return -EINVAL;
	}
	if (!atomic_get(&d->ready)) {
		return -ENOTCONN;
	}

	buf[0] = RMC_SERIAL_FRAME_START;
	memcpy(&buf[1], msg, msg_len);
	buf[1U + msg_len] = RMC_SERIAL_FRAME_END;

	return rmc_serial_uart_send(&d->uart, buf, (size_t)msg_len + 2U);
}

static int rmc_serial_register_callback(const struct device *dev, rmc_callback_t callback)
{
	/* rmc.c stores the callback on struct rmc_data; rmc_serial_notify()
	 * reads it live. Nothing to do here.
	 */
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);
	return 0;
}

static const struct rmc_backend_api rmc_serial_api = {
	.init = rmc_serial_init,
	.deinit = rmc_serial_deinit,
	.transmit = rmc_serial_transmit,
	.register_callback = rmc_serial_register_callback,
};

const struct rmc_backend_api *rmc_serial_backend_get(void)
{
	return &rmc_serial_api;
}
