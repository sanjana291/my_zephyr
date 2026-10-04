/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the RMC (Remote Message Communication) driver.
 *
 * RMC provides a transport-agnostic communication interface for the
 * application. The actual transport (WiFi via ESP32-C3 today, SubGHz FSK
 * in the future) is selected at RMC_Init() time and is never exposed to
 * the caller beyond that point - the application only ever talks to this
 * header.
 */

#ifndef INCLUDE_DRIVERS_RMC_H_
#define INCLUDE_DRIVERS_RMC_H_

#include <zephyr/device.h>
#include <zephyr/toolchain.h>
#include <zephyr/sys/__assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RMC communication backend (transport) selection.
 */
enum rmc_transport_type {
	RMC_TRANSPORT_WIFI = 0,
	RMC_TRANSPORT_SUBGHZ,
	RMC_TRANSPORT_SERIAL,
};

/**
 * @brief WiFi security type, matches the "STP" field of the RMC WiFi
 * Configuration packet.
 */
enum rmc_wifi_security {
	RMC_WIFI_SECURITY_NONE = 0,
	RMC_WIFI_SECURITY_PSK,
	RMC_WIFI_SECURITY_PSK_SHA256,
	RMC_WIFI_SECURITY_SAE,
	RMC_WIFI_SECURITY_WAPI,
	RMC_WIFI_SECURITY_EAP,
	RMC_WIFI_SECURITY_WEP,
	RMC_WIFI_SECURITY_WPA_PSK,
	RMC_WIFI_SECURITY_WPA_AUTO_PERSONAL,
};

enum rmc_subghz_channel {
	RMC_SUBGHZ_CHANNEL_1 = 0,
	RMC_SUBGHZ_CHANNEL_2,
	RMC_SUBGHZ_CHANNEL_3,
	RMC_SUBGHZ_CHANNEL_4,
	RMC_SUBGHZ_CHANNEL_5,
};

/**
 * @brief IP address assignment type, matches the "ATP" field of the RMC
 * WiFi Configuration packet.
 */
enum rmc_wifi_addr_type {
	RMC_WIFI_ADDR_DHCP = 0,
	RMC_WIFI_ADDR_STATIC,
};

typedef enum rmc_subghz_channel rmc_subghz_channel_t;

/**
 * @brief WiFi Configuration packet payload.
 *
 * This is the exact information carried over the wire to the ESP32-C3 in
 * the RMC "WiFi Configuration" packet (MTP 02).
 */
struct rmc_wifi_cfg {
	char ssid[33];			/* 32 chars max + NUL */
	char password[65];		/* 64 chars max + NUL */
	enum rmc_wifi_security security_type;
	uint8_t connect_timeout_s;	/* "TOT": ESP32-side AP connect timeout, <= 60s */
	enum rmc_wifi_addr_type addr_type;
	char ip_addr[16];		/* used only when addr_type == STATIC */
	char subnet_mask[16];
	char gateway_addr[16];
};

/**
 * @brief Driver-level initialization configuration for the WiFi backend.
 *
 * Superset of struct rmc_wifi_cfg: also carries the two independent
 * timeout/retry policies that are local to the i.MXRT side and are never
 * sent over the wire.
 */
struct rmc_wifi_init_cfg {
	struct rmc_wifi_cfg wifi;

	/** Timeout domain #1: ACK/NACK waits, WiFi/TCP connection retries. */
	uint32_t ack_retry_timeout_ms;
	uint8_t ack_retry_count;

	/** Timeout domain #2: heartbeat monitoring, independent of the above. */
	uint32_t heartbeat_timeout_ms;
};

struct rmc_subghz_cfg {
	uint8_t network_id[8];
	rmc_subghz_channel_t channel_id;
	uint8_t shuttle_addr;
	uint8_t remote_addr;
};

struct rmc_subghz_init_cfg {
	struct rmc_subghz_cfg sbg;
	uint16_t fw_ver;
	uint16_t hw_ver;
};

/**
 * @brief Initialization configuration for the Serial (RS-485) backend.
 *
 * The transport hardware (RS-485 device + underlying UART/baud) comes entirely
 * from Devicetree (the "serial-uart" phandle on the RMC node). This struct
 * carries no credentials; a caller passes it (non-NULL) to RMC_Init() only to
 * satisfy the common non-NULL cfg contract. The field is reserved for future
 * serial tunables.
 */
struct rmc_serial_init_cfg {
	uint32_t reserved;
};


