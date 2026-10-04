/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Configuration Module - JSON encode / decode.
 *
 * Packet-copy discipline
 * -----------------------
 * The just-received frame is copied ONCE into a pristine, read-only buffer
 * (s_rxbuf_orig) by cfg_json_dispatch(). That buffer is NEVER handed to a
 * mutating parser afterwards.
 *
 * Zephyr's json_obj_parse() mutates its input buffer in place (string
 * tokens are unescaped and NUL-terminated over the closing quote). Any
 * function that needs to call json_obj_parse() therefore first takes a
 * fresh, disposable working copy of the pristine frame via fresh_copy().
 * Because every parse call starts from an unmodified copy of the original
 * packet, parsing one field can never corrupt or zero out a field parsed
 * earlier (or later) from the same packet.
 *
 * Manual scans that only read (strstr()/strtoul()/strtoull()) operate
 * directly on the pristine buffer, which is safe because they never write
 * to it.
 *
 * Every string extracted from a parsed field is copied into a locally
 * owned, explicitly NUL-terminated staging buffer (see extract_string())
 * before being copied again into the fixed-size wire struct field, with
 * explicit bounds checking throughout.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>

#include <common_config/config_params.h>
#include <config_module/config_module.h>
#include "config_module_internal.h"

LOG_MODULE_DECLARE(config_module, CONFIG_CONFIG_MODULE_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * Pristine frame copy - set once per dispatch, never mutated afterwards.
 * (static - single tcp_server thread, no concurrency)
 * --------------------------------------------------------------------- */

static char   s_rxbuf_orig[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];
static size_t s_rxbuf_orig_len;

/* Copy frame into s_rxbuf_orig and return its length, or -EMSGSIZE if too big. */
static int frame_copy(const char *json, size_t json_len)
{
	if (json_len >= sizeof(s_rxbuf_orig)) {
		LOG_ERR("Frame too large for parse buffer (%zu >= %zu)",
			json_len, sizeof(s_rxbuf_orig));
		return -EMSGSIZE;
	}

	memcpy(s_rxbuf_orig, json, json_len);
	s_rxbuf_orig[json_len] = '\0';
	s_rxbuf_orig_len = json_len;

	return (int)json_len;
}

/*
 * @brief Copy the pristine frame into a caller-owned scratch buffer.
 *
 * Every parser that needs to call json_obj_parse() (which mutates its
 * input buffer in place) MUST call this first and parse the scratch copy,
 * never s_rxbuf_orig directly. Each call starts from the untouched
 * original, so earlier in-place mutations (e.g. from a previous field's
 * string decode) can never leak into a later parse.
 *
 * @param dst      Caller-owned destination buffer.
 * @param dst_size Size of dst, including room for the terminating NUL.
 */
static void fresh_copy(char *dst, size_t dst_size)
{
	size_t n = s_rxbuf_orig_len;

	if (n >= dst_size) {
		n = dst_size - 1;
	}
	memcpy(dst, s_rxbuf_orig, n);
	dst[n] = '\0';
}

/*
 * @brief Extract a JSON-decoded string field into a locally owned,
 *        always-NUL-terminated staging buffer, enforcing length bounds.
 *
 * @param src      Pointer returned by json_obj_parse() for a JSON_TOK_STRING
 *                  field (lives inside a scratch buffer from fresh_copy()).
 * @param dst      Destination buffer, at least dst_size bytes.
 * @param dst_size Size of dst, INCLUDING room for the terminating NUL.
 * @param min_len  Minimum acceptable length (inclusive).
 * @param max_len  Maximum acceptable length (inclusive); must be < dst_size.
 * @param out_len  On success, set to the copied length.
 * @return 0 on success, -EINVAL if src is NULL or the length is out of range.
 */
static int extract_string(const char *src, char *dst, size_t dst_size,
			   size_t min_len, size_t max_len, size_t *out_len)
{
	if (src == NULL) {
		return -EINVAL;
	}

	/* Bounded strnlen: never reads past dst_size even if the decoder's
	 * embedded NUL were somehow missing. */
	size_t len = strnlen(src, dst_size);

	if (len >= dst_size) {
		return -EINVAL; /* wouldn't fit with a terminator */
	}
	if (len < min_len || len > max_len) {
		return -EINVAL;
	}

	memcpy(dst, src, len);
	dst[len] = '\0';

	if (out_len != NULL) {
		*out_len = len;
	}
	return 0;
}

/*
 * @brief Find a bare-number JSON value by key in a buffer and return it
 *        as uint32_t via strtoul (handles values that overflow int32_t,
 *        e.g. IP addresses and large statistics counters).
 *
 * Only ever called against the pristine, read-only buffer - strstr()/
 * strtoul() never mutate their input, so this is always safe.
 *
 * @param buf     NUL-terminated buffer to search (must be s_rxbuf_orig).
 * @param key     JSON key string, e.g. "WIP".
 * @param out_val Where to store the parsed value.
 * @return 0 on success, -EINVAL if key not found or value is not a
 *         valid decimal number.
 */
static int parse_u32_field(const char *buf, const char *key, uint32_t *out_val)
{
	char search[16];

	snprintf(search, sizeof(search), "\"%s\"", key);

	const char *pos = strstr(buf, search);

	if (pos == NULL) {
		LOG_WRN("parse_u32_field: key %s not found", key);
		return -EINVAL;
	}

	pos += strlen(search);
	while (*pos == ':' || *pos == ' ' || *pos == '\t') {
		pos++;
	}
	if (*pos == '\0') {
		return -EINVAL;
	}

	char *endp;

	errno = 0;
	unsigned long v = strtoul(pos, &endp, 10);

	if (endp == pos || errno == ERANGE) {
		LOG_WRN("parse_u32_field: strtoul failed for key %s", key);
		return -EINVAL;
	}

	*out_val = (uint32_t)v;
	return 0;
}

/* -----------------------------------------------------------------------
 * Envelope descriptor - used for both dispatch (MTP) and SID extraction
 * --------------------------------------------------------------------- */

struct pkt_envelope {
	const char *mtp;
	size_t      mtp_len;
	const char *sid;
	size_t      sid_len;
};

static const struct json_obj_descr envelope_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct pkt_envelope, "MTP", mtp,
				   JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct pkt_envelope, "SID", sid,
				   JSON_TOK_STRING),
};

