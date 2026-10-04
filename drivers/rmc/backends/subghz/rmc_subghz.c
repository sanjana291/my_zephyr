/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC Sub-GHz backend: pairing state machine + working mode.
 *
 */

#include "rmc_subghz.h"
#include "../../rmc_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>
#include "sx127x_reg.h"

LOG_MODULE_REGISTER(rmc_subghz, CONFIG_RMC_LOG_LEVEL);

/* -------------------------------------------------------------------- */
/* Radio parameters (fixed by protocol specification)                    */
/* -------------------------------------------------------------------- */

#define RMC_SG_BITRATE_BPS      1200U
#define RMC_SG_FDEV_HZ          2500U
#define RMC_SG_RX_BW_REG        SX1278_RX_BW_10_4
#define RMC_SG_AFC_BW_REG       SX1278_RX_BW_20_8
#define RMC_SG_PREAMBLE_LEN		48U
#define RMC_SG_SYNC_LEN			8U
#define RMC_SG_SHAPING			FSK_SHAPING_GAUSSIAN_BT0_5
#define RMC_SG_DC_FREE			FSK_DC_FREE_WHITENING
#define RMC_SG_PA_POWER			20
#define RMC_SG_PAIRING_PA_POWER	0
#define RMC_SG_ADDR_FILTER      FSK_ADDR_FILTER_NODE
#define RMC_SG_PACKET_FORMAT	FSK_PACKET_VARIABLE

static struct rmc_subghz_data rmc_subghz_singleton;
static bool rmc_subghz_singleton_used;

K_THREAD_STACK_DEFINE(rmc_sg_sm_stack, RMC_SUBGHZ_SM_STACK_SIZE);

/* -------------------------------------------------------------------- */
/* Forward declarations                                                   */
/* -------------------------------------------------------------------- */

static void sg_sm_dispatch(struct rmc_subghz_data *d,
			    const struct rmc_sg_sm_msg *msg);
static void sg_enter_working_ready(struct rmc_subghz_data *d);
static void sg_handle_pairing_sts(struct rmc_subghz_data *d,
				   const struct rmc_sg_decoded *pkt);

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
/* -------------------------------------------------------------------- */

static void sg_notify(const struct device *dev, uint8_t event_type,
		       uint8_t event_code, const uint8_t *payload,
		       uint16_t payload_len)
{
	struct rmc_data *rdata = dev->data;
	rmc_callback_t cb = rdata->callback;
	uint8_t buf[256];
	uint8_t total_len;

	if (cb == NULL) {
		LOG_DBG("SubGHz: callback is NULL, not notifying");
		return;
	}
	if (payload == NULL) {
		payload_len = 0U;
	}
	if (payload_len > (uint16_t)(sizeof(buf) - 1U)) {
		payload_len = (uint16_t)(sizeof(buf) - 1U);
	}

	buf[0] = event_code;
	if (payload_len > 0U) {
		memcpy(&buf[1], payload, payload_len);
	}
	total_len = (uint8_t)(1U + payload_len);

	cb(dev, event_type, buf, total_len);
}

static void on_fsk_rx(const struct device *fsk_dev, const uint8_t *payload,
		       uint8_t len, const struct fsk_rx_metadata *meta,
		       void *user_data)
{
	ARG_UNUSED(fsk_dev);
	struct rmc_subghz_data *d = (struct rmc_subghz_data *)user_data;

	if (d == NULL) {
		LOG_ERR("FSK RX callback: user_data is NULL");
		return;
	}

	LOG_INF("Node 0x%02X", meta->src_addr);
	LOG_INF("Len : %u bytes", len);
	LOG_INF("RSSI : %d dBm", meta->rssi);
	LOG_INF("AFC  : %+d Hz", meta->afc_hz);

	if (len > 0) {
		LOG_HEXDUMP_INF(payload, len, "RAW:");
	}

	if (len == 0 || len > RMC_SG_MAX_FRAME_LEN) {
		LOG_WRN("RX packet dropped: invalid length %u", len);
		return;
	}

	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_RX_PACKET };
	msg.rx.len = len;
	memcpy(msg.rx.buf, payload, len);

	if (k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("RX packet dropped: state machine queue full");
	}
}

static void set_state(struct rmc_subghz_data *d, enum rmc_sg_state s)
{
	d->state = s;
	atomic_set(&d->ready, s == RMC_SG_STATE_WORKING_READY ? 1 : 0);
}

