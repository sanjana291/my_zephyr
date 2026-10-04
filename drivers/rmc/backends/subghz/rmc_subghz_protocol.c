/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rmc_subghz_protocol.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_DECLARE(rmc_subghz, CONFIG_RMC_LOG_LEVEL);


/* -------------------------------------------------------------------- */
/* Encode                                                                 */
/* -------------------------------------------------------------------- */

int rmc_sg_encode_beacon(uint8_t *buf, size_t buf_size,
			  const uint8_t *device_uid)
{
	/*
	 * Beacon: PTP(1) + PLD=device_uid(RMC_SG_DEVICE_UID_LEN)
	 */
	uint8_t frame_len = (uint8_t)( RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN );

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	buf[0] = RMC_SG_PTP_BEACON;
	memcpy(&buf[1], device_uid, RMC_SG_DEVICE_UID_LEN);

	return (int)frame_len;
}

int rmc_sg_encode_auth_req(uint8_t *buf, size_t buf_size,
			    const uint8_t *our_uid,
			    const uint8_t *shuttle_uid)
{

	uint8_t token[] = RMC_SG_AUTH_REQ_TOKEN;
	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN
						+ RMC_SG_REMOTE_UID_LEN   // our_uid (remote = 12 bytes)
						+ RMC_SG_DEVICE_UID_LEN   // shuttle_uid (8 bytes)
						+ RMC_SG_AUTH_TOKEN_LEN); // token (8 bytes)
	uint8_t *p = buf;

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_AUTH_REQ;
	memcpy(p, our_uid,     RMC_SG_REMOTE_UID_LEN); p += RMC_SG_REMOTE_UID_LEN;
	memcpy(p, shuttle_uid, RMC_SG_DEVICE_UID_LEN); p += RMC_SG_DEVICE_UID_LEN;
	memcpy(p, token,       RMC_SG_AUTH_TOKEN_LEN);

	return (int)frame_len;
}

int rmc_sg_encode_auth_conf(uint8_t *buf, size_t buf_size,
			     const uint8_t *our_uid)
{
	/*
	 * Auth Confirmation: HDR(1) + UID(12) + PLD(8 token)
	 */
	const uint8_t token[] = RMC_SG_AUTH_CONF_TOKEN;
	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
				       + RMC_SG_AUTH_TOKEN_LEN );
	uint8_t *p = buf;

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_AUTH_CONF;
	memcpy(p, our_uid, RMC_SG_REMOTE_UID_LEN); p += RMC_SG_REMOTE_UID_LEN;
	memcpy(p, token, RMC_SG_AUTH_TOKEN_LEN);    p += RMC_SG_AUTH_TOKEN_LEN;

	return (int)frame_len;
}

int rmc_sg_encode_cfg_write(uint8_t *buf, size_t buf_size,
			     const uint8_t *our_uid,
			     const struct rmc_sg_pkt_cfg_write *cfg)
{
	/*
	 * Config Write: HDR(1) + UID(12) + PLD(8 net + 1 ch + 1 node + 1 remote)
	 */
	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
				       + 8U + 1U + 1U + 1U);
	uint8_t *p = buf;

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_CFG_WRITE;
	memcpy(p, our_uid, RMC_SG_REMOTE_UID_LEN);  p += RMC_SG_REMOTE_UID_LEN;
	memcpy(p, cfg->network_id, 8U);              p += 8U;
	*p++ = cfg->channel_id;
	*p++ = cfg->node_id;
	*p++ = cfg->remote_id;

	return (int)frame_len;
}

int rmc_sg_encode_cfg_ok(uint8_t *buf, size_t buf_size,
			  const uint8_t *our_uid,
			  uint8_t status)
{
	/*
	 * Config Write Success: HDR(1) + UID(12) + PLD(1 status)
	 * = 4 + 12 + 1 + 2 = 19 bytes. Table says 18; using actual count.
	 */
	uint8_t frame_len = (uint8_t)( RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
				       + 1U);
	uint8_t *p = buf;

	if (buf_size < frame_len) {
		return -ENOMEM;
	}


	*p++ = RMC_SG_PTP_CFG_OK;
	memcpy(p, our_uid, RMC_SG_REMOTE_UID_LEN); p += RMC_SG_REMOTE_UID_LEN;
	*p++ = status;

	return (int)frame_len;
}