/* Bitmask with only the MTP bit set (field index 0). */
#define ENVELOPE_MTP_MASK  BIT(0)

/* -----------------------------------------------------------------------
 * ACK / NACK  (MTP "01")
 * --------------------------------------------------------------------- */

int cfg_json_encode_ack_nack(char *buf, size_t buf_size, bool ack, uint8_t rtp)
{
	return snprintf(buf, buf_size,
			"{\"MTP\":\"01\",\"MSG\":%d,\"RTP\":\"%02d\"}", ack ? 0 : 1, rtp);
}

/* -----------------------------------------------------------------------
 * MTP "04" Read Response encoder - one function per section
 *
 * snprintf() is used deliberately: json_obj_encode_buf() cannot express
 * the string-typed MTP/SID header fields as leading keys in the same
 * object as numeric payload fields without a nested-object layout, which
 * would not match the flat wire format in the spec.
 * --------------------------------------------------------------------- */

static int encode_sec_a(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"A\","
		"\"GFF\":%u,\"LWS\":%lu,\"PIB\":%u}",
		p->sec_a.gff, (unsigned long)p->sec_a.lws, p->sec_a.pib);
}

static int encode_sec_b(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"B\","
		"\"PWS\":%lu,\"PWC\":%lu,\"WGO\":%u}",
		(unsigned long)p->sec_b.pws, (unsigned long)p->sec_b.pwc,
		p->sec_b.wgo);
}

static int encode_sec_c(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"C\","
		"\"GDP\":%u,\"PLD\":%u,\"EFC\":%u}",
		p->sec_c.gdp, p->sec_c.pld, p->sec_c.efc);
}

static int encode_sec_d(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"D\","
		"\"SWL\":%u,\"SLD\":%u,\"LSP\":%u}",
		p->sec_d.swl, p->sec_d.sld, p->sec_d.lsp);
}

static int encode_sec_e(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"E\","
		"\"AUR\":%u,\"AUT\":%u,\"ALR\":%u,\"ALT\":%u,"
		"\"MAS\":%u,\"MAT\":%u,\"MUS\":%u,\"MUT\":%u,"
		"\"DUD\":%u,\"DUT\":%u,\"DLD\":%u,\"DLT\":%u,"
		"\"LAS\":%u,\"LAT\":%u,\"LDS\":%u,\"LDT\":%u}",
		p->sec_e.aur, p->sec_e.aut, p->sec_e.alr, p->sec_e.alt,
		p->sec_e.mas, p->sec_e.mat, p->sec_e.mus, p->sec_e.mut,
		p->sec_e.dud, p->sec_e.dut, p->sec_e.dld, p->sec_e.dlt,
		p->sec_e.las, p->sec_e.lat, p->sec_e.lds, p->sec_e.ldt);
}

static int encode_sec_f(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"F\","
		"\"SSD\":%u,\"RNW\":%u,\"RNP\":%u,\"SDP\":%u}",
		p->sec_f.ssd, p->sec_f.rnw, p->sec_f.rnp, p->sec_f.sdp);
}

static int encode_sec_g(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"G\","
		"\"ASR\":%u,\"ATL\":%u}",
		p->sec_g.asr, p->sec_g.atl);
}

static int encode_sec_h(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"H\","
		"\"LFT\":%u}",
		p->sec_h.lft);
}

static int encode_sec_i(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"I\","
		"\"PKD\":%u,\"API\":%lu,\"MSI\":%u}",
		p->sec_i.pkd, (unsigned long)p->sec_i.api, p->sec_i.msi);
}

static int encode_sec_j(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"J\","
		"\"FSO\":%d,\"RSO\":%d,\"FPO\":%d,\"RPO\":%d,\"PTC\":%u}",
		p->sec_j.fso, p->sec_j.rso, p->sec_j.fpo, p->sec_j.rpo,
		p->sec_j.ptc);
}

static int encode_sec_k(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"K\","
		"\"WHD\":%u,\"GBR\":%u}",
		p->sec_k.whd, p->sec_k.gbr);
}

/*
 * Section L: ssz/psz (length-prefix bytes) are NVM-internal; NOT emitted
 * in JSON. SSID and password are transmitted as plain JSON strings
 * (SSI/WPS), sourced from config_sec_l_t.sid / .wps which are fixed-size
 * char arrays that are NOT guaranteed NUL-terminated - always copy through
 * a bounded, explicitly NUL-terminated local buffer before using %s.
 */