static int radio_configure(struct rmc_subghz_data *d, uint8_t channel_id)
{
	struct fsk_config cfg;
	uint32_t freq;

	if (channel_id >= RMC_SG_CHANNEL_COUNT) {
		LOG_ERR("SubGHz: invalid channel %u", channel_id);
		return -EINVAL;
	}

	freq = rmc_sg_channel_freq_hz[channel_id];

	memset(&cfg, 0, sizeof(cfg));

	cfg.frequency_hz   = freq;
	cfg.bitrate_bps    = RMC_SG_BITRATE_BPS;
	cfg.fdev_hz        = RMC_SG_FDEV_HZ;
	cfg.rx_bw_reg      = RMC_SG_RX_BW_REG;
	cfg.afc_bw_reg     = RMC_SG_AFC_BW_REG;
	cfg.preamble_len   = RMC_SG_PREAMBLE_LEN;
	cfg.sync_len       = RMC_SG_SYNC_LEN;
	memcpy(cfg.sync_word, d->working_cfg.network_id, sizeof(cfg.sync_word));
	cfg.packet_format  = RMC_SG_PACKET_FORMAT;
	cfg.payload_len    = RMC_SG_MAX_FRAME_LEN;
	cfg.dc_free        = RMC_SG_DC_FREE;
	cfg.crc_on         = true;
	cfg.afc_on         = true;
	cfg.addr_filter    = RMC_SG_ADDR_FILTER;
	cfg.shaping        = RMC_SG_SHAPING;
	cfg.tx_power_dbm   = RMC_SG_PA_POWER;
	cfg.pa_boost       = true;
	cfg.max_retries    = CONFIG_RMC_SUBGHZ_RETRY_COUNT;
	cfg.node_addr      = d->working_cfg.node_id;
	cfg.broadcast_addr = 0xff;

	LOG_INF("SubGHz: configuring radio ch%u = %u Hz, bitrate=%u, fdev=%u, node_addr=0x%02x",
		channel_id, freq, cfg.bitrate_bps, cfg.fdev_hz, cfg.node_addr);
	int ret = fsk_configure(d->hw->radio, &cfg);

	if (ret) {
		return RMC_ERR_GENERAL;
	}
	return RMC_OK;
}

static int radio_send_msg(struct rmc_subghz_data *d, uint8_t dest,
			   const uint8_t *buf, uint8_t len)
{
	return fsk_send_to(d->hw->radio, dest, buf, len,
			   K_MSEC(CONFIG_RMC_SUBGHZ_DEFAULT_TX_TIMEOUT_MS));
}

static void read_device_uid(uint8_t *uid)
{
	memset(uid, 0, RMC_SG_DEVICE_UID_LEN);
	uint32_t *uuid_l = (uint32_t *)(0x40CAC000 + 0x900);
	uint32_t *uuid_h = (uint32_t *)(0x40CAC000 + 0x910);
	uint64_t uid64 = ((uint64_t)(*uuid_h) << 32) | (*uuid_l);

	for (int i = 0; i < 8; i++) {
		uid[7 - i] = (uint8_t)(uid64 >> (8 * i));
	}
}

static bool sg_cfg_is_empty(const struct rmc_subghz_cfg *cfg)
{
	static const struct rmc_subghz_cfg zero_cfg;

	return memcmp(cfg, &zero_cfg, sizeof(zero_cfg)) == 0;
}

static void beacon_tick_handler(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct rmc_subghz_data *d = CONTAINER_OF(dw, struct rmc_subghz_data,
						   beacon_tick_work);
	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_BEACON_TICK };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

static void pairing_timeout_handler(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct rmc_subghz_data *d = CONTAINER_OF(dw, struct rmc_subghz_data,
						   pairing_timeout_work);
	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_PAIRING_TIMEOUT };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

static void pairing_exchange_timeout_handler(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct rmc_subghz_data *d = CONTAINER_OF(dw, struct rmc_subghz_data,
						   pairing_exchange_timeout_work);
	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_PAIRING_EXCHANGE_TIMEOUT };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

/**
 * @brief Work handler: per-beacon scan-response window expired.
 *
 * Fired CONFIG_RMC_SUBGHZ_PAIRING_SCAN_RESP_TIMEOUT_MS after the most
 * recent beacon TX if no matching SCAN_RESPONSE (0x0D) has arrived.
 * Posting RMC_SG_EVT_SCAN_RESP_TIMEOUT lets the state machine know the
 * extension window is closed so it will not extend pairing_timeout_work
 * again for this beacon cycle.
 */
static void scan_resp_timeout_handler(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct rmc_subghz_data *d = CONTAINER_OF(dw, struct rmc_subghz_data,
						   scan_resp_timeout_work);
	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_SCAN_RESP_TIMEOUT };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

static int apply_radio_pairing_config(struct rmc_subghz_data *d)
{
	struct fsk_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	uint32_t freq = rmc_sg_channel_freq_hz[RMC_SG_PAIRING_CHANNEL];

	cfg.frequency_hz   = freq;
	cfg.bitrate_bps    = RMC_SG_BITRATE_BPS;
	cfg.fdev_hz        = RMC_SG_FDEV_HZ;
	cfg.rx_bw_reg      = RMC_SG_RX_BW_REG;
	cfg.afc_bw_reg     = RMC_SG_AFC_BW_REG;
	cfg.preamble_len   = RMC_SG_PREAMBLE_LEN;
	cfg.sync_len       = RMC_SG_SYNC_LEN;
	memset(&cfg.sync_word, 0xff, sizeof(cfg.sync_word));
	cfg.packet_format  = RMC_SG_PACKET_FORMAT;
	cfg.payload_len    = RMC_SG_MAX_FRAME_LEN;
	cfg.dc_free        = RMC_SG_DC_FREE;
	cfg.crc_on         = true;
	cfg.afc_on         = true;
	cfg.addr_filter    = RMC_SG_ADDR_FILTER;
	cfg.shaping        = RMC_SG_SHAPING;
	cfg.tx_power_dbm   = RMC_SG_PAIRING_PA_POWER;
	cfg.pa_boost       = true;
	cfg.node_addr      = RMC_SG_PAIRING_ADDR_SHUTTLE;
	cfg.broadcast_addr = 0xff;
	cfg.max_retries    = CONFIG_RMC_SUBGHZ_RETRY_COUNT;

	LOG_INF("SubGHz Pairing: configuring radio ch%u = %u Hz",
		RMC_SG_PAIRING_CHANNEL, cfg.frequency_hz);
	int ret = fsk_configure(d->hw->radio, &cfg);

	return (ret != 0) ? -EIO : 0;
}


