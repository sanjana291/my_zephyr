/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rmc_wifi_protocol.h"
#include "rmc_wifi_uart.h"
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_DECLARE(rmc_wifi, CONFIG_RMC_LOG_LEVEL);

static int dat_decode_array(uint8_t *dst, const char *src, uint8_t *out_len);

/* -------------------------------------------------------------------- */
/* Wire structs: every field is a JSON string, matching the packet spec */
/* -------------------------------------------------------------------- */

struct rmc_wifi_pkt_ack_wire {
	const char *mtp;
	const char *msg;
};

static const struct json_obj_descr rmc_wifi_ack_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_ack_wire, "MTP", mtp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_ack_wire, "MSG", msg, JSON_TOK_STRING),
};

/*
 * Decode-only: covers every field that can appear across ACK/NACK (01),
 * WiFi Status (03), TCP Status (04), Heartbeat (05), and RX Data (07) -
 * the only packet types this device ever receives. Parsed in ONE pass
 * (see rmc_wifi_protocol_decode()): json_obj_parse() mutates the input
 * buffer in place (it NUL-terminates each decoded string right where
 * its closing quote was), so a second parse pass over the same buffer
 * - e.g. an initial "sniff just MTP" pass followed by a type-specific
 * pass - silently corrupts the JSON for that second pass. A single
 * descriptor covering the union of fields, with the returned bitmask
 * telling us which fields were actually present, avoids the problem
 * entirely instead of working around it.
 */
struct rmc_wifi_pkt_rx_wire {
	const char *mtp;
	const char *msg;
	const char *wfi;
	const char *tcp;
	const char *dat;
};

static const struct json_obj_descr rmc_wifi_rx_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_rx_wire, "MTP", mtp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_rx_wire, "MSG", msg, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_rx_wire, "WFI", wfi, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_rx_wire, "TCP", tcp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_rx_wire, "DAT", dat, JSON_TOK_STRING),
};

/* Field bit positions within rmc_wifi_rx_descr / the ret bitmask. */
#define RMC_WIFI_RX_BIT_MTP BIT(0)
#define RMC_WIFI_RX_BIT_MSG BIT(1)
#define RMC_WIFI_RX_BIT_WFI BIT(2)
#define RMC_WIFI_RX_BIT_TCP BIT(3)
#define RMC_WIFI_RX_BIT_DAT BIT(4)

/* TX Data (06) and RX Data (07) share this shape. */
struct rmc_wifi_pkt_data_wire {
	const char *mtp;
	const char *dat;
};

static const struct json_obj_descr rmc_wifi_data_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_data_wire, "MTP", mtp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_data_wire, "DAT", dat, JSON_TOK_STRING),
};

struct rmc_wifi_pkt_cfg_wire {
	const char *mtp;
	const char *sid;
	const char *pwd;
	const char *stp;
	const char *tot;
	const char *atp;
	const char *ipa;
	const char *smk;
	const char *gwy;
};

/* DHCP: SID/PWD/STP/TOT/ATP only, matches the spec's DHCP example. */
static const struct json_obj_descr rmc_wifi_cfg_dhcp_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "MTP", mtp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "SID", sid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "PWD", pwd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "STP", stp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "TOT", tot, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "ATP", atp, JSON_TOK_STRING),
};

/* Static: adds IPA/SMK/GWY. */
static const struct json_obj_descr rmc_wifi_cfg_static_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "MTP", mtp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "SID", sid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "PWD", pwd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "STP", stp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "TOT", tot, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "ATP", atp, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "IPA", ipa, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "SMK", smk, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct rmc_wifi_pkt_cfg_wire, "GWY", gwy, JSON_TOK_STRING),
};

/* -------------------------------------------------------------------- */
/* Decode                                                                */
/* -------------------------------------------------------------------- */

static bool wire_bool_is_zero(const char *s)
{
	/* Every status/ack field in the spec is "0" (good) or "1" (bad). */
	return s != NULL && s[0] == '0' && s[1] == '\0';
}

