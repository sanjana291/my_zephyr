/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC Sub-GHz protocol: packet types, frame layout, encode/decode.
 */

#ifndef DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_PROTOCOL_H_
#define DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------- */
/* Constants                                                             */
/* -------------------------------------------------------------------- */

#define RMC_SG_NOTIFY_BUF_SIZE CONFIG_RMC_SUBGHZ_DATA_MAX_LEN


#define RMC_SG_DEVICE_UID_LEN   8U
#define RMC_SG_REMOTE_UID_LEN	12U

/** Minimum header: PTP = 1 byte. */
#define RMC_SG_HDR_LEN          1U

#define RMC_SG_HDR_WITH_UID_LEN (RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN)

/** Maximum over-the-air frame length (fits in SX1278 FIFO). */
#define RMC_SG_MAX_FRAME_LEN    255U

/** Maximum application payload inside a Data Communication packet. */
#define RMC_SG_MAX_DATA_PLD     (RMC_SG_MAX_FRAME_LEN - 5U )


/* -------------------------------------------------------------------- */
/* Packet types (PTP field)                                              */
/* -------------------------------------------------------------------- */

enum rmc_sg_ptp {
	RMC_SG_PTP_DATA_COMM   = 0x01, /**< Application data communication */
	RMC_SG_PTP_ACK         = 0x02, /**< ACK / NACK                     */
	RMC_SG_PTP_BEACON      = 0x03, /**< Beacon (pairing broadcast)      */
	RMC_SG_PTP_AUTH_REQ    = 0x04, /**< Authentication request          */
	RMC_SG_PTP_AUTH_CONF   = 0x05, /**< Authentication confirmation     */
	RMC_SG_PTP_CFG_WRITE   = 0x06, /**< Configuration write             */
	RMC_SG_PTP_CFG_OK      = 0x07, /**< Configuration write success     */
	RMC_SG_PTP_CFG_READ    = 0x08, /**< Configuration read              */
	RMC_SG_PTP_CFG_READ_RSP= 0x09, /**< Configuration read response     */
	RMC_SG_CONNECT_REQ 	   = 0x0A, /**< Connect request     			*/
	RMC_SG_DISCONNECT_REQ  = 0x0B, /**< Disconnect request			    */
	RMC_SG_PTP_PAIRING_STS = 0x0C, /**< Pairing status (final ok/fail)  */
	RMC_SG_SCAN_RESPONSE   = 0x0D, /**< Scan Response				    */
};

/* -------------------------------------------------------------------- */
/* Pairing status codes (PLD of RMC_SG_PTP_PAIRING_STS)                 */
/* -------------------------------------------------------------------- */

/** Pairing completed successfully - safe to move to working mode. */
#define RMC_SG_PAIRING_STS_OK    0x00U

/** Pairing failed on the peer's side - discard the exchanged config. */
#define RMC_SG_PAIRING_STS_FAIL  0x01U

/* -------------------------------------------------------------------- */
/* Fixed authentication tokens (from protocol document)                  */
/* -------------------------------------------------------------------- */

/** 8-byte vendor token sent by RC in Auth Request PLD (ASCII "CALIXTO$"). */
#define RMC_SG_AUTH_REQ_TOKEN   { 0x43,0x41,0x4C,0x49,0x58,0x54,0x4F,0x24 }

/** 8-byte vendor token sent by Shuttle in Auth Confirmation PLD ("SYSTEMS$\0"). */
#define RMC_SG_AUTH_CONF_TOKEN  { 0x53,0x59,0x53,0x54,0x45,0x4D,0x53,0x24 }

/** Auth token length in bytes. */
#define RMC_SG_AUTH_TOKEN_LEN   8U

/* -------------------------------------------------------------------- */
/* Channel map (working mode channels 0–5, pairing on channel 6)        */
/* -------------------------------------------------------------------- */

#define RMC_SG_CHANNEL_COUNT       21U   /* 0–20 working, 6 = pairing  */
#define RMC_SG_PAIRING_CHANNEL     20U

/** Centre frequencies in Hz for channels 0–6 (do not calculate, use table). */
static const uint32_t rmc_sg_channel_freq_hz[RMC_SG_CHANNEL_COUNT] = {
	433091600U, /* channel 0 */
	433174800U, /* channel 1 */
	433258000U, /* channel 2 */
	433341200U, /* channel 3 */
	433424400U, /* channel 4 */
	433507600U, /* channel 5 */
	433590800U, /* channel 6 */
	433674000U, /* channel 7 */
	433757200U, /* channel 8 */
	433840400U, /* channel 9 */
	433923600U, /* channel 10 */
	434006800U, /* channel 11 */
	434090000U, /* channel 12 */
	434173200U, /* channel 13 */
	434256400U, /* channel 14 */
	434339600U, /* channel 15 */
	434422800U, /* channel 16 */
	434506000U, /* channel 17 */
	434589200U, /* channel 18 */
	434672400U, /* channel 19 */
	434755600U, /* channel 20 — pairing only*/
};

struct rmc_sg_frame_hdr {
	uint8_t ptp;
};

struct rmc_sg_pkt_ack{
	uint8_t ack_pkt;
};

struct rmc_sg_pkt_beacon {
	uint8_t device_uid[RMC_SG_DEVICE_UID_LEN];
};