int rmc_sg_encode_pairing_sts(uint8_t *buf, size_t buf_size,
			       const uint8_t *our_uid,
			       uint8_t status)
{
	/*
	 * Pairing Status: HDR(1) + UID(12) + PLD(1 status)
	 */
	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
				       + 1U);
	uint8_t *p = buf;

	if (buf == NULL || our_uid == NULL) {
		return -EINVAL;
	}

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_PAIRING_STS;
	memcpy(p, our_uid, RMC_SG_REMOTE_UID_LEN); p += RMC_SG_REMOTE_UID_LEN;
	*p++ = status;

	return (int)frame_len;
}

int rmc_sg_encode_data(uint8_t *buf, size_t buf_size,
			const uint8_t *msg, uint8_t msg_len)
{
	/*
	 * Data Communication: HDR(1) + UID(12) + PLD(msg_len)
	 */
	uint8_t pld_len =  msg_len;
	uint8_t frame_len = (uint8_t)(pld_len + 1 );
	uint8_t *p = buf;

	if (buf_size < frame_len || msg_len > RMC_SG_MAX_DATA_PLD) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_DATA_COMM;

	if (msg_len > 0U && msg != NULL) {
		memcpy(p, msg, msg_len);
		p += msg_len;
	}

	return (int)frame_len;
}

int rmc_sg_encode_ack(uint8_t *buf, size_t buf_size, const uint8_t ack)
{

	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN + RMC_SG_HDR_LEN);
	uint8_t *p = buf;

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	*p++ = RMC_SG_PTP_ACK;
	*p = ack;

	return (int)frame_len;
}

int rmc_sg_encode_read_resp(uint8_t *buf, size_t buf_size,
			    const struct rmc_sg_pkt_cfg_read *cfg)
{
	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN +
				      RMC_SG_REMOTE_UID_LEN +
				      8U + 1U + 1U + 1U +
				      2U + 2U);
	uint8_t *p = buf;

	if (buf == NULL || cfg == NULL) {
		return -EINVAL;
	}

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	/* PTP */
	*p++ = RMC_SG_PTP_CFG_READ_RSP;

	/* Remote UID */
	memcpy(p, cfg->remote_uid, RMC_SG_REMOTE_UID_LEN);
	p += RMC_SG_REMOTE_UID_LEN;

	/* Network ID */
	memcpy(p, cfg->network_id, 8U);
	p += 8U;

	/* Configuration */
	*p++ = cfg->channel_id;
	*p++ = cfg->node_id;
	*p++ = cfg->remote_id;
	/* FW version */
	*p++ = (uint8_t)(cfg->fw_ver & 0xFFU);
	*p++ = (uint8_t)((cfg->fw_ver >> 8) & 0xFFU);

	/* HW version */
	*p++ = (uint8_t)(cfg->hw_ver & 0xFFU);
	*p++ = (uint8_t)((cfg->hw_ver >> 8) & 0xFFU);

	return (int)frame_len;
}
int rmc_sg_encode_scan_response(uint8_t *buf, size_t buf_size,
				const uint8_t *device_uid)
{

	uint8_t frame_len = (uint8_t)(RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN);

	if (buf == NULL || device_uid == NULL) {
		return -EINVAL;
	}

	if (buf_size < frame_len) {
		return -ENOMEM;
	}

	buf[0] = RMC_SG_SCAN_RESPONSE;
	memcpy(&buf[1], device_uid, RMC_SG_DEVICE_UID_LEN);

	return (int)frame_len;
}

/* -------------------------------------------------------------------- */
/* Decode                                                                 */
/* -------------------------------------------------------------------- */

int rmc_sg_decode(const uint8_t *buf, uint8_t len,
		  struct rmc_sg_decoded *out)
{
	const uint8_t *p;

	if (buf == NULL || out == NULL || len < RMC_SG_HDR_LEN) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	out->hdr.ptp = buf[0];
	p = &buf[RMC_SG_HDR_LEN];


