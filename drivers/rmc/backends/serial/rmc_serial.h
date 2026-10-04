/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC Serial (RS-485) backend.
 *
 * Carries the same application packets as the WiFi backend, but over a
 * half-duplex RS-485 link driven by the Calixto rs-485 driver (which owns
 * the DE line). There is no connection state machine: the link is READY the
 * instant RMC_Init() succeeds. Inbound frames are the raw application packet
 * wrapped as { <payload> }; the payload is delivered verbatim to the
 * application as an RMC_EVENT_RX_DATA event.
 */

#ifndef DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_H_
#define DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_H_

#include <zephyr/device.h>
#include "../rmc_backend.h"

/**
 * @brief Devicetree-derived hardware resources for the Serial backend.
 *
 * @p rs485 is the "calixto,rs-485" device from the RMC node's "serial-uart"
 * phandle; it owns the DE line and the underlying UART.
 */
struct rmc_serial_hw {
	const struct device *rs485;
};

/**
 * @brief Get the Serial backend's rmc_backend_api implementation.
 *
 * Only linked when CONFIG_RMC_SERIAL_BACKEND is enabled.
 */
const struct rmc_backend_api *rmc_serial_backend_get(void);

#endif /* DRIVERS_RMC_BACKENDS_SERIAL_RMC_SERIAL_H_ */