static int encode_sec_l(char *b, size_t sz, const config_params_t *p)
{
	char ssid[CFG_L_SSID_MAX_LEN + 1];
	char pwd[CFG_L_PWD_MAX_LEN + 1];
	char rdn[CFG_L_RDN_LEN + 1];

	uint8_t ssid_sz = strnlen(p->sec_l.sid, CFG_L_SSID_MAX_LEN);
	uint8_t pwd_sz  = strnlen(p->sec_l.wps, CFG_L_PWD_MAX_LEN);
	uint8_t rdn_sz  = strnlen(p->sec_l.rdn, CFG_L_RDN_LEN);

	memcpy(ssid, p->sec_l.sid, ssid_sz); ssid[ssid_sz] = '\0';
	memcpy(pwd,  p->sec_l.wps, pwd_sz);  pwd[pwd_sz]   = '\0';
	memcpy(rdn,  p->sec_l.rdn, rdn_sz);  rdn[rdn_sz]   = '\0';

	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"L\","
		"\"SSI\":\"%s\",\"WPS\":\"%s\","
		"\"WAT\":%u,\"WST\":%u,"
		"\"WIP\":%lu,\"WSM\":%lu,\"WGI\":%lu,"
		"\"RDN\":\"%s\",\"RCP\":%u}",
		ssid, pwd,
		p->sec_l.wat, p->sec_l.wst,
		(unsigned long)p->sec_l.wip,
		(unsigned long)p->sec_l.wsm,
		(unsigned long)p->sec_l.wgi,
		rdn, p->sec_l.rcp);
}

static int encode_sec_m(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"M\","
		"\"RAD\":%u,\"SYW\":%u,\"CID\":%u,\"SND\":%u,\"PMT\":%u}",
		p->sec_m.rad, p->sec_m.syw, p->sec_m.cid, p->sec_m.snd,
		p->sec_m.pmt);
}

static int encode_sec_n(char *b, size_t sz, const config_params_t *p)
{
	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"N\","
		"\"COM\":%u,\"MPH\":%u,\"PLM\":%u,\"MNO\":%u,\"MPL\":%u}",
		p->sec_n.com, p->sec_n.mph, p->sec_n.plm, p->sec_n.mno,
		p->sec_n.mpl);
}

/*
 * Section O encoder.
 *
 * STM is emitted as a JSON array of CFG_O_STM_LEN raw bytes. The
 * Configuration Module never interprets or reads hardware time for this
 * field - the application is responsible for populating params->sec_o.stm
 * before calling CFG_Resp(CFG_RESP_READ, ...).
 */
static int encode_sec_o(char *b, size_t sz, const config_params_t *p)
{
	char stm_buf[64];

	/* Convert struct rtc_time back to [DD, MM, YYYY, HR, MN, SC] format */
	int n = snprintf(stm_buf, sizeof(stm_buf), "%d,%d,%d,%d,%d,%d",
			 p->sec_o.stm.tm_mday,
			 p->sec_o.stm.tm_mon + 1,
			 p->sec_o.stm.tm_year + 1900,
			 p->sec_o.stm.tm_hour,
			 p->sec_o.stm.tm_min,
			 p->sec_o.stm.tm_sec);

	if (n < 0 || (size_t)n >= sizeof(stm_buf)) {
		return -EINVAL;
	}

	return snprintf(b, sz,
		"{\"MTP\":\"04\",\"SID\":\"O\","
		"\"FWV\":%u,\"HWV\":%u,\"STM\":[%s],\"TOD\":%lu,"
		"\"SCT\":%lu,\"RCT\":%lu,\"PSC\":%lu,\"PLC\":%lu,"
		"\"LCT\":%lu,\"M4C\":%u,\"PLT\":%u,\"TMN\":%lu}",
		p->sec_o.fwv, p->sec_o.hwv, stm_buf,
		(unsigned long)p->sec_o.tod,
		(unsigned long)p->sec_o.sct, (unsigned long)p->sec_o.rct,
		(unsigned long)p->sec_o.psc, (unsigned long)p->sec_o.plc,
		(unsigned long)p->sec_o.lct, p->sec_o.m4c, p->sec_o.plt, (unsigned long)p->sec_o.tmn);
}
typedef int (*encode_fn_t)(char *, size_t, const config_params_t *);

static const encode_fn_t encode_table[CONFIG_SEC_MAX] = {
	[CONFIG_SEC_A] = encode_sec_a,
	[CONFIG_SEC_B] = encode_sec_b,
	[CONFIG_SEC_C] = encode_sec_c,
	[CONFIG_SEC_D] = encode_sec_d,
	[CONFIG_SEC_E] = encode_sec_e,
	[CONFIG_SEC_F] = encode_sec_f,
	[CONFIG_SEC_G] = encode_sec_g,
	[CONFIG_SEC_H] = encode_sec_h,
	[CONFIG_SEC_I] = encode_sec_i,
	[CONFIG_SEC_J] = encode_sec_j,
	[CONFIG_SEC_K] = encode_sec_k,
	[CONFIG_SEC_L] = encode_sec_l,
	[CONFIG_SEC_M] = encode_sec_m,
	[CONFIG_SEC_N] = encode_sec_n,
	[CONFIG_SEC_O] = encode_sec_o,
};

int cfg_json_encode_read_response(char *buf, size_t buf_size,
				   char sec_id, const config_params_t *params)
{
	if (sec_id < 'A' || sec_id > 'O') {
		return -EINVAL;
	}
	return encode_table[sec_id - 'A'](buf, buf_size, params);
}

/* -----------------------------------------------------------------------
 * MTP "05" Write Request decoders - Zephyr json_obj_parse()
 *
 * Each section gets:
 *   1. An intermediate parse struct with int32_t for every numeric field
 *      that safely fits (json.h maps JSON_TOK_NUMBER -> int32_t regardless
 *      of target width) and const char * / size_t _len pairs for strings.
 *      Fields whose legal range can exceed INT32_MAX (IP addresses, large
 *      statistics counters) are intentionally omitted here and parsed
 *      separately with parse_u32_field() against the pristine buffer.
 *   2. A json_obj_descr[] that maps JSON keys to those fields.
 *   3. A "required fields" bitmask - every field that must be present.
 *   4. A copy function that casts and stores into config_params_t.
 *
 * Every parser below starts by taking a fresh working copy of the
 * pristine frame via fresh_copy() and parses ONLY that copy.
 * --------------------------------------------------------------------- */

/* ---- Section A ---- */