	switch (out->hdr.ptp) {
	case RMC_SG_PTP_BEACON:
		if (len < RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN) {
			return -EBADMSG;
		}
		memcpy(out->beacon.device_uid, p, RMC_SG_DEVICE_UID_LEN);
		return 0;

	case RMC_SG_PTP_AUTH_REQ:
		if (len < RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
		          + RMC_SG_DEVICE_UID_LEN + RMC_SG_AUTH_TOKEN_LEN) {
			return -EBADMSG;
		}
		memcpy(out->auth_req.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
		p += RMC_SG_REMOTE_UID_LEN;
		memcpy(out->auth_req.shuttle_uid, p, RMC_SG_DEVICE_UID_LEN);
		p += RMC_SG_DEVICE_UID_LEN;
		memcpy(out->auth_req.token, p, RMC_SG_AUTH_TOKEN_LEN);
		return 0;

	case RMC_SG_PTP_AUTH_CONF:
		/* Auth Conf: UID(12) + token(8). */
		if (len < RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
		          + RMC_SG_AUTH_TOKEN_LEN) {
			return -EBADMSG;
		}
		memcpy(out->auth_conf.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
		p += RMC_SG_REMOTE_UID_LEN;
		memcpy(out->auth_conf.token, p, RMC_SG_AUTH_TOKEN_LEN);
		return 0;

	case RMC_SG_PTP_CFG_WRITE:
		if (len < RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN
		          + 8U + 1U + 1U + 1U ) {
			return -EBADMSG;
		}
		memcpy(out->cfg_write.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
		p += RMC_SG_REMOTE_UID_LEN;
		memcpy(out->cfg_write.network_id, p, 8U);
		p += 8U;
		out->cfg_write.channel_id = *p++;
		out->cfg_write.node_id    = *p++;
		out->cfg_write.remote_id  = *p;
		return 0;

	case RMC_SG_PTP_CFG_OK:
		/* Config OK: UID(12) + status(1). */
		if (len < RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
		          + 1U ) {
			return -EBADMSG;
		}
		memcpy(out->cfg_ok.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
		p += RMC_SG_REMOTE_UID_LEN;
		out->cfg_ok.status = *p;
		return 0;

	case RMC_SG_PTP_PAIRING_STS:
		/* Pairing Status: UID(12) + status(1). */
		if (len < RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
		          + 1U ) {
			return -EBADMSG;
		}
		memcpy(out->pairing_sts.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
		p += RMC_SG_REMOTE_UID_LEN;
		out->pairing_sts.status = *p;
		return 0;

	case RMC_SG_PTP_DATA_COMM: {
		uint8_t pld_offset = RMC_SG_HDR_LEN;
		uint8_t min_len = (uint8_t)(pld_offset);

		if (len < min_len) {
			return -EBADMSG;
		}

		out->data.msg_len = len - min_len;
		if (out->data.msg_len > RMC_SG_MAX_DATA_PLD) {
			return -EBADMSG;
		}
		if (out->data.msg_len > 0U) {
			memcpy(out->data.msg, p, out->data.msg_len);
		}
		return 0;
	}

	case RMC_SG_PTP_ACK:
		if (len < RMC_SG_HDR_LEN) {
			return -EBADMSG;
		}
		return 0;

	case RMC_SG_PTP_CFG_READ:
	case RMC_SG_PTP_CFG_READ_RSP:
		if (out->hdr.ptp == RMC_SG_PTP_CFG_READ_RSP) {
			if (len < RMC_SG_HDR_LEN + RMC_SG_REMOTE_UID_LEN
			          + 8U + 1U + 1U + 1U ) {
				return -EBADMSG;
			}
			memcpy(out->cfg_read.remote_uid, p, RMC_SG_REMOTE_UID_LEN);
			p += RMC_SG_REMOTE_UID_LEN;
			memcpy(out->cfg_read.network_id, p, 8U);
			p += 8U;
			out->cfg_read.channel_id = *p++;
			out->cfg_read.node_id    = *p++;
			out->cfg_read.remote_id  = *p++;
			// memcpy(&out->cfg_read.fw_ver, p,
			// 	sizeof(out->cfg_read.fw_ver));
			// p += sizeof(out->cfg_read.fw_ver);

			// memcpy(&out->cfg_read.hw_ver, p,
			// 	sizeof(out->cfg_read.hw_ver));
			// p += sizeof(out->cfg_read.hw_ver);
		}
		return 0;
	case RMC_SG_CONNECT_REQ:
		out->ack.ack_pkt = RMC_SG_CONNECT_REQ;
		return 0;
	case RMC_SG_DISCONNECT_REQ:
		out->ack.ack_pkt = RMC_SG_DISCONNECT_REQ;
		return 0;

	case RMC_SG_SCAN_RESPONSE:
		/*
		 * Scan Response (0x0D): PTP(1) + shuttle device_uid(8).
		 * Sent by the shuttle after receiving a beacon; the remote
		 * uses the embedded UID to confirm this response is from the
		 * shuttle it is targeting before extending its beacon window.
		 */
		if (len < RMC_SG_HDR_LEN + RMC_SG_DEVICE_UID_LEN) {
			return -EBADMSG;
		}
		memcpy(out->scan_resp.device_uid, p, RMC_SG_DEVICE_UID_LEN);
		return 0;

	default:
		LOG_WRN("SubGHz: unknown PTP 0x%02x", out->hdr.ptp);
		return -ENOMSG;
	}
}