int rmc_wifi_protocol_decode(uint8_t *frame, size_t frame_len, struct rmc_wifi_rx_packet *out)
{
	struct rmc_wifi_pkt_rx_wire wire = {0};
	int ret;
	long mtp_val;

	if (frame == NULL || out == NULL || frame_len == 0) {
		return -EINVAL;
	}

	/* Single pass over the whole union of possible fields - see the
	 * comment on struct rmc_wifi_pkt_rx_wire for why this must not be
	 * split into a "sniff MTP" pass followed by a second pass.
	 */
	/*
	 * json_obj_parse mutates the frame buffer in place: it writes NUL
	 * bytes over the closing quote of every decoded string. Save a
	 * verbatim copy BEFORE parsing so the RX_DATA case can safely run
	 * strstr() on the original text.
	 */
	char frame_copy[RMC_WIFI_UART_MAX_FRAME_LEN];
	size_t copy_len = (frame_len < sizeof(frame_copy) - 1)
			? frame_len : sizeof(frame_copy) - 1;
	memcpy(frame_copy, frame, copy_len);
	frame_copy[copy_len] = '\0';

	ret = json_obj_parse((char *)frame, frame_len, rmc_wifi_rx_descr,
			      ARRAY_SIZE(rmc_wifi_rx_descr), &wire);
	if (ret < 0) {
		/*
		 * MTP 07 (RX Data) carries "DAT":[...] — a JSON array. Zephyr's
		 * json_obj_parse only handles scalar/string tokens via the
		 * JSON_TOK_STRING descriptor, so it returns -EINVAL when it
		 * hits the '['. The MTP field comes first in the frame so it
		 * is already decoded in wire.mtp before the parser bails.
		 *
		 * If MTP is present and equals 7, fall through to the
		 * RMC_WIFI_MTP_RX_DATA case which reads DAT directly from
		 * the raw frame via strstr(). For any other MTP value the
		 * parse error is a genuine problem; return -EBADMSG.
		 */
		if (wire.mtp == NULL) {
			/* Parser failed before reaching MTP — truly malformed. */
			LOG_WRN("JSON parse error before MTP: %d", ret);
			return -EBADMSG;
		}
		LOG_DBG("JSON parse partial (ret=%d), MTP=%s — continuing", ret, wire.mtp);
	}
	if (wire.mtp == NULL) {
		LOG_WRN("Packet missing MTP field");
		return -ENOMSG;
	}

	mtp_val = strtol(wire.mtp, NULL, 10);

	switch (mtp_val) {
	case RMC_WIFI_MTP_ACK_NACK:
		if (!(ret & RMC_WIFI_RX_BIT_MSG)) {
			return -EBADMSG;
		}
		out->mtp = RMC_WIFI_MTP_ACK_NACK;
		out->ack.nack = !wire_bool_is_zero(wire.msg);
		return 0;

	case RMC_WIFI_MTP_WIFI_STATUS:
		if (!(ret & RMC_WIFI_RX_BIT_MSG)) {
			return -EBADMSG;
		}
		out->mtp = RMC_WIFI_MTP_WIFI_STATUS;
		out->wifi_status.connected = wire_bool_is_zero(wire.msg);
		return 0;

	case RMC_WIFI_MTP_TCP_STATUS:
		if (!(ret & RMC_WIFI_RX_BIT_MSG)) {
			return -EBADMSG;
		}
		out->mtp = RMC_WIFI_MTP_TCP_STATUS;
		out->tcp_status.connected = wire_bool_is_zero(wire.msg);
		return 0;

	case RMC_WIFI_MTP_HEARTBEAT:
		if (!(ret & RMC_WIFI_RX_BIT_WFI) || !(ret & RMC_WIFI_RX_BIT_TCP)) {
			return -EBADMSG;
		}
		out->mtp = RMC_WIFI_MTP_HEARTBEAT;
		out->heartbeat.wifi_connected = wire_bool_is_zero(wire.wfi);
		out->heartbeat.tcp_connected = wire_bool_is_zero(wire.tcp);
		return 0;

	case RMC_WIFI_MTP_RX_DATA: {
		uint8_t bin_len;
		int decode_ret;
		const char *arr_start;
		/* "DAT": is exactly 6 chars: "DAT": */
		static const char dat_key[] = "\"DAT\":";

		/*
		 * json_obj_parse mutates frame in-place (NUL over closing
		 * quotes). Search frame_copy - the snapshot taken before
		 * parse - so the key is still intact.
		 * sizeof(dat_key) - 1 == 6, lands pointer on '['.
		 */
		arr_start = strstr(frame_copy, dat_key);
		if (arr_start == NULL) {
			LOG_WRN("MTP 07: DAT field not found in frame");
			return -EBADMSG;
		}
		arr_start += sizeof(dat_key) - 1;
		while (*arr_start == ' ' || *arr_start == '\t') {
			arr_start++;
		}
		if (*arr_start != '[') {
			LOG_WRN("MTP 07: DAT is not a JSON array (got 0x%02x)",
				(unsigned char)*arr_start);
			return -EBADMSG;
		}

		decode_ret = dat_decode_array(out->rx_data.data, arr_start, &bin_len);
		if (decode_ret != 0) {
			LOG_WRN("MTP 07: DAT array decode failed: %d", decode_ret);
			return -EBADMSG;
		}
		out->mtp = RMC_WIFI_MTP_RX_DATA;
		out->rx_data.len = bin_len;
		return 0;
	}

	case RMC_WIFI_MTP_WIFI_CFG:
	case RMC_WIFI_MTP_TX_DATA:
		/* This device only ever sends these; receiving one back is unexpected. */
		LOG_WRN("Unexpected inbound MTP %ld (this device only transmits it)", mtp_val);
		return -ENOTSUP;

	default:
		LOG_WRN("Unknown MTP value: %ld", mtp_val);
		return -ENOMSG;
	}
}