struct parse_sec_a { int32_t gff; int32_t lws; int32_t pib; };

static const struct json_obj_descr parse_sec_a_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_a, "GFF", gff, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_a, "LWS", lws, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_a, "PIB", pib, JSON_TOK_NUMBER),
};

#define SEC_A_REQ_MASK  (BIT(3) - 1)

static int parse_sec_a(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_a p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_a_descr,
				 ARRAY_SIZE(parse_sec_a_descr), &p);

	if (ret < 0 || (ret & SEC_A_REQ_MASK) != SEC_A_REQ_MASK) {
		LOG_WRN("parse_sec_a: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_a.gff = (uint16_t)p.gff;
	out->sec_a.lws = (uint32_t)p.lws;
	out->sec_a.pib = (uint16_t)p.pib;
	return 0;
}

/* ---- Section B ---- */

struct parse_sec_b { int32_t pws; int32_t pwc; int32_t wgo; };

static const struct json_obj_descr parse_sec_b_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_b, "PWS", pws, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_b, "PWC", pwc, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_b, "WGO", wgo, JSON_TOK_NUMBER),
};

#define SEC_B_REQ_MASK  (BIT(3) - 1)

static int parse_sec_b(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_b p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_b_descr,
				 ARRAY_SIZE(parse_sec_b_descr), &p);

	if (ret < 0 || (ret & SEC_B_REQ_MASK) != SEC_B_REQ_MASK) {
		LOG_WRN("parse_sec_b: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_b.pws = (uint32_t)p.pws;
	out->sec_b.pwc = (uint32_t)p.pwc;
	out->sec_b.wgo = (uint16_t)p.wgo;
	return 0;
}

/* ---- Section C ---- */

struct parse_sec_c { int32_t gdp; int32_t pld; int32_t efc; };

static const struct json_obj_descr parse_sec_c_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_c, "GDP", gdp, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_c, "PLD", pld, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_c, "EFC", efc, JSON_TOK_NUMBER),
};

#define SEC_C_REQ_MASK  (BIT(3) - 1)

static int parse_sec_c(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_c p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_c_descr,
				 ARRAY_SIZE(parse_sec_c_descr), &p);

	if (ret < 0 || (ret & SEC_C_REQ_MASK) != SEC_C_REQ_MASK) {
		LOG_WRN("parse_sec_c: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_c.gdp = (uint16_t)p.gdp;
	out->sec_c.pld = (uint16_t)p.pld;
	out->sec_c.efc = (uint16_t)p.efc;
	return 0;
}

/* ---- Section D ---- */

struct parse_sec_d { int32_t swl; int32_t sld; int32_t lsp; };

static const struct json_obj_descr parse_sec_d_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_d, "SWL", swl, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_d, "SLD", sld, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_d, "LSP", lsp, JSON_TOK_NUMBER),
};

#define SEC_D_REQ_MASK  (BIT(3) - 1)

static int parse_sec_d(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_d p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_d_descr,
				 ARRAY_SIZE(parse_sec_d_descr), &p);

	if (ret < 0 || (ret & SEC_D_REQ_MASK) != SEC_D_REQ_MASK) {
		LOG_WRN("parse_sec_d: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_d.swl = (uint16_t)p.swl;
	out->sec_d.sld = (uint16_t)p.sld;
	out->sec_d.lsp = (uint16_t)p.lsp;
	return 0;
}

/* ---- Section E ---- */

struct parse_sec_e {
	int32_t aur; int32_t aut; int32_t alr; int32_t alt;
	int32_t mas; int32_t mat; int32_t mus; int32_t mut;
	int32_t dud; int32_t dut; int32_t dld; int32_t dlt;
	int32_t las; int32_t lat; int32_t lds; int32_t ldt;
};

static const struct json_obj_descr parse_sec_e_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "AUR", aur, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "AUT", aut, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "ALR", alr, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "ALT", alt, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "MAS", mas, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "MAT", mat, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "MUS", mus, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "MUT", mut, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "DUD", dud, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "DUT", dut, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "DLD", dld, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "DLT", dlt, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "LAS", las, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "LAT", lat, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "LDS", lds, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_e, "LDT", ldt, JSON_TOK_NUMBER),
};

#define SEC_E_REQ_MASK  (BIT(16) - 1)   /* all 16 fields */

static int parse_sec_e(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_e p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_e_descr,
				 ARRAY_SIZE(parse_sec_e_descr), &p);

	if (ret < 0 || (ret & SEC_E_REQ_MASK) != SEC_E_REQ_MASK) {
		LOG_WRN("parse_sec_e: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_e.aur = (uint16_t)p.aur; out->sec_e.aut = (uint8_t)p.aut;
	out->sec_e.alr = (uint16_t)p.alr; out->sec_e.alt = (uint8_t)p.alt;
	out->sec_e.mas = (uint16_t)p.mas; out->sec_e.mat = (uint8_t)p.mat;
	out->sec_e.mus = (uint16_t)p.mus; out->sec_e.mut = (uint8_t)p.mut;
	out->sec_e.dud = (uint16_t)p.dud; out->sec_e.dut = (uint8_t)p.dut;
	out->sec_e.dld = (uint16_t)p.dld; out->sec_e.dlt = (uint8_t)p.dlt;
	out->sec_e.las = (uint16_t)p.las; out->sec_e.lat = (uint8_t)p.lat;
	out->sec_e.lds = (uint16_t)p.lds; out->sec_e.ldt = (uint8_t)p.ldt;
	return 0;
}

/* ---- Section F ---- */

struct parse_sec_f { int32_t ssd; int32_t rnw; int32_t rnp; int32_t sdp; };

static const struct json_obj_descr parse_sec_f_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_f, "SSD", ssd, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_f, "RNW", rnw, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_f, "RNP", rnp, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_f, "SDP", sdp, JSON_TOK_NUMBER),
};