static void sg_start_pairing(struct rmc_subghz_data *d)
{

	int ret = apply_radio_pairing_config(d);

	if (ret != 0) {
		LOG_ERR("SubGHz: pairing channel config failed: %d", ret);
		sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_UNKNOWN, NULL, 0);
		return;
	}

	d->remote_uid_known = false;
	set_state(d, RMC_SG_STATE_PAIRING_BEACON);


	k_work_schedule(&d->pairing_timeout_work,
			K_MSEC(CONFIG_RMC_SUBGHZ_PAIRING_TIMEOUT_MS));

	/* Kick off the first beacon immediately. */
	struct rmc_sg_sm_msg tick = { .evt = RMC_SG_EVT_BEACON_TICK };

	(void)k_msgq_put(&d->sm_msgq, &tick, K_NO_WAIT);

	LOG_INF("SubGHz: beaconing started (ch6 = 434.70 MHz, beacon window = %u ms)",
		CONFIG_RMC_SUBGHZ_PAIRING_TIMEOUT_MS);
}

/*
 * Enter working mode after a LIVE pairing exchange (cfg_write received).
 * The remote is already talking to us - go straight to WORKING_READY.
 */
static void sg_enter_working_ready(struct rmc_subghz_data *d)
{
    int ret = radio_configure(d, d->working_cfg.channel_id);

    if (ret != 0) {
        LOG_ERR("SubGHz: working channel config failed: %d", ret);
        sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_UNKNOWN, NULL, 0);
        return;
    }

    set_state(d, RMC_SG_STATE_WORKING_READY);
    LOG_INF("SubGHz: WORKING_READY on ch%u (node=0x%02x remote=0x%02x)",
        d->working_cfg.channel_id,
        d->working_cfg.node_id,
        d->working_cfg.remote_id);

    sg_notify(d->dev, RMC_EVENT_WIRELESS_CONN,
               RMC_WIRELESS_CONN_CONNECTED, NULL, 0);
    sg_notify(d->dev, RMC_EVENT_REMOTE_CONN,
               RMC_REMOTE_CONN_CONNECTED, NULL, 0);

	uint8_t pair_cfg[RMC_SG_SYNC_LEN + 3];
	uint8_t *p = pair_cfg;

	memcpy(p, d->working_cfg.network_id, RMC_SG_SYNC_LEN);
	p += RMC_SG_SYNC_LEN;

	*p++ = d->working_cfg.channel_id;
	*p++ = d->working_cfg.remote_id;
	*p++ = d->working_cfg.node_id;

	sg_notify(d->dev,
			RMC_EVENT_PAIRING,
			RMC_PAIRING_SUCCESS,
			pair_cfg,
			sizeof(pair_cfg));
}

static void sg_enter_working_wait_conn(struct rmc_subghz_data *d)
{
    int ret = radio_configure(d, d->working_cfg.channel_id);

    if (ret != 0) {
        LOG_ERR("SubGHz: working channel config failed: %d", ret);
        sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_UNKNOWN, NULL, 0);
        return;
    }

    set_state(d, RMC_SG_STATE_WORKING_WAIT_CONN);
    LOG_INF("SubGHz: WORKING_WAIT_CONN on ch%u - waiting for shuttle "
            "connection request (node=0x%02x remote=0x%02x)",
        d->working_cfg.channel_id,
        d->working_cfg.node_id,
        d->working_cfg.remote_id);

    /* Tell the app the radio link is up, but not the remote yet. */
    sg_notify(d->dev, RMC_EVENT_WIRELESS_CONN,
               RMC_WIRELESS_CONN_CONNECTED, NULL, 0);
}

/* -------------------------------------------------------------------- */
/* Pairing state handlers                                               */
/* -------------------------------------------------------------------- */

// static void sg_handle_beacon_tick(struct rmc_subghz_data *d)
// {
// 	if (d->state != RMC_SG_STATE_PAIRING_BEACON) {
// 		return;
// 	}

// 	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
// 	int len = rmc_sg_encode_beacon(frame, sizeof(frame), d->device_uid);

// 	if (len > 0) {
// 		(void)radio_send_msg(d,RMC_SG_PAIRING_ADDR_REMOTE, frame, (uint8_t)len);
// 		LOG_DBG("SubGHz: beacon TX");
// 	}


// 	k_work_schedule(&d->beacon_tick_work, K_MSEC(6000));
// }

static void sg_handle_beacon_send(struct rmc_subghz_data *d)
{
	if (d->state != RMC_SG_STATE_PAIRING_BEACON) {
		return;
	}

	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
	int len = rmc_sg_encode_beacon(frame, sizeof(frame), d->device_uid);

	if (len > 0) {
		(void)radio_send_msg(d, RMC_SG_PAIRING_ADDR_REMOTE, frame, (uint8_t)len);
		LOG_DBG("SubGHz: beacon TX");


		d->scan_resp_extended = false;
		k_work_reschedule(&d->scan_resp_timeout_work,
				  K_MSEC(CONFIG_RMC_SUBGHZ_PAIRING_SCAN_RESP_TIMEOUT_MS));
		LOG_DBG("SubGHz: scan-response window armed (%u ms)",
			CONFIG_RMC_SUBGHZ_PAIRING_SCAN_RESP_TIMEOUT_MS);
	}
}

