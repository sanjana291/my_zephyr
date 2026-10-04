/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_H_
#define DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include "../rmc_backend.h"

/**
 * @brief Devicetree-derived hardware resources for the WiFi (ESP32-C3) backend.
 *
 * Populated by rmc.c from the "wifi-uart" / "wifi-enable-gpios" DT
 * properties and stored in the parent rmc_config; the backend reads it
 * back out of dev->config at init time.
 */
struct rmc_wifi_hw {
	const struct device *uart;
	struct gpio_dt_spec chip_en;
};

/**
 * @brief Get the WiFi backend's rmc_backend_api implementation.
 *
 * Returns NULL if CONFIG_RMC_WIFI_BACKEND is disabled.
 */
const struct rmc_backend_api *rmc_wifi_backend_get(void);

#endif /* DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_H_ */
