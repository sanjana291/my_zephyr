/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Private RMC driver internals.
 *
 * struct rmc_config and struct rmc_data are the DT-derived resource
 * bundle and runtime state for one RMC device instance. rmc.c populates
 * struct rmc_config from Devicetree at build time; each backend reads
 * its own HW sub-struct back out of dev->config at init time. This
 * header is never included by the application (see <drivers/rmc.h> for
 * the public API).
 */

#ifndef DRIVERS_RMC_RMC_INTERNAL_H_
#define DRIVERS_RMC_RMC_INTERNAL_H_

#include <zephyr/kernel.h>
#include "drivers/rmc.h"
#include "backends/rmc_backend.h"
#include "backends/wifi/rmc_wifi.h"
#include "backends/subghz/rmc_subghz.h"
#include "backends/serial/rmc_serial.h"

/** Devicetree-derived resources for one RMC device instance. */
struct rmc_config {
	bool has_wifi;
	struct rmc_wifi_hw wifi_hw;	/* valid only when has_wifi */

	bool has_subghz;
	struct rmc_subghz_hw subghz_hw;	/* valid only when has_subghz */

	bool has_serial;
	struct rmc_serial_hw serial_hw;	/* valid only when has_serial */
};

/** Runtime state for one RMC device instance. */
struct rmc_data {
	struct k_mutex lock;

	/** Backend selected by the most recent successful RMC_Init(), NULL if none. */
	const struct rmc_backend_api *backend;
	enum rmc_transport_type transport;
	bool initialized;

	/** Backend-private state, opaque to rmc.c, set by backend->init(). */
	void *backend_data;

	rmc_callback_t callback;
};

#endif /* DRIVERS_RMC_RMC_INTERNAL_H_ */