/**
 * @brief Handle an incoming Scan Response (0x0D) during the beacon phase.
 *
 */
static void sg_handle_scan_response(struct rmc_subghz_data *d,
				     const struct rmc_sg_decoded *pkt)
{
	if (d->state != RMC_SG_STATE_PAIRING_BEACON) {
		LOG_DBG("SubGHz: SCAN_RESPONSE ignored, not in PAIRING_BEACON state");
		return;
	}

	/* Drop if the scan-response window has already closed. */
	if (!k_work_delayable_is_pending(&d->scan_resp_timeout_work)) {
		LOG_DBG("SubGHz: SCAN_RESPONSE arrived after window closed, ignoring");
		return;
	}

	/* Verify the shuttle UID embedded in the scan response. */
	if (memcmp(pkt->scan_resp.device_uid, d->device_uid,
		   RMC_SG_DEVICE_UID_LEN) != 0) {
		LOG_WRN("SubGHz: SCAN_RESPONSE UID mismatch, ignoring");
		return;
	}

	if (d->scan_resp_extended) {
		LOG_DBG("SubGHz: SCAN_RESPONSE already extended this cycle, ignoring");
		return;
	}


	k_work_cancel_delayable(&d->scan_resp_timeout_work);
	d->scan_resp_extended = true;

	k_work_reschedule(&d->pairing_timeout_work,
			  K_MSEC(CONFIG_RMC_SUBGHZ_PAIRING_SCAN_RESP_TIMEOUT_MS));

	LOG_INF("SubGHz: SCAN_RESPONSE matched - beacon window extended by %u ms",
		CONFIG_RMC_SUBGHZ_PAIRING_SCAN_RESP_TIMEOUT_MS);

	sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_SCANING_EXTD, NULL, 0);
}

static void sg_handle_auth_req(struct rmc_subghz_data *d,
				const struct rmc_sg_decoded *pkt)
{
	if (d->state != RMC_SG_STATE_PAIRING_BEACON) {
		return;
	}

	if (memcmp(pkt->auth_req.shuttle_uid, d->device_uid,
		    RMC_SG_DEVICE_UID_LEN) != 0) {
		LOG_WRN("SubGHz: auth req UID mismatch, ignoring");
		return;
	}

	uint8_t token[] = RMC_SG_AUTH_REQ_TOKEN;
	if (memcmp(pkt->auth_req.token,
		   token,
		   RMC_SG_AUTH_TOKEN_LEN) != 0) {
		LOG_WRN("SubGHz: AUTH_REQ token mismatch");
		sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_FAILED_AUTHENTICATION, NULL, 0);
		return;
	}

	memcpy(d->remote_uid, pkt->auth_req.remote_uid, RMC_SG_REMOTE_UID_LEN);
	d->remote_uid_known = true;

	LOG_INF("SubGHz: auth request received from shuttle, notifying application");

	sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_REQUEST, NULL, 0);

	k_work_cancel_delayable(&d->beacon_tick_work);
	k_work_cancel_delayable(&d->pairing_timeout_work);
	k_work_cancel_delayable(&d->scan_resp_timeout_work);


	k_work_schedule(&d->pairing_exchange_timeout_work,
			K_MSEC(CONFIG_RMC_SUBGHZ_PAIRING_EXCHANGE_TIMEOUT_MS));

	LOG_INF("SubGHz: sending auth confirmation, exchange timeout = %u ms",
		CONFIG_RMC_SUBGHZ_PAIRING_EXCHANGE_TIMEOUT_MS);

	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
	int len = rmc_sg_encode_auth_conf(frame, sizeof(frame), d->remote_uid);

	if (len > 0) {
		(void)radio_send_msg(d, RMC_SG_PAIRING_ADDR_REMOTE, frame,
				     (uint8_t)len);
	}

	set_state(d, RMC_SG_STATE_PAIRING_WAIT_CFG);
}

/**
 * @brief Handle cfg_write: store config, send cfg_ok, then enter working
 *        mode. There is no persistence step - this device only remembers
 *        the config for the current power cycle (or until re-paired).
 */
static void sg_handle_cfg_write(struct rmc_subghz_data *d,
				 const struct rmc_sg_decoded *pkt)
{
	if (d->state != RMC_SG_STATE_PAIRING_WAIT_CFG) {
		return;
	}

	const struct rmc_sg_pkt_cfg_write *cfg = &pkt->cfg_write;

	if (cfg->channel_id > RMC_SG_PAIRING_CHANNEL) {
		LOG_ERR("SubGHz: cfg write invalid channel %u", cfg->channel_id);

		uint8_t frame[RMC_SG_MAX_FRAME_LEN];
		int len = rmc_sg_encode_cfg_ok(frame, sizeof(frame),
					        d->remote_uid, 0x01U);

		if (len > 0) {
			(void)radio_send_msg(d, RMC_SG_PAIRING_ADDR_REMOTE, frame,
					     (uint8_t)len);
		}
		return;
	}


	memcpy(d->working_cfg.network_id, cfg->network_id, 8U);
	d->working_cfg.channel_id = cfg->channel_id;
	d->working_cfg.node_id    = cfg->node_id;
	d->working_cfg.remote_id  = cfg->remote_id;
	memcpy(d->working_cfg.remote_uid, d->remote_uid, RMC_SG_REMOTE_UID_LEN);
	d->working_cfg.valid = true;

	LOG_INF("SubGHz: cfg write ch=%u node=0x%02x remote=0x%02x",
		cfg->channel_id, cfg->node_id, cfg->remote_id);

	/* Send cfg_ok (status = 0x00 = success). */
	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
	int len = rmc_sg_encode_cfg_ok(frame, sizeof(frame),
				        d->remote_uid, 0x00U);

	if (len > 0) {
		(void)radio_send_msg(d, RMC_SG_PAIRING_ADDR_REMOTE, frame,
				     (uint8_t)len);
	}


	set_state(d, RMC_SG_STATE_PAIRING_WAIT_STS);
}