/**
 * @brief RMC asynchronous event types, delivered via rmc_callback_t.
 *
 * Each event type has its own event-code enum below, so new codes can be
 * added in the future without changing this callback interface. These
 * names are intentionally transport-agnostic: the application never needs
 * to know whether the underlying link is WiFi, SubGHz, or anything else.
 */
enum rmc_event_type {
	RMC_EVENT_WIRELESS_CONN = 0,
	RMC_EVENT_REMOTE_CONN,
	RMC_EVENT_RX_DATA,
	RMC_EVENT_ERROR,
	RMC_EVENT_HEARTBEAT,
	RMC_EVENT_PAIRING,
};

/** Event codes for RMC_EVENT_WIRELESS_CONN. */
enum rmc_wireless_conn_event {
	RMC_WIRELESS_CONN_CONNECTED = 0,
	RMC_WIRELESS_CONN_DISCONNECTED,
	RMC_WIRELESS_CONN_FAILED,
	RMC_WIRELESS_CONN_TIMEOUT,
};

/** Event codes for RMC_EVENT_REMOTE_CONN. */
enum rmc_remote_conn_event {
	RMC_REMOTE_CONN_CONNECTED = 0,
	RMC_REMOTE_CONN_DISCONNECTED,
	RMC_REMOTE_CONN_TIMEOUT,
};

/** Event codes for RMC_EVENT_RX_DATA. */
enum rmc_rx_data_event {
	RMC_RX_DATA_RECEIVED = 0,
};

/** Event codes for RMC_EVENT_HEARTBEAT. */
enum rmc_heartbeat_event {
	RMC_HEARTBEAT_RECEIVED = 0,
	RMC_HEARTBEAT_TIMEOUT,
};

/** Event codes for RMC_EVENT_ERROR. */
enum rmc_error_event {
	RMC_ERR_INVALID_PACKET = 0,
	RMC_ERR_COMM,
	RMC_ERR_RETRY_LIMIT,
	RMC_ERR_UNKNOWN,
	RMC_ERR_CMD_REJECTED,
};

/**Event codes for RMC_EVENT_PAIRING */
enum rmc_pairing_events {
	RMC_PAIRING_REQUEST = 0,
	RMC_PAIRING_SUCCESS,
	RMC_PAIRING_FAILED_TIMEOUT,
	RMC_PAIRING_FAILED_RDBACK_STATUS,
	RMC_PAIRING_FAILED_AUTHENTICATION,
	RMC_PAIRING_SCANING_EXTD,
};

/**
 * @brief RMC module return type.
 *
 * All four public RMC_*() functions return rmc_status_t. The values are
 * transport-agnostic; the application never sees a raw Zephyr errno.
 * rmc.c maps internal backend errors to these codes before returning.
 */
typedef uint8_t rmc_status_t;

#define RMC_OK               ((rmc_status_t)0)
#define RMC_ERR_INVALID_ARG  ((rmc_status_t)1) /* bad argument passed to API */
#define RMC_ERR_NOT_INIT     ((rmc_status_t)2) /* RMC_Init() not called / failed */
#define RMC_ERR_ALREADY_INIT ((rmc_status_t)3) /* already initialized */
#define RMC_ERR_NOT_CONN     ((rmc_status_t)4) /* link not ready for TX */
#define RMC_ERR_BUSY         ((rmc_status_t)5) /* TX queue full, retry */
#define RMC_ERR_NOT_SUPP     ((rmc_status_t)6) /* backend not implemented */
#define RMC_ERR_HW           ((rmc_status_t)7) /* peripheral / GPIO deinit/init failed */
#define RMC_ERR_GENERAL      ((rmc_status_t)8) /* catch-all */

/**
 * @brief RMC event callback.
 *
 * Invoked asynchronously from RMC's internal context whenever a
 * connection, data, error, or heartbeat event occurs.
 *
 * @p rmc_msg layout:
 *   - rmc_msg[0]        : event code (one of the rmc_*_event enums that
 *                         corresponds to evnt_typ, e.g.
 *                         enum rmc_wireless_conn_event for
 *                         RMC_EVENT_WIRELESS_CONN). Always present
 *                         (rmc_msg_len >= 1).
 *   - rmc_msg[1..]      : optional payload bytes (e.g. RX data for
 *                         RMC_EVENT_RX_DATA). Present only when
 *                         rmc_msg_len > 1.
 *
 * @p rmc_msg is only valid for the duration of the call; copy it if it
 * needs to outlive the callback.
 *
 * @param dev         RMC device the event originated from.
 * @param evnt_typ    One of enum rmc_event_type.
 * @param rmc_msg     Buffer: [0] = event code, [1..] = optional payload.
 * @param rmc_msg_len Total length of @p rmc_msg (always >= 1).
 */