struct rmc_sg_pkt_auth_req {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t shuttle_uid[RMC_SG_DEVICE_UID_LEN];
	uint8_t token[RMC_SG_AUTH_TOKEN_LEN];       /* remaining PLD bytes     */
};

struct rmc_sg_pkt_auth_conf {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t shuttle_uid[RMC_SG_DEVICE_UID_LEN];
	uint8_t token[RMC_SG_AUTH_TOKEN_LEN];
};

struct rmc_sg_pkt_cfg_write {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t shuttle_uid[RMC_SG_DEVICE_UID_LEN];
	uint8_t network_id[8];
	uint8_t channel_id;
	uint8_t node_id;
	uint8_t remote_id;
};

struct rmc_sg_pkt_cfg_read {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t shuttle_uid[RMC_SG_DEVICE_UID_LEN];
	uint8_t network_id[8];
	uint8_t channel_id;
	uint8_t node_id;
	uint8_t remote_id;
	uint16_t fw_ver;
	uint16_t hw_ver;
};

struct rmc_sg_pkt_cfg_ok {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t shuttle_uid[RMC_SG_DEVICE_UID_LEN];
	uint8_t status; /* 0x00 = success */
};

struct rmc_sg_pkt_pairing_sts {
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint8_t status; /* RMC_SG_PAIRING_STS_OK / RMC_SG_PAIRING_STS_FAIL */
};

struct rmc_sg_pkt_data {
	uint8_t msg[RMC_SG_MAX_DATA_PLD];
	uint8_t msg_len;
};

struct rmc_sg_pkt_scan_response {
	uint8_t device_uid[RMC_SG_DEVICE_UID_LEN];
};


/* -------------------------------------------------------------------- */
/* Encode / decode API                                                   */
/* -------------------------------------------------------------------- */

/**
 * @brief Encode a Beacon frame into @p buf.
 * @return number of bytes written, negative errno on error.
 */
int rmc_sg_encode_beacon(uint8_t *buf, size_t buf_size,
			  const uint8_t *device_uid);

/**
 * @brief Encode an Authentication Request frame.
 */
int rmc_sg_encode_auth_req(uint8_t *buf, size_t buf_size,
			    const uint8_t *our_uid,
			    const uint8_t *shuttle_uid);

/**
 * @brief Encode an Authentication Confirmation frame.
 */
int rmc_sg_encode_auth_conf(uint8_t *buf, size_t buf_size,
			     const uint8_t *our_uid);

/**
 * @brief Encode a Configuration Write frame.
 */
int rmc_sg_encode_cfg_write(uint8_t *buf, size_t buf_size,
			     const uint8_t *our_uid,
			     const struct rmc_sg_pkt_cfg_write *cfg);

/**
 * @brief Encode a Configuration Write Success frame.
 */
int rmc_sg_encode_cfg_ok(uint8_t *buf, size_t buf_size,
			  const uint8_t *our_uid,
			  uint8_t status);

/**
 * @brief Encode a Pairing Status frame (final ok/fail after cfg exchange).
 */
int rmc_sg_encode_pairing_sts(uint8_t *buf, size_t buf_size,
			       const uint8_t *our_uid,
			       uint8_t status);

/**
 * @brief Encode a Data Communication frame (with ACK requested).
 */
int rmc_sg_encode_data(uint8_t *buf, size_t buf_size,
			const uint8_t *msg, uint8_t msg_len);

/**
 * @brief Encode an ACK frame.
 */
int rmc_sg_encode_ack(uint8_t *buf, size_t buf_size, const uint8_t ack);

/**
 * @brief Encode an Read Response frame.
 */
int rmc_sg_encode_read_resp(uint8_t *buf, size_t buf_size,
							 const struct rmc_sg_pkt_cfg_read *cfg);

/**
 * @brief Encode a Scan Response frame (0x0D).
 *
 * Carries the shuttle's device_uid (8 bytes) so the remote can verify
 * the response belongs to the shuttle it is currently beaconing to.
 *
 * @return number of bytes written, negative errno on error.
 */
int rmc_sg_encode_scan_response(uint8_t *buf, size_t buf_size,
				const uint8_t *device_uid);

/**
 * @brief Decode an incoming raw frame.
 *
 * Populates @p hdr always. Type-specific decoded data is written into
 * the union pointed by @p out — caller must check hdr.ptp to know which
 * member is valid.
 *
 * @return 0 on success, -EBADMSG on framing error, -ENOMSG on unknown PTP.
 */
struct rmc_sg_decoded {
	struct rmc_sg_frame_hdr hdr;
	union {
		struct rmc_sg_pkt_beacon      beacon;
		struct rmc_sg_pkt_auth_req    auth_req;
		struct rmc_sg_pkt_auth_conf   auth_conf;
		struct rmc_sg_pkt_cfg_write   cfg_write;
		struct rmc_sg_pkt_cfg_read    cfg_read;
		struct rmc_sg_pkt_cfg_ok      cfg_ok;
		struct rmc_sg_pkt_pairing_sts pairing_sts;
		struct rmc_sg_pkt_ack         ack;
		struct rmc_sg_pkt_data        data;
		struct rmc_sg_pkt_scan_response scan_resp;
	};
};

int rmc_sg_decode(const uint8_t *buf, uint8_t len,
		  struct rmc_sg_decoded *out);

#endif /* DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_PROTOCOL_H_ */