/**
 * @brief Handle the final Pairing Status packet from the shuttle.
 *
 */
static void sg_handle_pairing_sts(struct rmc_subghz_data *d,
				   const struct rmc_sg_decoded *pkt)
{
	if (d->state != RMC_SG_STATE_PAIRING_WAIT_STS) {
		return;
	}

	if (!d->remote_uid_known ||
	    memcmp(pkt->pairing_sts.remote_uid, d->remote_uid,
		   RMC_SG_REMOTE_UID_LEN) != 0) {
		LOG_WRN("SubGHz: PAIRING_STS UID mismatch, ignoring");
		return;
	}

	k_work_cancel_delayable(&d->beacon_tick_work);
	k_work_cancel_delayable(&d->pairing_timeout_work);
	k_work_cancel_delayable(&d->pairing_exchange_timeout_work);
	k_work_cancel_delayable(&d->scan_resp_timeout_work);

	if (pkt->pairing_sts.status == RMC_SG_PAIRING_STS_OK) {
		LOG_INF("SubGHz: pairing status OK - entering working mode");

		sg_enter_working_ready(d);
	} else {
		LOG_WRN("SubGHz: pairing status FAIL (0x%02x) - discarding "
			"exchanged config", pkt->pairing_sts.status);

		/* Discard everything latched during this exchange. */
		memset(&d->working_cfg, 0, sizeof(d->working_cfg));
		d->remote_uid_known = false;

		set_state(d, RMC_SG_STATE_PAIRING_IDLE);
		sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_FAILED_RDBACK_STATUS, NULL, 0);
	}
}

/**
 * @brief Handle the 5-second beacon-window expiry.
 *
 */
static void sg_handle_pairing_timeout(struct rmc_subghz_data *d)
{
	/* Only act while we are still in the beaconing phase. */
	if (d->state != RMC_SG_STATE_PAIRING_BEACON) {
		return;
	}

	LOG_INF("SubGHz: 5-second beacon window expired");

	k_work_cancel_delayable(&d->beacon_tick_work);

	if (d->working_cfg.valid) {

		LOG_INF("SubGHz: pre-loaded config found after beacon window "
			"- ch%u node=0x%02x remote=0x%02x, entering WORKING_WAIT_CONN",
			d->working_cfg.channel_id,
			d->working_cfg.node_id,
			d->working_cfg.remote_id);
		sg_enter_working_wait_conn(d);
	} else {

		LOG_WRN("SubGHz: no pairing response "
			"- entering PAIRING_IDLE");

		set_state(d, RMC_SG_STATE_PAIRING_IDLE);
		sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_FAILED_TIMEOUT, NULL, 0);
	}
}

/**
 * @brief Handle the 20-second pairing exchange timeout.
 *
 */
static void sg_handle_pairing_exchange_timeout(struct rmc_subghz_data *d)
{
	if (d->state == RMC_SG_STATE_PAIRING_WAIT_STS) {
		LOG_WRN("SubGHz: pairing exchange timeout - PAIRING_STS never "
			"received within %u ms, treating as pairing failure",
			CONFIG_RMC_SUBGHZ_PAIRING_EXCHANGE_TIMEOUT_MS);

		k_work_cancel_delayable(&d->beacon_tick_work);

		/* Discard the unconfirmed config latched by cfg_write. */
		memset(&d->working_cfg, 0, sizeof(d->working_cfg));
		d->remote_uid_known = false;

		set_state(d, RMC_SG_STATE_PAIRING_IDLE);
		sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_FAILED_TIMEOUT, NULL, 0);
		return;
	}

	if (d->state != RMC_SG_STATE_PAIRING_WAIT_CFG) {
		return;
	}

	LOG_WRN("SubGHz: pairing exchange timeout - shuttle did not complete "
		"within %u ms", CONFIG_RMC_SUBGHZ_PAIRING_EXCHANGE_TIMEOUT_MS);

	k_work_cancel_delayable(&d->beacon_tick_work);

	if (d->working_cfg.valid) {

		LOG_INF("SubGHz: exchange timed out but pre-loaded config exists "
			"- entering WORKING_WAIT_CONN");
		sg_enter_working_wait_conn(d);
	} else {
		LOG_WRN("SubGHz: exchange timed out with pairing config "
			"- entering PAIRING_IDLE");

		set_state(d, RMC_SG_STATE_PAIRING_IDLE);
		sg_notify(d->dev, RMC_EVENT_PAIRING, RMC_PAIRING_FAILED_TIMEOUT, NULL, 0);
	}
}
/* -------------------------------------------------------------------- */
/* Working mode handlers                                                 */
/* -------------------------------------------------------------------- */