#define SEC_F_REQ_MASK  (BIT(4) - 1)

static int parse_sec_f(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_f p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_f_descr,
				 ARRAY_SIZE(parse_sec_f_descr), &p);

	if (ret < 0 || (ret & SEC_F_REQ_MASK) != SEC_F_REQ_MASK) {
		LOG_WRN("parse_sec_f: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_f.ssd = (uint16_t)p.ssd;
	out->sec_f.rnw = (uint16_t)p.rnw;
	out->sec_f.rnp = (uint16_t)p.rnp;
	out->sec_f.sdp = (uint16_t)p.sdp;
	return 0;
}

/* ---- Section G ---- */

struct parse_sec_g { int32_t asr; int32_t atl; };

static const struct json_obj_descr parse_sec_g_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_g, "ASR", asr, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_g, "ATL", atl, JSON_TOK_NUMBER),
};

#define SEC_G_REQ_MASK  (BIT(2) - 1)

static int parse_sec_g(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_g p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_g_descr,
				 ARRAY_SIZE(parse_sec_g_descr), &p);

	if (ret < 0 || (ret & SEC_G_REQ_MASK) != SEC_G_REQ_MASK) {
		LOG_WRN("parse_sec_g: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_g.asr = (uint16_t)p.asr;
	out->sec_g.atl = (uint16_t)p.atl;
	return 0;
}

/* ---- Section H ---- */

struct parse_sec_h { int32_t lft; };

static const struct json_obj_descr parse_sec_h_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_h, "LFT", lft, JSON_TOK_NUMBER),
};

#define SEC_H_REQ_MASK  BIT(0)

static int parse_sec_h(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_h p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_h_descr,
				 ARRAY_SIZE(parse_sec_h_descr), &p);

	if (ret < 0 || (ret & SEC_H_REQ_MASK) != SEC_H_REQ_MASK) {
		LOG_WRN("parse_sec_h: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_h.lft = (uint16_t)p.lft;
	return 0;
}

/* ---- Section I ---- */

struct parse_sec_i { int32_t pkd; int32_t api; int32_t msi; };

static const struct json_obj_descr parse_sec_i_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_i, "PKD", pkd, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_i, "API", api, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_i, "MSI", msi, JSON_TOK_NUMBER),
};

#define SEC_I_REQ_MASK  (BIT(3) - 1)

static int parse_sec_i(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_i p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_i_descr,
				 ARRAY_SIZE(parse_sec_i_descr), &p);

	if (ret < 0 || (ret & SEC_I_REQ_MASK) != SEC_I_REQ_MASK) {
		LOG_WRN("parse_sec_i: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_i.pkd = (uint16_t)p.pkd;
	out->sec_i.api = (uint32_t)p.api;
	out->sec_i.msi = (uint16_t)p.msi;
	return 0;
}

/* ---- Section J ---- */

struct parse_sec_j {
	int32_t fso; int32_t rso; int32_t fpo; int32_t rpo; int32_t ptc;
};

static const struct json_obj_descr parse_sec_j_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_j, "FSO", fso, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_j, "RSO", rso, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_j, "FPO", fpo, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_j, "RPO", rpo, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_j, "PTC", ptc, JSON_TOK_NUMBER),
};

#define SEC_J_REQ_MASK  (BIT(5) - 1)

static int parse_sec_j(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_j p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_j_descr,
				 ARRAY_SIZE(parse_sec_j_descr), &p);

	if (ret < 0 || (ret & SEC_J_REQ_MASK) != SEC_J_REQ_MASK) {
		LOG_WRN("parse_sec_j: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_j.fso = (int16_t)p.fso; out->sec_j.rso = (int16_t)p.rso;
	out->sec_j.fpo = (int16_t)p.fpo; out->sec_j.rpo = (int16_t)p.rpo;
	out->sec_j.ptc = (uint16_t)p.ptc;
	return 0;
}

/* ---- Section K ---- */

struct parse_sec_k { int32_t whd; int32_t gbr; };

static const struct json_obj_descr parse_sec_k_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_k, "WHD", whd, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_k, "GBR", gbr, JSON_TOK_NUMBER),
};

#define SEC_K_REQ_MASK  (BIT(2) - 1)

static int parse_sec_k(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_k p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_k_descr,
				 ARRAY_SIZE(parse_sec_k_descr), &p);

	if (ret < 0 || (ret & SEC_K_REQ_MASK) != SEC_K_REQ_MASK) {
		LOG_WRN("parse_sec_k: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_k.whd = (uint16_t)p.whd;
	out->sec_k.gbr = (uint16_t)p.gbr;
	return 0;
}

/* ---- Section L ---- */

struct parse_sec_l {
	const char *ssi;  size_t ssi_len;
	const char *wps;  size_t wps_len;
	int32_t     wat;
	int32_t     wst;
	/* wip, wsm, wgi parsed separately - values can exceed INT32_MAX */
	const char *rdn;  size_t rdn_len;
	int32_t     rcp;
};

static const struct json_obj_descr parse_sec_l_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "SSI", ssi, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "WPS", wps, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "WAT", wat, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "WST", wst, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "RDN", rdn, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_l, "RCP", rcp, JSON_TOK_NUMBER),
};

#define SEC_L_REQ_MASK  (BIT(6) - 1)   /* SSI,WPS,WAT,WST,RDN,RCP */