/* -------------------------------------------------------------------- */
/* Encode                                                                */
/* -------------------------------------------------------------------- */

int rmc_wifi_protocol_encode_ack(bool nack, char *buf, size_t buf_size)
{
	struct rmc_wifi_pkt_ack_wire wire = {
		.mtp = "01",
		.msg = nack ? "1" : "0",
	};

	return json_obj_encode_buf(rmc_wifi_ack_descr, ARRAY_SIZE(rmc_wifi_ack_descr),
				    &wire, buf, buf_size);
}

int rmc_wifi_protocol_encode_wifi_cfg(const struct rmc_wifi_cfg *cfg, char *buf, size_t buf_size)
{
	char stp_buf[4];
	char tot_buf[4];
	char atp_buf[4];
	struct rmc_wifi_pkt_cfg_wire wire;
	const struct json_obj_descr *descr;
	size_t descr_len;

	if (cfg == NULL || buf == NULL) {
		return -EINVAL;
	}

	snprintf(stp_buf, sizeof(stp_buf), "%u", (unsigned int)cfg->security_type);
	snprintf(tot_buf, sizeof(tot_buf), "%u", (unsigned int)cfg->connect_timeout_s);
	snprintf(atp_buf, sizeof(atp_buf), "%u", (unsigned int)cfg->addr_type);

	wire = (struct rmc_wifi_pkt_cfg_wire){
		.mtp = "02",
		.sid = cfg->ssid,
		.pwd = cfg->password,
		.stp = stp_buf,
		.tot = tot_buf,
		.atp = atp_buf,
		.ipa = cfg->ip_addr,
		.smk = cfg->subnet_mask,
		.gwy = cfg->gateway_addr,
	};

	if (cfg->addr_type == RMC_WIFI_ADDR_STATIC) {
		descr = rmc_wifi_cfg_static_descr;
		descr_len = ARRAY_SIZE(rmc_wifi_cfg_static_descr);
	} else {
		descr = rmc_wifi_cfg_dhcp_descr;
		descr_len = ARRAY_SIZE(rmc_wifi_cfg_dhcp_descr);
	}

	return json_obj_encode_buf(descr, descr_len, &wire, buf, buf_size);
}

/* -------------------------------------------------------------------- */
/* DAT field codec: JSON integer array                                  */
/*                                                                       */
/* MTP 06 TX:  {"MTP":"06","DAT":[123,1,58,2,58,0,125]}                */
/* MTP 07 RX:  {"MTP":"07","DAT":[123,1,58,2,58,0,125]}                */
/*                                                                       */
/* Each byte is emitted as a plain decimal integer inside a JSON array. */
/* This is binary-safe (all 0x00-0xFF values round-trip cleanly) and    */
/* matches the cJSON-style encoding used by the ESP32-C3 firmware.      */
/* -------------------------------------------------------------------- */