static void sg_handle_working_conn_req(struct rmc_subghz_data *d,
					const struct rmc_sg_decoded *pkt)
{
	ARG_UNUSED(pkt);

	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
	int len = rmc_sg_encode_auth_conf(frame, sizeof(frame), d->device_uid);

	if (len > 0) {
		(void)radio_send_msg(d, RMC_SG_PAIRING_ADDR_REMOTE, frame,
				     (uint8_t)len);
	}

	set_state(d, RMC_SG_STATE_WORKING_READY);
	LOG_INF("SubGHz: working mode ready");

	sg_notify(d->dev, RMC_EVENT_REMOTE_CONN,
		   RMC_REMOTE_CONN_CONNECTED, NULL, 0);
}

static void sg_handle_rx_data(struct rmc_subghz_data *d,
			       const struct rmc_sg_decoded *pkt)
{
	sg_notify(d->dev, RMC_EVENT_RX_DATA, RMC_RX_DATA_RECEIVED,
		   pkt->data.msg, (uint16_t)pkt->data.msg_len);
}

/* -------------------------------------------------------------------- */
/* TX request handler                                                    */
/* -------------------------------------------------------------------- */

static int sg_tx_data(struct rmc_subghz_data *d,
		       const struct rmc_sg_sm_msg *msg)
{
	if (d->state != RMC_SG_STATE_WORKING_READY) {
		sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM, NULL, 0);
		return RMC_ERR_NOT_CONN;
	}

	uint8_t frame[RMC_SG_MAX_FRAME_LEN];
	int len = rmc_sg_encode_data(frame, sizeof(frame),
				      msg->tx.data, msg->tx.data_len);

	if (len <= 0) {
		sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_INVALID_PACKET,
			  NULL, 0);
		return RMC_ERR_INVALID_ARG;
	}

	int ret = radio_send_msg(d, d->working_cfg.remote_id, frame,
				 (uint8_t)len);

	if (ret) {
		return RMC_ERR_GENERAL;
	}
	return RMC_OK;
}

static int sg_send_ack(struct rmc_subghz_data *d,
				const struct rmc_sg_pkt_ack *ack)
{
	uint8_t frame[RMC_SG_HDR_LEN + RMC_SG_HDR_LEN];

	int len = rmc_sg_encode_ack(frame, sizeof(frame), ack->ack_pkt);

	if (len <= 0) {
		return RMC_ERR_INVALID_ARG;
	}

	int ret = radio_send_msg(d, d->working_cfg.remote_id, frame,
				 (uint8_t)len);

	if (ret) {
		return RMC_ERR_GENERAL;
	}
	return RMC_OK;
}

static int sg_read_resp(struct rmc_subghz_data *d,
			const struct rmc_sg_pkt_cfg_read *rd)
{
	uint8_t frame[RMC_SG_HDR_LEN +
		      RMC_SG_REMOTE_UID_LEN +
		      RMC_SG_SYNC_LEN +
		      1U + 1U + 1U +
		      2U + 2U];

	struct rmc_sg_pkt_cfg_read cfg;
	int len;
	int ret;

	if (d == NULL || rd == NULL) {
		return RMC_ERR_INVALID_ARG;
	}

	memset(&cfg, 0, sizeof(cfg));

	/*
	 * Fill the response with our current configuration.
	 */
	memcpy(cfg.remote_uid,
	       d->working_cfg.remote_uid,
	       RMC_SG_REMOTE_UID_LEN);

	memcpy(cfg.network_id,
	       d->working_cfg.network_id,
	       RMC_SG_SYNC_LEN);

	cfg.channel_id = d->working_cfg.channel_id;
	cfg.node_id    = d->working_cfg.node_id;
	cfg.remote_id  = d->working_cfg.remote_id;

	cfg.fw_ver = d->working_cfg.fw_ver;
	cfg.hw_ver = d->working_cfg.hw_ver;

	/*
	 * Encode Configuration Read Response.
	 */
	len = rmc_sg_encode_read_resp(frame, sizeof(frame), &cfg);

	if (len <= 0) {
		LOG_ERR("SubGHz: failed to encode CFG_READ_RSP: %d", len);
		return RMC_ERR_INVALID_PACKET;
	}

	/*
	 * Send response to the requesting remote. Still uses the pairing
	 * address - the shuttle is on the pairing channel/address until
	 * both sides move over to the working link below.
	 */
	ret = radio_send_msg(d,
			     RMC_SG_PAIRING_ADDR_REMOTE,
			     frame,
			     (uint8_t)len);

	if (ret != 0) {
		LOG_ERR("SubGHz: failed to send CFG_READ_RSP: %d", ret);
		return RMC_ERR_GENERAL;
	}

	return RMC_OK;
}
/* -------------------------------------------------------------------- */
/* RX dispatch                                                           */
/* -------------------------------------------------------------------- */