static int parse_sec_l(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_l p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_l_descr,
				 ARRAY_SIZE(parse_sec_l_descr), &p);

	if (ret < 0 || (ret & SEC_L_REQ_MASK) != SEC_L_REQ_MASK) {
		LOG_WRN("parse_sec_l: json_obj_parse failed ret=%d mask=0x%x", ret, ret);
		return -EINVAL;
	}

	/* SSID: 1..(CFG_L_SSID_MAX_LEN - 1) chars so the wire buffer, which
	 * is NOT NUL-terminated, always has at least one spare byte that we
	 * zero-fill (acts as an implicit terminator for any consumer that
	 * treats it as a C string up to CFG_L_SSID_MAX_LEN bytes). */
	char ssid_stage[CFG_L_SSID_MAX_LEN];
	size_t ssid_len;

	if (extract_string(p.ssi, ssid_stage, sizeof(ssid_stage),
			    1, CFG_L_SSID_MAX_LEN - 1, &ssid_len) < 0) {
		LOG_WRN("parse_sec_l: SSI length out of range");
		return -EINVAL;
	}
	memset(out->sec_l.sid, 0, CFG_L_SSID_MAX_LEN);
	memcpy(out->sec_l.sid, ssid_stage, ssid_len);

	out->sec_l.ssz = ssid_len;

	char pwd_stage[CFG_L_PWD_MAX_LEN];
	size_t pwd_len;

	if (extract_string(p.wps, pwd_stage, sizeof(pwd_stage),
			    1, CFG_L_PWD_MAX_LEN - 1, &pwd_len) < 0) {
		LOG_WRN("parse_sec_l: WPS length out of range");
		return -EINVAL;
	}
	memset(out->sec_l.wps, 0, CFG_L_PWD_MAX_LEN);
	memcpy(out->sec_l.wps, pwd_stage, pwd_len);
	out->sec_l.psz = pwd_len;

	/* RDN - remote controller DNS name / MAC string, up to CFG_L_RDN_LEN
	 * bytes; empty allowed. */
	char rdn_stage[CFG_L_RDN_LEN + 1];
	size_t rdn_len;

	if (extract_string(p.rdn, rdn_stage, sizeof(rdn_stage),
			    0, CFG_L_RDN_LEN, &rdn_len) < 0) {
		LOG_WRN("parse_sec_l: RDN length out of range");
		return -EINVAL;
	}
	memset(out->sec_l.rdn, 0, CFG_L_RDN_LEN);
	memcpy(out->sec_l.rdn, rdn_stage, rdn_len);

	/* Scalar numerics - safe in int32_t */
	out->sec_l.wat = (uint8_t)p.wat;
	out->sec_l.wst = (uint8_t)p.wst;
	out->sec_l.rcp = (uint16_t)p.rcp;

	/* IP fields - parsed from the pristine buffer because they can
	 * exceed INT32_MAX. */
	uint32_t wip, wsm, wgi;

	if (parse_u32_field(s_rxbuf_orig, "WIP", &wip) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "WSM", &wsm) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "WGI", &wgi) < 0) {
		LOG_WRN("parse_sec_l: WIP/WSM/WGI parse failed");
		return -EINVAL;
	}
	out->sec_l.wip = wip;
	out->sec_l.wsm = wsm;
	out->sec_l.wgi = wgi;

	return 0;
}

/* ---- Section M ---- */

struct parse_sec_m {
	int32_t rad; int32_t syw; int32_t cid; int32_t snd; int32_t pmt;
};

static const struct json_obj_descr parse_sec_m_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_m, "RAD", rad, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_m, "SYW", syw, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_m, "CID", cid, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_m, "SND", snd, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_m, "PMT", pmt, JSON_TOK_NUMBER),
};

#define SEC_M_REQ_MASK  (BIT(5) - 1)

static int parse_sec_m(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_m p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_m_descr,
				 ARRAY_SIZE(parse_sec_m_descr), &p);

	if (ret < 0 || (ret & SEC_M_REQ_MASK) != SEC_M_REQ_MASK) {
		LOG_WRN("parse_sec_m: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_m.rad = (uint8_t)p.rad;
	out->sec_m.syw = (uint8_t)p.syw;
	out->sec_m.cid = (uint8_t)p.cid;
	out->sec_m.snd = (uint8_t)p.snd;
	out->sec_m.pmt = (uint16_t)p.pmt;
	return 0;
}

/* ---- Section N ---- */

struct parse_sec_n {
	int32_t com; int32_t mph; int32_t plm; int32_t mno; int32_t mpl;
};

static const struct json_obj_descr parse_sec_n_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_n, "COM", com, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_n, "MPH", mph, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_n, "PLM", plm, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_n, "MNO", mno, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_n, "MPL", mpl, JSON_TOK_NUMBER),
};

#define SEC_N_REQ_MASK  (BIT(5) - 1)

static int parse_sec_n(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_n p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_n_descr,
				 ARRAY_SIZE(parse_sec_n_descr), &p);

	if (ret < 0 || (ret & SEC_N_REQ_MASK) != SEC_N_REQ_MASK) {
		LOG_WRN("parse_sec_n: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}
	out->sec_n.com = (uint8_t)p.com;
	out->sec_n.mph = (uint8_t)p.mph;
	out->sec_n.plm = (uint16_t)p.plm;
	out->sec_n.mno = (uint16_t)p.mno;
	out->sec_n.mpl = (uint16_t)p.mpl;
	return 0;
}

/* ---- Section O ---- */

struct parse_sec_o {
	int32_t fwv; int32_t hwv;   /* read-only; accepted but not stored */
	int32_t m4c; int32_t plt;
	/* TOD/SCT/RCT/PSC/PLC/LCT parsed separately (may exceed INT32_MAX).
	 * STM is a JSON array, parsed manually below. */
};

static const struct json_obj_descr parse_sec_o_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_o, "FWV", fwv, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_o, "HWV", hwv, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_o, "M4C", m4c, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct parse_sec_o, "PLT", plt, JSON_TOK_NUMBER)
};

/* FWV/HWV (bits 0,1) are optional - Studio may omit read-only fields on
 * write. M4C (bit 2) is required. */