/**
 * @brief Manually build {"MTP":"06","DAT":[b0,b1,...,bN]} into @p buf.
 *
 * Zephyr's json_obj_encode_buf only handles string-typed fields, so we
 * hand-format the integer array here. Each byte is written as a decimal
 * number (0-255); no hex escaping needed because decimal digits are all
 * valid JSON.
 *
 * @param buf      Output character buffer.
 * @param buf_size Size of @p buf in bytes.
 * @param data     Binary payload.
 * @param data_len Number of bytes in @p data (1-RMC_WIFI_DATA_MAX_LEN).
 *
 * @return Number of bytes written (excluding NUL) on success.
 * @retval -EINVAL  @p data is NULL / zero-length / exceeds max.
 * @retval -ENOMEM  @p buf is too small.
 */
static int dat_encode_json(char *buf, size_t buf_size,
			   const uint8_t *data, uint8_t data_len)
{
	/*
	 * Worst-case size breakdown:
	 *   {"MTP":"06","DAT":[  -> 18 chars
	 *   each byte as decimal -> max 3 digits + 1 comma = 4 chars
	 *   last byte            -> 3 digits (no trailing comma)
	 *   ]}                   -> 2 chars
	 *   NUL                  -> 1 char
	 *   Total: 18 + data_len*4 + 3 = data_len*4 + 21
	 */
	static const char prefix[] = "{\"MTP\":\"06\",\"DAT\":[";
	size_t pos = 0;
	size_t prefix_len = sizeof(prefix) - 1; /* exclude NUL */

	if (data == NULL || data_len == 0 || data_len > RMC_WIFI_DATA_MAX_LEN) {
		return -EINVAL;
	}
	if (buf_size < (size_t)(data_len * 4 + 22)) {
		return -ENOMEM;
	}

	memcpy(buf, prefix, prefix_len);
	pos = prefix_len;

	for (uint8_t i = 0; i < data_len; i++) {
		if (i > 0) {
			buf[pos++] = ',';
		}
		/* snprintf decimal for this byte (1-3 chars) */
		pos += snprintf(&buf[pos], buf_size - pos, "%u",
				(unsigned int)data[i]);
	}

	buf[pos++] = ']';
	buf[pos++] = '}';
	buf[pos]   = '\0';

	return (int)pos;
}

/**
 * @brief Parse a JSON integer array from the DAT field back to binary.
 *
 * Expects the raw array token as Zephyr's json_obj_parse gives it after
 * parsing the outer object (it will be the substring starting at '[').
 *
 * @param dst      Output binary buffer (RMC_WIFI_DATA_MAX_LEN bytes).
 * @param src      Pointer to the '[' that begins the array in the
 *                 (already NUL-terminated) JSON buffer.
 * @param out_len  Set to number of decoded bytes on success.
 *
 * @retval 0        Success.
 * @retval -EINVAL  Malformed array.
 * @retval -ENOMEM  More bytes than RMC_WIFI_DATA_MAX_LEN.
 */
static int dat_decode_array(uint8_t *dst, const char *src, uint8_t *out_len)
{
	uint8_t count = 0;

	if (src == NULL || *src != '[') {
		return -EINVAL;
	}
	src++; /* skip opening '[' */

	while (*src != '\0' && *src != ']') {
		char *end;
		long val;

		/* Skip whitespace */
		while (*src == ' ' || *src == '\t') {
			src++;
		}
		if (*src == ']' || *src == '\0') {
			break;
		}

		val = strtol(src, &end, 10);
		if (end == src || val < 0 || val > 255) {
			return -EINVAL;
		}
		if (count >= RMC_WIFI_DATA_MAX_LEN) {
			return -ENOMEM;
		}
		dst[count++] = (uint8_t)val;

		src = end;

		/* Skip optional comma between elements */
		while (*src == ' ' || *src == '\t') {
			src++;
		}
		if (*src == ',') {
			src++;
		}
	}

	*out_len = count;
	return 0;
}

int rmc_wifi_protocol_encode_tx_data(const uint8_t *data, uint8_t data_len,
				      char *buf, size_t buf_size)
{
	/*
	 * dat_encode_json() returns the number of bytes written on success
	 * (positive) or a negative errno on failure.
	 * The caller (send_tx_data in rmc_wifi.c) checks (ret != 0), so we
	 * must return 0 on success — matching json_obj_encode_buf convention.
	 */
	int ret = dat_encode_json(buf, buf_size, data, data_len);

	return (ret > 0) ? 0 : ret;
}