typedef void (*rmc_callback_t)(const struct device *dev,
				uint8_t evnt_typ,
				uint8_t *rmc_msg,
				uint8_t rmc_msg_len);

__subsystem struct rmc_driver_api {
	rmc_status_t (*init)(const struct device *dev,
			      enum rmc_transport_type transport, void *cfg);
	rmc_status_t (*deinit)(const struct device *dev);
	rmc_status_t (*transmit)(const struct device *dev, uint8_t msg_type,
				  const uint8_t *msg, uint8_t msg_len);
	rmc_status_t (*register_callback)(const struct device *dev,
					   rmc_callback_t callback);
};

/**
 * @brief Initialize RMC and select its communication backend.
 *
 * Selects and initializes the backend given by @p transport, using
 * @p cfg (struct rmc_wifi_init_cfg * for RMC_TRANSPORT_WIFI, struct
 * rmc_subghz_init_cfg * for RMC_TRANSPORT_SUBGHZ). All later RMC_*()
 * calls on @p dev are dispatched to this backend automatically.
 *
 * @retval RMC_OK            on success.
 * @retval RMC_ERR_INVALID_ARG invalid transport or NULL cfg.
 * @retval RMC_ERR_ALREADY_INIT already initialized; call RMC_Deinit() first.
 * @retval RMC_ERR_NOT_SUPP  backend not enabled / not implemented.
 * @retval RMC_ERR_HW        peripheral or GPIO initialization failed.
 */
__syscall rmc_status_t RMC_Init(const struct device *dev,
				 enum rmc_transport_type transport,
				 void *cfg);

static inline rmc_status_t z_impl_RMC_Init(const struct device *dev,
					     enum rmc_transport_type transport,
					     void *cfg)
{
	const struct rmc_driver_api *api =
		(const struct rmc_driver_api *)dev->api;

	__ASSERT_NO_MSG(api->init != NULL);

	return api->init(dev, transport, cfg);
}

/**
 * @brief Deinitialize RMC and release the active backend.
 *
 * @retval RMC_OK            on success.
 * @retval RMC_ERR_NOT_INIT  not currently initialized.
 */
__syscall rmc_status_t RMC_Deinit(const struct device *dev);

static inline rmc_status_t z_impl_RMC_Deinit(const struct device *dev)
{
	const struct rmc_driver_api *api =
		(const struct rmc_driver_api *)dev->api;

	__ASSERT_NO_MSG(api->deinit != NULL);

	return api->deinit(dev);
}

/**
 * @brief Transmit application data over the active RMC backend.
 *
 * Only succeeds once the backend has reached its READY/CONNECTED state.
 * The application never needs to check connection state itself.
 *
 * @retval RMC_OK            data queued for transmission.
 * @retval RMC_ERR_NOT_INIT  RMC_Init() not called or failed.
 * @retval RMC_ERR_NOT_CONN  backend not connected/ready yet.
 * @retval RMC_ERR_BUSY      TX queue full; retry later.
 * @retval RMC_ERR_INVALID_ARG NULL msg or msg_len out of range.
 */
__syscall rmc_status_t RMC_Transmit(const struct device *dev,
				     uint8_t msg_type,
				     const uint8_t *msg,
				     uint8_t msg_len);

static inline rmc_status_t z_impl_RMC_Transmit(const struct device *dev,
						 uint8_t msg_type,
						 const uint8_t *msg,
						 uint8_t msg_len)
{
	const struct rmc_driver_api *api =
		(const struct rmc_driver_api *)dev->api;

	__ASSERT_NO_MSG(api->transmit != NULL);

	return api->transmit(dev, msg_type, msg, msg_len);
}

/**
 * @brief Register the application's RMC event callback.
 *
 * Only one callback is supported per device; registering again replaces
 * the previous one. Pass NULL to unregister.
 *
 * @retval RMC_OK always.
 */
__syscall rmc_status_t RMC_RegisterCallback(const struct device *dev,
					     rmc_callback_t callback);

static inline rmc_status_t z_impl_RMC_RegisterCallback(const struct device *dev,
							 rmc_callback_t callback)
{
	const struct rmc_driver_api *api =
		(const struct rmc_driver_api *)dev->api;

	__ASSERT_NO_MSG(api->register_callback != NULL);

	return api->register_callback(dev, callback);
}

#include <syscalls/rmc.h>

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_DRIVERS_RMC_H_ */