#define SEC_O_REQ_MASK  BIT(3)

/**
 * @brief Parse the STM JSON array from the pristine buffer into
 *        out->sec_o.stm[].
 *
 * Expected format: "STM":[b0,b1,...,b(CFG_O_STM_LEN-1)], each element
 * 0-255 (uint8_t). This module treats STM purely as opaque data - no
 * calendar/RTC-specific interpretation or validation is performed here;
 * that belongs to the application layer.
 *
 * Only ever reads from s_rxbuf_orig (pristine, read-only).
 *
 * @return 0 on success, -EINVAL if the key is absent or malformed.
 */
static int parse_sec_o_stm(config_params_t *out)
{
	const char *key = strstr(s_rxbuf_orig, "\"STM\"");
	if (key == NULL) {
		LOG_WRN("parse_sec_o: STM key not found");
		return -EINVAL;
	}

	const char *pos = key + 5;
	while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;

	if (*pos != '[') return -EINVAL;
	pos++;

	uint32_t parsed_vals[CFG_O_STM_LEN];

	for (int i = 0; i < CFG_O_STM_LEN; i++) {
		while (*pos == ' ' || *pos == '\t') pos++;

		if (*pos == '\0' || *pos == ']') return -EINVAL;

		char *endp;
		errno = 0;
		unsigned long v = strtoul(pos, &endp, 10);

		if (endp == pos || errno == ERANGE) return -EINVAL;

		parsed_vals[i] = (uint32_t)v;
		pos = endp;

		while (*pos == ' ' || *pos == '\t') pos++;
		if (i < CFG_O_STM_LEN - 1) {
			if (*pos != ',') return -EINVAL;
			pos++;
		}
	}

	/* Map parsed array to Zephyr struct rtc_time with POSIX offsets */
	out->sec_o.stm.tm_mday = (int)parsed_vals[0];
	out->sec_o.stm.tm_mon  = (int)parsed_vals[1] - 1;
	out->sec_o.stm.tm_year = (int)parsed_vals[2] - 1900;
	out->sec_o.stm.tm_hour = (int)parsed_vals[3];
	out->sec_o.stm.tm_min  = (int)parsed_vals[4];
	out->sec_o.stm.tm_sec  = (int)parsed_vals[5];
	/* Note: tm_wday is generally calculated by the hardware or left at 0 */
	out->sec_o.stm.tm_wday = 0;

	return 0;
}
static int parse_sec_o(config_params_t *out)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct parse_sec_o p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 parse_sec_o_descr,
				 ARRAY_SIZE(parse_sec_o_descr), &p);

	if (ret < 0 || (ret & SEC_O_REQ_MASK) != SEC_O_REQ_MASK) {
		LOG_WRN("parse_sec_o: json_obj_parse failed ret=%d", ret);
		return -EINVAL;
	}

	/* fwv/hwv: read-only, never overwrite the firmware's values */
	out->sec_o.m4c = (uint8_t)p.m4c;
	out->sec_o.plt = (uint8_t)p.plt;

	/* Statistics counters - parsed from the pristine buffer, values may
	 * exceed INT32_MAX. */
	uint32_t tod, sct, rct, psc, plc, lct, tmn;

	if (parse_u32_field(s_rxbuf_orig, "TOD", &tod) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "SCT", &sct) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "RCT", &rct) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "PSC", &psc) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "PLC", &plc) < 0 ||
	    parse_u32_field(s_rxbuf_orig, "LCT", &lct) < 0||
		parse_u32_field(s_rxbuf_orig, "TMN", &tmn) < 0) {
		LOG_WRN("parse_sec_o: statistics fields parse failed");
		return -EINVAL;
	}
	out->sec_o.tod = tod;
	out->sec_o.sct = sct;
	out->sec_o.rct = rct;
	out->sec_o.psc = psc;
	out->sec_o.plc = plc;
	out->sec_o.lct = lct;
	out->sec_o.tmn = tmn;

	/*
	 * STM: raw 7-byte array, parsed and bounds-checked as opaque data.
	 * The Configuration Module does not interpret or act on it in any
	 * way (no RTC access) - the application receives the raw bytes via
	 * CFG_EVT_WRITE_REQ and decides what, if anything, to do with them.
	 */
	if (parse_sec_o_stm(out) < 0) {
		LOG_WRN("parse_sec_o: STM array parse failed");
		return -EINVAL;
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Parse dispatch table
 * --------------------------------------------------------------------- */

typedef int (*parse_fn_t)(config_params_t *);

static const parse_fn_t parse_table[CONFIG_SEC_MAX] = {
	[CONFIG_SEC_A] = parse_sec_a,
	[CONFIG_SEC_B] = parse_sec_b,
	[CONFIG_SEC_C] = parse_sec_c,
	[CONFIG_SEC_D] = parse_sec_d,
	[CONFIG_SEC_E] = parse_sec_e,
	[CONFIG_SEC_F] = parse_sec_f,
	[CONFIG_SEC_G] = parse_sec_g,
	[CONFIG_SEC_H] = parse_sec_h,
	[CONFIG_SEC_I] = parse_sec_i,
	[CONFIG_SEC_J] = parse_sec_j,
	[CONFIG_SEC_K] = parse_sec_k,
	[CONFIG_SEC_L] = parse_sec_l,
	[CONFIG_SEC_M] = parse_sec_m,
	[CONFIG_SEC_N] = parse_sec_n,
	[CONFIG_SEC_O] = parse_sec_o,
};

/* -----------------------------------------------------------------------
 * MTP "00" Configuration Type (Factory / User)
 * --------------------------------------------------------------------- */

struct pkt_config_type {
	int32_t ctp;
};

static const struct json_obj_descr config_type_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct pkt_config_type, "CTP", ctp, JSON_TOK_NUMBER),
};