static void sg_dispatch_rx(struct rmc_subghz_data *d,
			    const struct rmc_sg_sm_msg *msg)
{
	struct rmc_sg_decoded pkt;
	int ret = rmc_sg_decode(msg->rx.buf, msg->rx.len, &pkt);

	if (ret != 0) {
		LOG_WRN("SubGHz: decode error %d", ret);
		sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_INVALID_PACKET,
			  NULL, 0);
		return;
	}

	switch (pkt.hdr.ptp) {
	case RMC_SG_PTP_AUTH_REQ:
		if (d->state == RMC_SG_STATE_PAIRING_BEACON) {
			sg_handle_auth_req(d, &pkt);
		} else if (d->state == RMC_SG_STATE_WORKING_WAIT_CONN) {
			sg_handle_working_conn_req(d, &pkt);
		}
		break;

	case RMC_SG_PTP_CFG_WRITE:
		sg_handle_cfg_write(d, &pkt);
		break;

	case RMC_SG_PTP_PAIRING_STS:
		sg_handle_pairing_sts(d, &pkt);
		break;

	case RMC_SG_PTP_DATA_COMM:
		if (d->state == RMC_SG_STATE_WORKING_READY) {
			sg_handle_rx_data(d, &pkt);
		}
		break;

	case RMC_SG_CONNECT_REQ:
		if(d->state != RMC_SG_STATE_WORKING_READY){
			sg_send_ack(d, &pkt.ack);
			set_state(d, RMC_SG_STATE_WORKING_READY);
			sg_notify(d->dev, RMC_EVENT_REMOTE_CONN, RMC_REMOTE_CONN_CONNECTED,
				NULL, 0);
		}
		break;
	case RMC_SG_DISCONNECT_REQ:
		if(d->state == RMC_SG_STATE_WORKING_READY){
			sg_send_ack(d,  &pkt.ack);
			set_state(d, RMC_SG_STATE_WORKING_WAIT_CONN);
			sg_notify(d->dev, RMC_EVENT_REMOTE_CONN, RMC_REMOTE_CONN_DISCONNECTED,
				NULL, 0);
		}
		break;
	case RMC_SG_PTP_CFG_READ:
		if (d->state == RMC_SG_STATE_WORKING_READY ||
		    d->state == RMC_SG_STATE_PAIRING_WAIT_STS) {
			sg_read_resp(d, &pkt.cfg_read);
		} else {
			LOG_WRN("SubGHz: CFG_READ dropped, wrong state (%d)",
				d->state);
		}
		break;
	case RMC_SG_SCAN_RESPONSE:
		sg_handle_scan_response(d, &pkt);
		break;

	case RMC_SG_PTP_BEACON:
	case RMC_SG_PTP_AUTH_CONF:
	case RMC_SG_PTP_CFG_OK:
	case RMC_SG_PTP_CFG_READ_RSP:
		break;

	default:
		LOG_WRN("SubGHz: unhandled PTP 0x%02x in state %d",
			pkt.hdr.ptp, d->state);
	}
}

/* -------------------------------------------------------------------- */
/* State machine dispatch                                                */
/* -------------------------------------------------------------------- */

static void sg_sm_dispatch(struct rmc_subghz_data *d,
			    const struct rmc_sg_sm_msg *msg)
{
	int ret = 0;

	switch (msg->evt) {
	case RMC_SG_EVT_RX_PACKET:
		sg_dispatch_rx(d, msg);
		break;
	case RMC_SG_EVT_BEACON_TICK:
		sg_handle_beacon_send(d);
		break;
	case RMC_SG_EVT_PAIRING_TIMEOUT:
		sg_handle_pairing_timeout(d);
		break;
	case RMC_SG_EVT_PAIRING_EXCHANGE_TIMEOUT:
		sg_handle_pairing_exchange_timeout(d);
		break;
	case RMC_SG_EVT_TX_REQUEST:
		ret = sg_tx_data(d, msg);
		if (ret != 0) {
			LOG_ERR("SubGHz: TX failed: %d", ret);
			sg_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM,
				  NULL, 0);
		}
		break;
	case RMC_SG_EVT_DEINIT:
		break;
	default:
		break;
	}
}

/* -------------------------------------------------------------------- */
/* State machine thread                                                  */
/* -------------------------------------------------------------------- */

static void rmc_sg_sm_thread(void *p1, void *p2, void *p3)
{
	struct rmc_subghz_data *d = p1;
	struct rmc_sg_sm_msg msg;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	sg_start_pairing(d);

	if (d->working_cfg.valid) {
		LOG_INF("SubGHz: pre-loaded config present (ch%u node=0x%02x "
			"remote=0x%02x) - beaconing first for %u ms",
			d->working_cfg.channel_id,
			d->working_cfg.node_id,
			d->working_cfg.remote_id,
			CONFIG_RMC_SUBGHZ_PAIRING_TIMEOUT_MS);
	} else {
		LOG_INF("SubGHz: no pre-loaded config - beaconing for %u ms",
			CONFIG_RMC_SUBGHZ_PAIRING_TIMEOUT_MS);
	}

	while (1) {
		k_msgq_get(&d->sm_msgq, &msg, K_FOREVER);

		if (msg.evt == RMC_SG_EVT_DEINIT) {
			k_work_cancel_delayable(&d->beacon_tick_work);
			k_work_cancel_delayable(&d->pairing_timeout_work);
			k_work_cancel_delayable(&d->pairing_exchange_timeout_work);
			k_work_cancel_delayable(&d->scan_resp_timeout_work);
			set_state(d, RMC_SG_STATE_STOPPED);
			return;
		}

		sg_sm_dispatch(d, &msg);
	}
}

/* -------------------------------------------------------------------- */
/* Backend vtable                                                        */
/* -------------------------------------------------------------------- */

