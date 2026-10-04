/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC driver: public API dispatch layer.
 *
 * This file implements struct rmc_driver_api only. It has zero
 * knowledge of WiFi or SubGHz protocol details - every call is
 * validated here (state, arguments) and then forwarded to whichever
 * backend was selected by the most recent RMC_Init().
 *
 * Internal errno values from the backends are mapped to rmc_status_t
 * here, so the application never sees a raw Zephyr errno.
 */

#define DT_DRV_COMPAT calixto_rmc

#include "drivers/rmc.h"
#include "rmc_internal.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(rmc, CONFIG_RMC_LOG_LEVEL);

/* -------------------------------------------------------------------- */
/* errno -> rmc_status_t mapping (stays fully private to rmc.c)        */
/* -------------------------------------------------------------------- */

static rmc_status_t errno_to_rmc(int err)
{
	switch (err) {
	case 0:        return RMC_OK;
	case -EINVAL:  return RMC_ERR_INVALID_ARG;
	case -ENODEV:  return RMC_ERR_HW;
	case -EALREADY:return RMC_ERR_ALREADY_INIT;
	case -ENOTSUP: return RMC_ERR_NOT_SUPP;
	case -ENOMEM:  return RMC_ERR_NOT_SUPP;
	case -ENOTCONN:return RMC_ERR_NOT_CONN;
	case -EBUSY:   return RMC_ERR_BUSY;
	case -ENOSYS:  return RMC_ERR_NOT_SUPP;
	default:       return RMC_ERR_GENERAL;
	}
}

/* -------------------------------------------------------------------- */
/* Backend selection                                                    */
/* -------------------------------------------------------------------- */

static const struct rmc_backend_api *rmc_get_backend(enum rmc_transport_type transport)
{
	switch (transport) {
	case RMC_TRANSPORT_WIFI:
#if defined(CONFIG_RMC_WIFI_BACKEND)
		return rmc_wifi_backend_get();
#else
		return NULL;
#endif
	case RMC_TRANSPORT_SUBGHZ:
#if defined(CONFIG_RMC_SUBGHZ_BACKEND)
		return rmc_subghz_backend_get();
#else
		return NULL;
#endif
	case RMC_TRANSPORT_SERIAL:
#if defined(CONFIG_RMC_SERIAL_BACKEND)
		return rmc_serial_backend_get();
#else
		return NULL;
#endif
	default:
		return NULL;
	}
}

/* -------------------------------------------------------------------- */
/* Driver API implementation                                            */
/* -------------------------------------------------------------------- */

static rmc_status_t rmc_api_init(const struct device *dev,
				  enum rmc_transport_type transport, void *cfg)
{
	struct rmc_data *data = dev->data;
	const struct rmc_backend_api *backend;
	int ret;

	if (cfg == NULL) {
		return RMC_ERR_INVALID_ARG;
	}

	backend = rmc_get_backend(transport);
	if (backend == NULL) {
		LOG_ERR("Transport %d not available (backend disabled or unimplemented)",
			transport);
		return RMC_ERR_NOT_SUPP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->initialized) {
		LOG_ERR("RMC already initialized; call RMC_Deinit() first");
		k_mutex_unlock(&data->lock);
		return RMC_ERR_ALREADY_INIT;
	}

	ret = backend->init(dev, cfg);
	if (ret == 0) {
		data->backend = backend;
		data->transport = transport;
		data->initialized = true;
	}

	k_mutex_unlock(&data->lock);
	return errno_to_rmc(ret);
}

static rmc_status_t rmc_api_deinit(const struct device *dev)
{
	struct rmc_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->initialized) {
		k_mutex_unlock(&data->lock);
		return RMC_ERR_NOT_INIT;
	}

	ret = data->backend->deinit(dev);
	data->initialized = false;
	data->backend = NULL;

	k_mutex_unlock(&data->lock);
	return errno_to_rmc(ret);
}

static rmc_status_t rmc_api_transmit(const struct device *dev, uint8_t msg_type,
				      const uint8_t *msg, uint8_t msg_len)
{
	struct rmc_data *data = dev->data;

	if (!data->initialized) {
		return RMC_ERR_NOT_INIT;
	}

	/* Deliberately unlocked: transmit is on the hot path and must not
	 * block behind init/deinit. The backend owns its own thread-safety.
	 */
	return errno_to_rmc(data->backend->transmit(dev, msg_type, msg, msg_len));
}

static rmc_status_t rmc_api_register_callback(const struct device *dev,
					       rmc_callback_t callback)
{
	struct rmc_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	data->callback = callback;

	if (data->initialized) {
		ret = data->backend->register_callback(dev, callback);
	} else {
		/* Allow registering before RMC_Init(); the backend will be
		 * told about the callback once one is selected.
		 */
		ret = 0;
	}

	k_mutex_unlock(&data->lock);
	return errno_to_rmc(ret);
}

static const struct rmc_driver_api rmc_driver_api = {
	.init              = rmc_api_init,
	.deinit            = rmc_api_deinit,
	.transmit          = rmc_api_transmit,
	.register_callback = rmc_api_register_callback,
};

static int rmc_dev_init(const struct device *dev)
{
	struct rmc_data *data = dev->data;

	k_mutex_init(&data->lock);
	data->backend     = NULL;
	data->initialized = false;
	data->callback    = NULL;

	return 0;
}

/* -------------------------------------------------------------------- */
/* Devicetree instantiation                                             */
/* -------------------------------------------------------------------- */

#define RMC_WIFI_HW_INIT(inst)                                                \
	.has_wifi = true,                                                     \
	.wifi_hw = {                                                          \
		.uart    = DEVICE_DT_GET(DT_INST_PHANDLE(inst, wifi_uart)),  \
		.chip_en = GPIO_DT_SPEC_INST_GET(inst, wifi_enable_gpios),   \
	},

#define RMC_SUBGHZ_HW_INIT(inst)                                             \
	.has_subghz = true,                                                  \
	.subghz_hw = {                                                       \
		.radio = DEVICE_DT_GET(DT_INST_PHANDLE(inst, subghz)),       \
	},

#define RMC_SERIAL_HW_INIT(inst)                                             \
	.has_serial = true,                                                  \
	.serial_hw = {                                                       \
		.rs485 = DEVICE_DT_GET(DT_INST_PHANDLE(inst, serial_uart)),  \
	},

#define RMC_INIT(inst)                                                       \
	static const struct rmc_config rmc_config_##inst = {                 \
		IF_ENABLED(CONFIG_RMC_WIFI_BACKEND,                           \
			(COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, wifi_uart),  \
				     (RMC_WIFI_HW_INIT(inst)), ())))          \
		IF_ENABLED(CONFIG_RMC_SUBGHZ_BACKEND,                         \
			(COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, subghz),     \
				     (RMC_SUBGHZ_HW_INIT(inst)), ())))        \
		IF_ENABLED(CONFIG_RMC_SERIAL_BACKEND,                         \
			(COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, serial_uart),\
				     (RMC_SERIAL_HW_INIT(inst)), ())))        \
	};                                                                    \
	static struct rmc_data rmc_data_##inst;                              \
	DEVICE_DT_INST_DEFINE(inst, rmc_dev_init, NULL,                      \
			       &rmc_data_##inst, &rmc_config_##inst,          \
			       POST_KERNEL, CONFIG_RMC_INIT_PRIORITY,         \
			       &rmc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RMC_INIT)
