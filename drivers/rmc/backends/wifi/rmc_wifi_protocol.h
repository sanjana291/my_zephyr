/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC WiFi packet codec (MTP 01-07), per the RMC packet format spec.
 *
 * All wire-level JSON values are strings, including numeric-looking ones
 * (e.g. "TOT":"20"). Encoding/decoding is done with Zephyr's JSON
 * library (zephyr/data/json.h); decoded string fields point into the
 * caller-owned frame buffer and are only valid until that buffer is
 * reused, so callers of rmc_wifi_protocol_decode() must consume
 * out->rx_data.data (the only field with lifetime concerns) before the
 * frame buffer is overwritten.
 */

#ifndef DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_PROTOCOL_H_
#define DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "drivers/rmc.h"

/** Max "DAT" payload per the packet format spec (TX/RX Data). */
#define RMC_WIFI_DATA_MAX_LEN CONFIG_RMC_WIFI_DATA_MAX_LEN

/** RMC WiFi protocol message types ("MTP" field). */
enum rmc_wifi_mtp {
	RMC_WIFI_MTP_ACK_NACK = 1,
	RMC_WIFI_MTP_WIFI_CFG = 2,
	RMC_WIFI_MTP_WIFI_STATUS = 3,
	RMC_WIFI_MTP_TCP_STATUS = 4,
	RMC_WIFI_MTP_HEARTBEAT = 5,
	RMC_WIFI_MTP_TX_DATA = 6,
	RMC_WIFI_MTP_RX_DATA = 7,
};

/** Decoded representation of any inbound (ESP32-C3 -> i.MXRT) packet. */
struct rmc_wifi_rx_packet {
	enum rmc_wifi_mtp mtp;
	union {
		/** Valid when mtp == RMC_WIFI_MTP_ACK_NACK. */
		struct {
			bool nack;
		} ack;
		/** Valid when mtp == RMC_WIFI_MTP_WIFI_STATUS. */
		struct {
			bool connected;
		} wifi_status;
		/** Valid when mtp == RMC_WIFI_MTP_TCP_STATUS. */
		struct {
			bool connected;
		} tcp_status;
		/** Valid when mtp == RMC_WIFI_MTP_HEARTBEAT. */
		struct {
			bool wifi_connected;
			bool tcp_connected;
		} heartbeat;
		/** Valid when mtp == RMC_WIFI_MTP_RX_DATA. */
		struct {
			uint8_t data[RMC_WIFI_DATA_MAX_LEN + 1];
			uint8_t len;
		} rx_data;
	};
};

/**
 * @brief Decode one complete JSON frame (as produced by the UART framer)
 * into @p out.
 *
 * @param frame Mutable buffer containing exactly one JSON object,
 *              NUL-terminated. Modified in place by the JSON parser.
 * @param frame_len Length of the JSON object, excluding the NUL.
 * @param out Decoded packet.
 *
 * @retval 0 on success.
 * @retval -EBADMSG malformed JSON.
 * @retval -ENOMSG unrecognized or missing "MTP" field.
 * @retval -ENOTSUP recognized but unexpected MTP for an inbound packet
 *                  (e.g. MTP 02/06, which this device only ever sends).
 */
int rmc_wifi_protocol_decode(uint8_t *frame, size_t frame_len, struct rmc_wifi_rx_packet *out);

/**
 * @brief Encode an ACK or NACK packet (MTP 01).
 *
 * @retval 0 on success, negative errno on failure.
 */
int rmc_wifi_protocol_encode_ack(bool nack, char *buf, size_t buf_size);

/**
 * @brief Encode a WiFi Configuration packet (MTP 02).
 *
 * @retval 0 on success, negative errno on failure.
 */
int rmc_wifi_protocol_encode_wifi_cfg(const struct rmc_wifi_cfg *cfg, char *buf, size_t buf_size);

/**
 * @brief Encode a TX Data packet (MTP 06).
 *
 * @p data must not contain embedded NUL bytes (it is treated as text and
 * JSON-string-escaped by the encoder).
 *
 * @retval 0 on success, negative errno on failure.
 */
int rmc_wifi_protocol_encode_tx_data(const uint8_t *data, uint8_t data_len,
				      char *buf, size_t buf_size);

#endif /* DRIVERS_RMC_BACKENDS_WIFI_RMC_WIFI_PROTOCOL_H_ */
