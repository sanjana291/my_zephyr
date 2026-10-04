/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal RMC backend contract.
 *
 * This is the abstraction rmc.c dispatches every public RMC_*() call
 * through, after RMC_Init() has selected a transport. It is private to
 * the rmc driver library - never included by the application, and never
 * included through <drivers/rmc.h>.
 *
 * Each backend (WiFi, SubGHz, ...) implements this vtable and exposes a
 * single `const struct rmc_backend_api *rmc_<name>_backend_get(void);`
 * accessor. Adding a new backend never requires changing rmc.c's logic,
 * only wiring the new accessor into RMC_Init()'s transport switch.
 */

#ifndef DRIVERS_RMC_BACKENDS_RMC_BACKEND_H_
#define DRIVERS_RMC_BACKENDS_RMC_BACKEND_H_

#include <zephyr/device.h>
#include "drivers/rmc.h"

struct rmc_backend_api {
	/**
	 * Initialize the backend. @p cfg is the transport-specific config
	 * passed by the application to RMC_Init() (e.g. struct
	 * rmc_wifi_init_cfg *), untouched by rmc.c. The backend reads its
	 * DT-derived HW resources from dev->config itself.
	 */
	int (*init)(const struct device *dev, void *cfg);

	/** Tear down the backend and release any resources it holds. */
	int (*deinit)(const struct device *dev);

	/**
	 * Queue application data for transmission. Must return -ENOTCONN
	 * if the backend has not reached its READY/CONNECTED state.
	 */
	int (*transmit)(const struct device *dev, uint8_t msg_type,
			 const uint8_t *msg, uint8_t msg_len);

	/** Store (or clear, if callback == NULL) the application's event callback. */
	int (*register_callback)(const struct device *dev, rmc_callback_t callback);
};

#endif /* DRIVERS_RMC_BACKENDS_RMC_BACKEND_H_ */