#define CTP_REQ_MASK  BIT(0)

static void handle_config_type_request(void)
{
	char work[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(work, sizeof(work));

	struct pkt_config_type p = {0};
	int ret = json_obj_parse(work, strlen(work),
				 config_type_descr,
				 ARRAY_SIZE(config_type_descr), &p);

	if (ret < 0 || (ret & CTP_REQ_MASK) != CTP_REQ_MASK ||
	    (p.ctp != (int32_t)CFG_TYPE_FACTORY && p.ctp != (int32_t)CFG_TYPE_USER)) {
		LOG_WRN("MTP 00: missing or invalid CTP");
		cfg_send_ack_nack(false, 0);
		return;
	}

	config_type_req_t req = { .type = (config_type_t)p.ctp };

	LOG_DBG("Configuration Type: %s",
		(p.ctp == (int32_t)CFG_TYPE_FACTORY) ? "FACTORY" : "USER");

	cfg_notify_app(CFG_EVT_CONFIG_TYPE_REQ, &req);
}

/* -----------------------------------------------------------------------
 * Inbound packet dispatch
 * --------------------------------------------------------------------- */

static void handle_read_request(const struct pkt_envelope *env)
{
	if (env->sid == NULL || env->sid_len != 1) {
		LOG_WRN("MTP 03: missing or invalid SID");
		cfg_send_ack_nack(false, 3);
		return;
	}

	char sec_id = env->sid[0];

	if (sec_id < 'A' || sec_id > 'O') {
		LOG_WRN("MTP 03: invalid SID '%c'", sec_id);
		cfg_send_ack_nack(false, 3);
		return;
	}

	k_mutex_lock(&cfg_ctx.lock, K_FOREVER);
	cfg_ctx.pending_sid = sec_id;
	k_mutex_unlock(&cfg_ctx.lock);

	LOG_DBG("Read request for section '%c'", sec_id);

	config_read_req_t req = { .sec_id = sec_id };

	cfg_notify_app(CFG_EVT_READ_REQ, &req);
}

static void handle_write_request(const struct pkt_envelope *env)
{
	if (env->sid == NULL || env->sid_len != 1) {
		LOG_WRN("MTP 05: missing or invalid SID");
		cfg_send_ack_nack(false, 5);
		return;
	}

	char sec_id = env->sid[0];

	if (sec_id < 'A' || sec_id > 'O') {
		LOG_WRN("MTP 05: invalid SID '%c'", sec_id);
		cfg_send_ack_nack(false, 5);
		return;
	}

	config_write_req_t wreq;

	memset(&wreq, 0, sizeof(wreq));
	wreq.sec_id = sec_id;

	/* Every parser below takes its own fresh copy of the pristine frame. */
	if (parse_table[sec_id - 'A'](&wreq.params) < 0) {
		LOG_WRN("MTP 05: parse failed for section '%c'", sec_id);
		cfg_send_ack_nack(false, 5);
		return;
	}

	if (!cfg_validate_section(sec_id, &wreq.params)) {
		LOG_WRN("MTP 05: section '%c' failed range validation", sec_id);
		cfg_send_ack_nack(false, 5);
		return;
	}

	LOG_DBG("Write request for section '%c' - OK", sec_id);
	cfg_notify_app(CFG_EVT_WRITE_REQ, &wreq);
}

static void handle_clear_request(const struct pkt_envelope *env)
{
	if (env->sid == NULL || env->sid_len != 1) {
		LOG_WRN("MTP 06: missing or invalid SID");
		cfg_send_ack_nack(false, 6);
		return;
	}

	char sec_id = env->sid[0];

	if (sec_id != '0' && (sec_id < 'A' || sec_id > 'O')) {
		LOG_WRN("MTP 06: invalid SID '%c'", sec_id);
		cfg_send_ack_nack(false, 6);
		return;
	}

	LOG_DBG("Clear request for section '%c'", sec_id);

	config_read_req_t req = { .sec_id = sec_id };

	cfg_notify_app(CFG_EVT_CLEAR_REQ, &req);
}

void cfg_json_dispatch(const char *json, size_t json_len)
{
	if (json_len == 0) {
		return;
	}

	if (frame_copy(json, json_len) < 0) {
		return;
	}

	/*
	 * Envelope parse (MTP/SID): take a fresh, disposable working copy
	 * of the pristine frame. json_obj_parse() will NUL-terminate the
	 * decoded MTP/SID strings in place inside this scratch buffer only;
	 * s_rxbuf_orig itself is never touched.
	 */
	char env_buf[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];

	fresh_copy(env_buf, sizeof(env_buf));

	struct pkt_envelope env = {0};

	int ret = json_obj_parse(env_buf, strlen(env_buf),
				 envelope_descr,
				 ARRAY_SIZE(envelope_descr), &env);

	if (ret < 0 || (ret & ENVELOPE_MTP_MASK) == 0 || env.mtp == NULL) {
		LOG_WRN("Packet missing MTP field (ret=%d)", ret);
		return;
	}

	env.mtp_len = strlen(env.mtp);
	if (env.sid != NULL) {
		env.sid_len = strlen(env.sid);
	}

	if (env.mtp_len == 2 && env.mtp[0] == '0') {
		switch (env.mtp[1]) {
		case '0':
			handle_config_type_request();
			return;
		case '1':
			LOG_DBG("Received ACK/NACK from client (MTP 01)");
			return;
		case '3':
			handle_read_request(&env);
			return;
		case '5':
			handle_write_request(&env);
			return;
		case '6':
			handle_clear_request(&env);
			return;
		default:
			break;
		}
	}

	LOG_WRN("Unknown MTP \"%.*s\" ", (int)env.mtp_len, env.mtp);
}