static int rmc_subghz_init(const struct device *dev, void *cfg_ptr)
{
	struct rmc_data *rdata = dev->data;
	const struct rmc_config *rconfig = dev->config;
	struct rmc_subghz_init_cfg *app_cfg = cfg_ptr;
	struct rmc_subghz_data *d;

	if (!rconfig->has_subghz) {
		LOG_ERR("SubGHz: no subghz-radio in Devicetree");
		return -ENOTSUP;
	}
	if (app_cfg == NULL) {
		return -EINVAL;
	}
	if (rmc_subghz_singleton_used) {
		LOG_ERR("SubGHz: only one active instance supported");
		return -ENOTSUP;
	}
	if (!device_is_ready(rconfig->subghz_hw.radio)) {
		LOG_ERR("SubGHz: radio device not ready");
		return -ENOTSUP;
	}

	d = &rmc_subghz_singleton;
	memset(d, 0, sizeof(*d));
	d->dev     = dev;
	d->hw      = &rconfig->subghz_hw;
	d->app_cfg = *app_cfg;

	d->working_cfg.fw_ver	  = d->app_cfg.fw_ver;
	d->working_cfg.hw_ver     = d->app_cfg.hw_ver;

	if (!sg_cfg_is_empty(&d->app_cfg.sbg)) {
		const struct rmc_subghz_cfg *sbg = &d->app_cfg.sbg;

		memcpy(d->working_cfg.network_id, sbg->network_id,
		       sizeof(sbg->network_id));
		d->working_cfg.channel_id = (uint8_t)sbg->channel_id;
		d->working_cfg.node_id    = sbg->shuttle_addr;
		d->working_cfg.remote_id  = sbg->remote_addr;
		d->working_cfg.valid = true;

		LOG_INF("SubGHz: config provided - ch%u node=0x%02x remote=0x%02x",
			d->working_cfg.channel_id, d->working_cfg.node_id,
			d->working_cfg.remote_id);
	}

	read_device_uid(d->device_uid);
	LOG_HEXDUMP_INF(d->device_uid, RMC_SG_DEVICE_UID_LEN,
			"SubGHz device UID:");

	k_work_init_delayable(&d->beacon_tick_work,             beacon_tick_handler);
	k_work_init_delayable(&d->pairing_timeout_work,         pairing_timeout_handler);
	k_work_init_delayable(&d->pairing_exchange_timeout_work,pairing_exchange_timeout_handler);
	k_work_init_delayable(&d->scan_resp_timeout_work,       scan_resp_timeout_handler);
	d->scan_resp_extended = false;

	k_msgq_init(&d->sm_msgq, d->sm_msgq_buf,
		     sizeof(struct rmc_sg_sm_msg),
		     CONFIG_RMC_SUBGHZ_SM_MSGQ_DEPTH);

	rdata->backend_data = d;

	int ret = apply_radio_pairing_config(d);

	if (ret != 0) {
		LOG_ERR("SubGHz: initial radio_configure(pairing ch%u) failed: %d",
			RMC_SG_PAIRING_CHANNEL, ret);
		return ret;
	}

	ret = fsk_set_rx_callback(d->hw->radio, on_fsk_rx, (void *)d);
	if (ret != 0) {
		LOG_ERR("SubGHz: fsk_set_rx_callback() failed: %d", ret);
		return -EIO;
	}
	LOG_INF("SubGHz: RX callback registered - listening on pairing ch%u",
		RMC_SG_PAIRING_CHANNEL);

	rmc_subghz_singleton_used = true;

	/* Start state machine thread. */
	k_thread_create(&d->sm_thread, rmc_sg_sm_stack,
			K_THREAD_STACK_SIZEOF(rmc_sg_sm_stack),
			rmc_sg_sm_thread, d, NULL, NULL,
			CONFIG_RMC_SUBGHZ_SM_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&d->sm_thread, "rmc_sg_sm");

	return 0;
}

static int rmc_subghz_deinit(const struct device *dev)
{
	struct rmc_data *rdata = dev->data;
	struct rmc_subghz_data *d = rdata->backend_data;
	struct rmc_sg_sm_msg msg = { .evt = RMC_SG_EVT_DEINIT };

	if (d == NULL) {
		return -ENODEV;
	}

	(void)k_msgq_put(&d->sm_msgq, &msg, K_FOREVER);
	k_thread_join(&d->sm_thread, K_FOREVER);

	(void)fsk_sleep(d->hw->radio);

	rdata->backend_data = NULL;
	rmc_subghz_singleton_used = false;
	return 0;
}

static int rmc_subghz_transmit(const struct device *dev, uint8_t msg_type,
				const uint8_t *msg, uint8_t msg_len)
{
	ARG_UNUSED(msg_type);
	struct rmc_data *rdata = dev->data;
	struct rmc_subghz_data *d = rdata->backend_data;

	if (d == NULL) {
		return -ENODEV;
	}
	if (!atomic_get(&d->ready)) {
		return -ENOTCONN;
	}
	if (msg == NULL || msg_len == 0U || msg_len > RMC_SG_MAX_DATA_PLD) {
		return -EINVAL;
	}

	struct rmc_sg_sm_msg sm_msg = { .evt = RMC_SG_EVT_TX_REQUEST };

	sm_msg.tx.data_len = msg_len;
	memcpy(sm_msg.tx.data, msg, msg_len);

	if (k_msgq_put(&d->sm_msgq, &sm_msg, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	return 0;
}

static int rmc_subghz_register_callback(const struct device *dev,
					  rmc_callback_t callback)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);
	return 0;
}

static const struct rmc_backend_api rmc_subghz_api = {
	.init              = rmc_subghz_init,
	.deinit            = rmc_subghz_deinit,
	.transmit          = rmc_subghz_transmit,
	.register_callback = rmc_subghz_register_callback,
};

const struct rmc_backend_api *rmc_subghz_backend_get(void)
{
	return &rmc_subghz_api;
}