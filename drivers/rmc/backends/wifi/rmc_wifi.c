/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RMC WiFi (ESP32-C3) backend: connection state machine.
 *
 * Design summary (see the driver architecture discussion for the full
 * rationale):
 *
 *   - A single dedicated thread runs an event loop (k_msgq) that is the
 *     ONLY place connection state is read or written. UART RX frames,
 *     ACK/heartbeat timer expiries, and application TX requests are all
 *     posted into this queue from their originating context (workqueue,
 *     timer callback, application thread) and processed serially here.
 *     This removes the need for locking around connection state.
 *
 *   - "Connect attempts" (struct rmc_wifi_data.connect_attempts) span
 *     the whole bring-up: send config -> wait ACK -> wait WiFi status ->
 *     wait TCP status.
 *
 *   - Two DISTINCT timeout domains are in play here, and mixing them up
 *     was the source of a real bug (CFG being retransmitted immediately
 *     after a valid ACK): waiting for the ACK/NACK reply to a command
 *     uses cfg.ack_retry_timeout_ms (short - it only bounds a UART
 *     round-trip) and, on expiry, retransmits the command up to
 *     cfg.ack_retry_count times before escalating to a hardware reset.
 *     Waiting for the module to actually join the WiFi AP AFTER an ACK
 *     uses cfg.wifi.connect_timeout_s (the "TOT" field already sent to
 *     the module in the config packet, up to 60s) and never retransmits
 *     CFG on its own - if that timer expires, or the module explicitly
 *     reports WiFi connection failure, the driver goes straight to a
 *     hardware reset + full restart. No CFG resend is ever sent "blind"
 *     while this wait is in progress.
 *
 *   - The subsequent wait for the remote TCP connection has NO timeout
 *     at all: it can be established at any time, however long the
 *     module takes, so no timer is armed for that phase. It only ends
 *     on an explicit TCP status packet (success -> READY, or an
 *     explicit failure report -> hardware reset + restart); heartbeat
 *     monitoring, once READY, is what continues to guard against a
 *     genuinely dead link from then on.
 *
 *   - NACK on a TX Data packet is reported to the application via the
 *     callback and the driver takes no further autonomous action - the
 *     link stays READY. A NACKed WiFi Configuration command is reported
 *     the same way, but - since the module actively rejected the
 *     command rather than losing it - is then retransmitted following
 *     the same attempt-counted retry policy as an ACK/NACK timeout.
 *
 *   - Heartbeat loss, and a heartbeat packet that itself reports the
 *     WiFi or TCP link as down, are both treated as "link lost" and
 *     trigger the same hardware reset + full restart as a post-ACK
 *     connection failure.
 */

#include "rmc_wifi.h"
#include "rmc_wifi_protocol.h"
#include "rmc_wifi_uart.h"
#include "../../rmc_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(rmc_wifi, CONFIG_RMC_LOG_LEVEL);

/* -------------------------------------------------------------------- */
/* Internal state machine types                                         */
/* -------------------------------------------------------------------- */

enum rmc_wifi_state {
	RMC_WIFI_STATE_IDLE = 0,	/* zero-init default, before begin_connect_sequence() runs */
	RMC_WIFI_STATE_SEND_CONFIG,	/* config sent, awaiting ACK/NACK */
	RMC_WIFI_STATE_WAIT_WIFI_CONNECTED,
	RMC_WIFI_STATE_WAIT_TCP_CONNECTED,
	RMC_WIFI_STATE_READY,
	RMC_WIFI_STATE_STOPPED,	/* deinit in progress / done */
};

enum rmc_wifi_sm_evt {
	RMC_WIFI_SM_EVT_RX_PACKET,
	RMC_WIFI_SM_EVT_ACK_TIMEOUT,
	RMC_WIFI_SM_EVT_HEARTBEAT_TIMEOUT,
	RMC_WIFI_SM_EVT_TX_REQUEST,
	RMC_WIFI_SM_EVT_DEINIT,
};

struct rmc_wifi_sm_msg {
	enum rmc_wifi_sm_evt evt;
	union {
		struct rmc_wifi_rx_packet rx;	/* RMC_WIFI_SM_EVT_RX_PACKET */
		struct {			/* RMC_WIFI_SM_EVT_TX_REQUEST */
			uint8_t data[RMC_WIFI_DATA_MAX_LEN];
			uint8_t len;
		} tx;
	};
};

/** Backend-private runtime state for one WiFi backend instance. */
struct rmc_wifi_data {
	const struct device *dev;
	const struct rmc_wifi_hw *hw;
	struct rmc_wifi_uart uart;
	struct rmc_wifi_init_cfg cfg;

	enum rmc_wifi_state state;
	atomic_t ready;			/* fast-path flag read by RMC_Transmit() */
	uint8_t connect_attempts;

	/* Single-outstanding ack tracker, reused for config-send and tx-data-send. */
	struct k_work_delayable ack_timeout_work;
	bool ack_pending;
	enum rmc_wifi_mtp pending_msg_type;
	uint8_t ack_retries_left;
	uint8_t pending_tx_buf[RMC_WIFI_DATA_MAX_LEN];
	uint8_t pending_tx_len;

	/* Heartbeat monitoring, active only in RMC_WIFI_STATE_READY. */
	struct k_work_delayable heartbeat_timeout_work;

	/* Event loop */
	struct k_msgq sm_msgq;
	char sm_msgq_buf[CONFIG_RMC_WIFI_MSGQ_DEPTH * sizeof(struct rmc_wifi_sm_msg)];
	struct k_thread sm_thread;
};

/* Single static instance: this backend supports one active RMC WiFi
 * device at a time (one physical ESP32-C3 link), matching the current
 * hardware. Flagged clearly here so a future multi-instance need is an
 * explicit, deliberate change rather than a silent limitation.
 */
static struct rmc_wifi_data rmc_wifi_singleton;
static bool rmc_wifi_singleton_used;

K_THREAD_STACK_DEFINE(rmc_wifi_stack_area, CONFIG_RMC_WIFI_THREAD_STACK_SIZE);

/* -------------------------------------------------------------------- */
/* Small helpers                                                        */
/* -------------------------------------------------------------------- */

/**
 * Pack event_code as rmc_msg[0] followed by any optional payload, then
 * invoke the application callback with the new (dev, evnt_typ, rmc_msg,
 * rmc_msg_len) signature.  All internal call sites keep using the old
 * (event_type, event_code, payload, payload_len) shape - the packing
 * happens entirely here, in one place.
 *
 * Buffer size: 1 (event code) + RMC_WIFI_DATA_MAX_LEN (max payload) + 1 guard.
 */
#define RMC_WIFI_NOTIFY_BUF_SIZE (1U + RMC_WIFI_DATA_MAX_LEN + 1U)

static void rmc_wifi_notify(const struct device *dev, uint8_t event_type, uint8_t event_code,
			     const uint8_t *payload, uint16_t payload_len)
{
	struct rmc_data *rdata = dev->data;
	rmc_callback_t cb = rdata->callback;
	uint8_t buf[RMC_WIFI_NOTIFY_BUF_SIZE];
	uint8_t total_len;

	/* TEMP DEBUG: human-readable link-state trace. Every WiFi and remote
	 * (TCP) up/down transition funnels through here, so logging at this one
	 * point covers all of them - and it runs before the callback gate, so
	 * it prints regardless of whether a callback is registered. */
	switch (event_type) {
	case RMC_EVENT_WIRELESS_CONN:
		LOG_INF("WiFi %s",
			event_code == RMC_WIRELESS_CONN_CONNECTED ? "connected" :
			event_code == RMC_WIRELESS_CONN_FAILED    ? "disconnected (connect failed)" :
			event_code == RMC_WIRELESS_CONN_TIMEOUT   ? "disconnected (connect timeout)" :
								    "disconnected");
		break;
	case RMC_EVENT_REMOTE_CONN:
		LOG_INF("Remote %s",
			event_code == RMC_REMOTE_CONN_CONNECTED ? "connected" :
			event_code == RMC_REMOTE_CONN_TIMEOUT   ? "disconnected (timeout)" :
								  "disconnected");
		break;
	default:
		break;
	}

	if (cb == NULL) {
		return;
	}

	if (payload_len > RMC_WIFI_DATA_MAX_LEN) {
		payload_len = RMC_WIFI_DATA_MAX_LEN;
	}

	buf[0] = event_code;
	if (payload != NULL && payload_len > 0) {
		memcpy(&buf[1], payload, payload_len);
	}
	total_len = (uint8_t)(1U + payload_len);

	cb(dev, event_type, buf, total_len);
}

static void set_state(struct rmc_wifi_data *d, enum rmc_wifi_state new_state)
{
	d->state = new_state;
	atomic_set(&d->ready, new_state == RMC_WIFI_STATE_READY ? 1 : 0);
}

static void arm_ack_timer(struct rmc_wifi_data *d)
{
	k_work_schedule(&d->ack_timeout_work, K_MSEC(d->cfg.ack_retry_timeout_ms));
}

/**
 * Arms the same underlying timer as arm_ack_timer(), but for waiting on a
 * connection indication (WiFi/TCP status packet) AFTER a CFG command has
 * already been ACKed. Deliberately a separate, much longer timeout domain:
 * cfg.wifi.connect_timeout_s is the "TOT" value already communicated to the
 * module in the config packet as how long IT will spend trying to join the
 * AP, so the i.MXRT side must wait at least that long before concluding the
 * module isn't going to respond - reusing the short ACK-wait timeout here
 * was the bug that caused CFG to be resent almost immediately after a valid
 * ACK. Only one of the two waits is ever active at a time (mutually
 * exclusive states), so sharing the one k_work_delayable is safe.
 */
static void arm_connect_timer(struct rmc_wifi_data *d)
{
	uint32_t timeout_ms = (uint32_t)d->cfg.wifi.connect_timeout_s * 1000U;

	if (timeout_ms == 0) {
		/* Defensive fallback only - RMC_Init() callers are expected to
		 * supply a sensible connect_timeout_s (the packet format caps
		 * it at 60s); this just avoids an effectively-infinite wait if
		 * one didn't.
		 */
		LOG_WRN("connect_timeout_s is 0, falling back to ack_retry_timeout_ms");
		timeout_ms = d->cfg.ack_retry_timeout_ms;
	}

	k_work_schedule(&d->ack_timeout_work, K_MSEC(timeout_ms));
}

static void disarm_ack_timer(struct rmc_wifi_data *d)
{
	k_work_cancel_delayable(&d->ack_timeout_work);
}

static void arm_heartbeat_timer(struct rmc_wifi_data *d)
{
	k_work_schedule(&d->heartbeat_timeout_work, K_MSEC(d->cfg.heartbeat_timeout_ms));
}

static void disarm_heartbeat_timer(struct rmc_wifi_data *d)
{
	k_work_cancel_delayable(&d->heartbeat_timeout_work);
}

/** Blocking - only ever called from the dedicated state-machine thread. */
static void esp32_reset_pulse(struct rmc_wifi_data *d)
{
	gpio_pin_set_dt(&d->hw->chip_en, 0);
	k_sleep(K_MSEC(CONFIG_RMC_WIFI_CHIP_EN_RESET_LOW_MS));
	gpio_pin_set_dt(&d->hw->chip_en, 1);
	k_sleep(K_MSEC(CONFIG_RMC_WIFI_CHIP_EN_STARTUP_DELAY_MS));
}

static int send_config(struct rmc_wifi_data *d)
{
	char buf[300];
	int ret = rmc_wifi_protocol_encode_wifi_cfg(&d->cfg.wifi, buf, sizeof(buf));

	if (ret != 0) {
		LOG_ERR("Failed to encode WiFi config packet: %d", ret);
		return ret;
	}
	/* TEMP DEBUG: the exact config packet sent to the ESP32 (SSID / IP / etc.).
	 * Contains the WiFi password in plaintext - debug builds only. */
	LOG_INF("wifi TX config: %s", buf);
	return rmc_wifi_uart_send(&d->uart, buf, strlen(buf), K_MSEC(d->cfg.ack_retry_timeout_ms));
}

static int send_ack(struct rmc_wifi_data *d, bool nack)
{
	char buf[32];
	int ret = rmc_wifi_protocol_encode_ack(nack, buf, sizeof(buf));

	if (ret != 0) {
		LOG_ERR("Failed to encode ACK/NACK packet: %d", ret);
		return ret;
	}
	return rmc_wifi_uart_send(&d->uart, buf, strlen(buf), K_MSEC(d->cfg.ack_retry_timeout_ms));
}

static int send_tx_data(struct rmc_wifi_data *d, const uint8_t *data, uint8_t len)
{
	char buf[RMC_WIFI_DATA_MAX_LEN + 32];
	int ret = rmc_wifi_protocol_encode_tx_data(data, len, buf, sizeof(buf));

	if (ret != 0) {
		LOG_ERR("Failed to encode TX Data packet: %d", ret);
		return ret;
	}
	return rmc_wifi_uart_send(&d->uart, buf, strlen(buf), K_MSEC(d->cfg.ack_retry_timeout_ms));
}

/* -------------------------------------------------------------------- */
/* Connection sequence                                                  */
/* -------------------------------------------------------------------- */

static void start_connect_attempt(struct rmc_wifi_data *d)
{
	d->connect_attempts++;

	if (send_config(d) != 0) {
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM, NULL, 0);
		/* Couldn't even get the command onto the wire - treat exactly like
		 * an unanswered command so the normal retry/recovery path applies.
		 */
	}

	d->ack_pending = true;
	d->pending_msg_type = RMC_WIFI_MTP_WIFI_CFG;
	set_state(d, RMC_WIFI_STATE_SEND_CONFIG);
	arm_ack_timer(d);
}

static void recover_and_restart(struct rmc_wifi_data *d)
{
	rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_UNKNOWN, NULL, 0);

	disarm_ack_timer(d);
	disarm_heartbeat_timer(d);
	d->ack_pending = false;
	d->connect_attempts = 0;

	esp32_reset_pulse(d);
	start_connect_attempt(d);
}

static void handle_connect_attempt_failure(struct rmc_wifi_data *d)
{
	if (d->connect_attempts >= d->cfg.ack_retry_count) {
		LOG_WRN("WiFi connect attempt retries exhausted (%u/%u), resetting ESP32-C3",
			d->connect_attempts, d->cfg.ack_retry_count);
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_RETRY_LIMIT, NULL, 0);
		recover_and_restart(d);
		return;
	}

	LOG_INF("Retrying WiFi connect sequence (attempt %u/%u)",
		d->connect_attempts + 1, d->cfg.ack_retry_count);
	start_connect_attempt(d);
}

static void begin_connect_sequence(struct rmc_wifi_data *d)
{
	d->connect_attempts = 0;
	esp32_reset_pulse(d);
	start_connect_attempt(d);
}

/* -------------------------------------------------------------------- */
/* RX packet handlers                                                   */
/* -------------------------------------------------------------------- */

static void handle_ack_nack(struct rmc_wifi_data *d, bool nack)
{
	if (!d->ack_pending) {
		LOG_WRN("Unexpected ACK/NACK with no outstanding command, ignoring");
		return;
	}

	disarm_ack_timer(d);
	d->ack_pending = false;

	if (nack) {
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_INVALID_PACKET, NULL, 0);

		if (d->pending_msg_type == RMC_WIFI_MTP_WIFI_CFG) {
			/* NACK during the CFG phase: go straight to the reset logic
			 * (CHIP_EN power-recycle + full restart), not a plain CFG
			 * resend.
			 */
			LOG_WRN("WiFi Configuration rejected (NACK); recovering");
			recover_and_restart(d);
		}
		/* NACK on TX Data: notify only, no reset - link stays READY and
		 * normal operation continues.
		 */
		return;
	}

	/* ACK */
	if (d->pending_msg_type == RMC_WIFI_MTP_WIFI_CFG) {
		/* Do NOT retransmit CFG from here. The module now needs time to
		 * actually join the AP - wait for its own TOT-based timeout
		 * (arm_connect_timer()), not the short ACK-wait timeout.
		 */
		set_state(d, RMC_WIFI_STATE_WAIT_WIFI_CONNECTED);
		arm_connect_timer(d);
	}
	/* ACK for TX Data: transmission confirmed, nothing further to do. */
}

static void handle_wifi_status(struct rmc_wifi_data *d, bool connected)
{
	if (d->state != RMC_WIFI_STATE_WAIT_WIFI_CONNECTED) {
		LOG_WRN("Unexpected WiFi status packet in state %d, ignoring", d->state);
		return;
	}

	disarm_ack_timer(d);

	if (connected) {
		rmc_wifi_notify(d->dev, RMC_EVENT_WIRELESS_CONN, RMC_WIRELESS_CONN_CONNECTED, NULL, 0);
		/* No timer armed for the TCP wait itself: TCP connection has no
		 * timeout - it can be established at any time, however long the
		 * module takes. We just wait for an explicit TCP status packet
		 * (see handle_tcp_status()). Heartbeat monitoring, however,
		 * starts right now (not once TCP connects) - see
		 * handle_heartbeat() for how it's interpreted during this phase.
		 */
		set_state(d, RMC_WIFI_STATE_WAIT_TCP_CONNECTED);
		arm_heartbeat_timer(d);
	} else {
		/* Module explicitly reported failure after already ACKing the
		 * config - recover via power-recycle rather than just resending
		 * CFG again blind (see the file header comment for why).
		 */
		rmc_wifi_notify(d->dev, RMC_EVENT_WIRELESS_CONN, RMC_WIRELESS_CONN_FAILED, NULL, 0);
		recover_and_restart(d);
	}
}

static void handle_tcp_status(struct rmc_wifi_data *d, bool connected)
{
	if (d->state != RMC_WIFI_STATE_WAIT_TCP_CONNECTED &&
	    d->state != RMC_WIFI_STATE_READY) {
		LOG_WRN("Unexpected TCP status packet in state %d, ignoring", d->state);
		return;
	}

	/* No timer to disarm here by design (TCP wait is unbounded), but
	 * cancel defensively in case anything ever schedules one for this
	 * phase in the future.
	 */
	disarm_ack_timer(d);

	if (d->state == RMC_WIFI_STATE_WAIT_TCP_CONNECTED) {
		if (connected) {
			rmc_wifi_notify(d->dev, RMC_EVENT_REMOTE_CONN,
					 RMC_REMOTE_CONN_CONNECTED, NULL, 0);
			d->connect_attempts = 0;
			set_state(d, RMC_WIFI_STATE_READY);
			/* Heartbeat monitoring has been running continuously since
			 * WiFi connected (see handle_wifi_status()) - no re-arm
			 * needed here.
			 */
		} else {
			/* Module explicitly reported TCP failure before we ever
			 * reached READY - recover.
			 */
			rmc_wifi_notify(d->dev, RMC_EVENT_REMOTE_CONN,
					 RMC_REMOTE_CONN_DISCONNECTED, NULL, 0);
			recover_and_restart(d);
		}
	} else {
		/* d->state == RMC_WIFI_STATE_READY
		 * An unsolicited status packet from the ESP32 reporting the
		 * live TCP connection has changed. A "connected" report while
		 * already READY is a harmless no-op (notify only). A
		 * "disconnected" report means the remote TCP server dropped us
		 * while we were operational - notify the application and
		 * recover immediately, exactly as we would for a heartbeat
		 * reporting tcp_up == false.
		 */
		if (!connected) {
			LOG_WRN("TCP disconnected while READY, recovering");
			rmc_wifi_notify(d->dev, RMC_EVENT_REMOTE_CONN,
					 RMC_REMOTE_CONN_DISCONNECTED, NULL, 0);
			recover_and_restart(d);
		} else {
			/* Already READY and TCP reports connected - nothing to do. */
			rmc_wifi_notify(d->dev, RMC_EVENT_REMOTE_CONN,
					 RMC_REMOTE_CONN_CONNECTED, NULL, 0);
		}
	}
}

static void handle_heartbeat(struct rmc_wifi_data *d, bool wifi_up, bool tcp_up)
{
	if (d->state != RMC_WIFI_STATE_WAIT_TCP_CONNECTED && d->state != RMC_WIFI_STATE_READY) {
		/* Heartbeats seen before WiFi is even connected are just noise. */
		return;
	}

	disarm_heartbeat_timer(d);
	arm_heartbeat_timer(d);
	rmc_wifi_notify(d->dev, RMC_EVENT_HEARTBEAT, RMC_HEARTBEAT_RECEIVED, NULL, 0);

	if (!wifi_up) {
		/* WiFi link itself is reported down - always a genuine failure,
		 * regardless of TCP phase.
		 */
		LOG_WRN("Heartbeat reports WiFi link down, recovering");
		rmc_wifi_notify(d->dev, RMC_EVENT_WIRELESS_CONN, RMC_WIRELESS_CONN_DISCONNECTED, NULL, 0);
		recover_and_restart(d);
		return;
	}

	if (d->state == RMC_WIFI_STATE_READY && !tcp_up) {
		/* TCP was established and has now dropped - that's a real
		 * regression once READY. (While still in WAIT_TCP_CONNECTED,
		 * tcp_up == false is the expected/normal reading - TCP simply
		 * hasn't connected yet - and must NOT trigger recovery.)
		 */
		LOG_WRN("Heartbeat reports TCP link down while READY, recovering");
		rmc_wifi_notify(d->dev, RMC_EVENT_REMOTE_CONN, RMC_REMOTE_CONN_DISCONNECTED, NULL, 0);
		recover_and_restart(d);
	}
}

static void handle_rx_data(struct rmc_wifi_data *d, const uint8_t *data, uint8_t len)
{
	/* Per the sequence diagram, inbound RX Data is ACK'd by this device,
	 * regardless of the higher-level connection phase.
	 */
	if (send_ack(d, false) != 0) {
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM, NULL, 0);
	}

	rmc_wifi_notify(d->dev, RMC_EVENT_RX_DATA, RMC_RX_DATA_RECEIVED, data, len);
}

static void handle_ack_timeout(struct rmc_wifi_data *d)
{
	switch (d->state) {
	case RMC_WIFI_STATE_SEND_CONFIG:
		/* No ACK/NACK arrived in time for the CFG command itself -
		 * retransmit per the attempt-counted retry policy.
		 */
		d->ack_pending = false;
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM, NULL, 0);
		handle_connect_attempt_failure(d);
		break;

	case RMC_WIFI_STATE_WAIT_WIFI_CONNECTED:
		/* CFG was already ACKed; the module never sent a WiFi status
		 * within its own TOT window. Do NOT resend CFG here - go
		 * straight to a power-recycle and restart, per spec.
		 */
		LOG_WRN("WiFi connection indication not received within %u s, recovering",
			d->cfg.wifi.connect_timeout_s);
		rmc_wifi_notify(d->dev, RMC_EVENT_WIRELESS_CONN, RMC_WIRELESS_CONN_TIMEOUT, NULL, 0);
		recover_and_restart(d);
		break;

	/* RMC_WIFI_STATE_WAIT_TCP_CONNECTED intentionally has no case here:
	 * that phase is unbounded (no connect timer is ever armed for it -
	 * TCP can connect at any time), so this event can never legitimately
	 * fire while in it. Falls through to default if it ever does (e.g. a
	 * stale timer from a state already left).
	 */

	case RMC_WIFI_STATE_READY:
		if (!d->ack_pending || d->pending_msg_type != RMC_WIFI_MTP_TX_DATA) {
			break; /* stale timer fire, ignore */
		}
		if (d->ack_retries_left > 0) {
			d->ack_retries_left--;
			LOG_INF("TX Data ACK timeout, retrying (%u left)", d->ack_retries_left);
			send_tx_data(d, d->pending_tx_buf, d->pending_tx_len);
			arm_ack_timer(d);
		} else {
			d->ack_pending = false;
			rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_RETRY_LIMIT, NULL, 0);
		}
		break;

	default:
		break; /* stale timer from a state already left (e.g. after recovery) */
	}
}

/* -------------------------------------------------------------------- */
/* Event loop                                                            */
/* -------------------------------------------------------------------- */

static void rmc_wifi_sm_dispatch(struct rmc_wifi_data *d, struct rmc_wifi_sm_msg *msg)
{
	switch (msg->evt) {
	case RMC_WIFI_SM_EVT_RX_PACKET:
		switch (msg->rx.mtp) {
		case RMC_WIFI_MTP_ACK_NACK:
			handle_ack_nack(d, msg->rx.ack.nack);
			break;
		case RMC_WIFI_MTP_WIFI_STATUS:
			handle_wifi_status(d, msg->rx.wifi_status.connected);
			break;
		case RMC_WIFI_MTP_TCP_STATUS:
			handle_tcp_status(d, msg->rx.tcp_status.connected);
			break;
		case RMC_WIFI_MTP_HEARTBEAT:
			handle_heartbeat(d, msg->rx.heartbeat.wifi_connected,
					  msg->rx.heartbeat.tcp_connected);
			break;
		case RMC_WIFI_MTP_RX_DATA:
			handle_rx_data(d, msg->rx.rx_data.data, msg->rx.rx_data.len);
			break;
		default:
			break;
		}
		break;

	case RMC_WIFI_SM_EVT_ACK_TIMEOUT:
		handle_ack_timeout(d);
		break;

	case RMC_WIFI_SM_EVT_HEARTBEAT_TIMEOUT:
		if (d->state == RMC_WIFI_STATE_READY || d->state == RMC_WIFI_STATE_WAIT_TCP_CONNECTED) {
			/* Total heartbeat silence means the module itself has gone
			 * quiet - a different condition from "TCP hasn't connected
			 * yet" (which has no timeout at all, by design). This still
			 * warrants recovery regardless of TCP phase.
			 */
			rmc_wifi_notify(d->dev, RMC_EVENT_HEARTBEAT, RMC_HEARTBEAT_TIMEOUT,
					 NULL, 0);
			recover_and_restart(d);
		}
		break;

	case RMC_WIFI_SM_EVT_TX_REQUEST:
		if (d->state != RMC_WIFI_STATE_READY || d->ack_pending) {
			/* State changed between RMC_Transmit()'s fast-path check and
			 * this message being processed (e.g. heartbeat loss, or a
			 * prior TX still awaiting ack). RMC_Transmit() already
			 * returned success, so surface the failure via callback.
			 */
			rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_UNKNOWN, NULL, 0);
			break;
		}

		memcpy(d->pending_tx_buf, msg->tx.data, msg->tx.len);
		d->pending_tx_len = msg->tx.len;

		if (send_tx_data(d, d->pending_tx_buf, d->pending_tx_len) != 0) {
			rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_COMM, NULL, 0);
			break;
		}

		d->ack_pending = true;
		d->pending_msg_type = RMC_WIFI_MTP_TX_DATA;
		d->ack_retries_left = d->cfg.ack_retry_count;
		arm_ack_timer(d);
		break;

	case RMC_WIFI_SM_EVT_DEINIT:
		break; /* handled in the thread loop before reaching here */
	}
}

static void ack_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct rmc_wifi_data *d = CONTAINER_OF(dw, struct rmc_wifi_data, ack_timeout_work);
	struct rmc_wifi_sm_msg msg = { .evt = RMC_WIFI_SM_EVT_ACK_TIMEOUT };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

static void heartbeat_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct rmc_wifi_data *d = CONTAINER_OF(dw, struct rmc_wifi_data, heartbeat_timeout_work);
	struct rmc_wifi_sm_msg msg = { .evt = RMC_WIFI_SM_EVT_HEARTBEAT_TIMEOUT };

	(void)k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT);
}

static void rmc_wifi_on_frame(uint8_t *frame, size_t len, void *user_data)
{
	struct rmc_wifi_data *d = user_data;
	struct rmc_wifi_sm_msg msg = { .evt = RMC_WIFI_SM_EVT_RX_PACKET };
	int ret;

	/* TEMP DEBUG: the exact JSON frame from the ESP32, before decode mutates
	 * it in place (ACK / status / heartbeat / RX data). */
	LOG_INF("wifi RX raw frame: %.*s", (int)len, (const char *)frame);

	ret = rmc_wifi_protocol_decode(frame, len, &msg.rx);

	if (ret != 0) {
		LOG_WRN("Failed to decode RX frame: %d", ret);
		rmc_wifi_notify(d->dev, RMC_EVENT_ERROR, RMC_ERR_INVALID_PACKET, NULL, 0);
		return;
	}

	if (k_msgq_put(&d->sm_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("State-machine queue full, dropping RX packet (MTP %d)", msg.rx.mtp);
	}
}

static void rmc_wifi_thread_fn(void *p1, void *p2, void *p3)
{
	struct rmc_wifi_data *d = p1;
	struct rmc_wifi_sm_msg msg;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	begin_connect_sequence(d);

	while (1) {
		k_msgq_get(&d->sm_msgq, &msg, K_FOREVER);

		if (msg.evt == RMC_WIFI_SM_EVT_DEINIT) {
			disarm_ack_timer(d);
			disarm_heartbeat_timer(d);
			set_state(d, RMC_WIFI_STATE_STOPPED);
			return;
		}

		rmc_wifi_sm_dispatch(d, &msg);
	}
}

/* -------------------------------------------------------------------- */
/* Backend API                                                           */
/* -------------------------------------------------------------------- */

static int rmc_wifi_init(const struct device *dev, void *cfg)
{
	struct rmc_data *rdata = dev->data;
	const struct rmc_config *rconfig = dev->config;
	struct rmc_wifi_init_cfg *app_cfg = cfg;
	struct rmc_wifi_data *d;
	int ret;

	if (!rconfig->has_wifi) {
		LOG_ERR("Devicetree node has no WiFi resources (wifi-uart / wifi-enable-gpios)");
		return -ENODEV;
	}
	if (app_cfg == NULL) {
		return -EINVAL;
	}
	if (rmc_wifi_singleton_used) {
		LOG_ERR("RMC WiFi backend supports only one active instance");
		return -ENOMEM;
	}

	if (!gpio_is_ready_dt(&rconfig->wifi_hw.chip_en)) {
		LOG_ERR("CHIP_EN GPIO not ready");
		return -ENODEV;
	}

	/* Requirement: CHIP_EN configured as output, driven low by default. */
	ret = gpio_pin_configure_dt(&rconfig->wifi_hw.chip_en, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure CHIP_EN GPIO: %d", ret);
		return ret;
	}

	d = &rmc_wifi_singleton;
	memset(d, 0, sizeof(*d));
	d->dev = dev;
	d->hw = &rconfig->wifi_hw;
	d->cfg = *app_cfg;

	if (d->cfg.ack_retry_timeout_ms == 0) {
		d->cfg.ack_retry_timeout_ms = CONFIG_RMC_WIFI_DEFAULT_ACK_RETRY_TIMEOUT_MS;
	}
	if (d->cfg.ack_retry_count == 0) {
		d->cfg.ack_retry_count = CONFIG_RMC_WIFI_DEFAULT_ACK_RETRY_COUNT;
	}
	if (d->cfg.heartbeat_timeout_ms == 0) {
		d->cfg.heartbeat_timeout_ms = CONFIG_RMC_WIFI_DEFAULT_HEARTBEAT_TIMEOUT_MS;
	}

	ret = rmc_wifi_uart_init(&d->uart, rconfig->wifi_hw.uart, rmc_wifi_on_frame, d);
	if (ret != 0) {
		LOG_ERR("Failed to init UART layer: %d", ret);
		return ret;
	}

	k_work_init_delayable(&d->ack_timeout_work, ack_timeout_handler);
	k_work_init_delayable(&d->heartbeat_timeout_work, heartbeat_timeout_handler);
	k_msgq_init(&d->sm_msgq, d->sm_msgq_buf, sizeof(struct rmc_wifi_sm_msg),
		    CONFIG_RMC_WIFI_MSGQ_DEPTH);

	rdata->backend_data = d;
	rmc_wifi_singleton_used = true;

	k_thread_create(&d->sm_thread, rmc_wifi_stack_area,
			 K_THREAD_STACK_SIZEOF(rmc_wifi_stack_area),
			 rmc_wifi_thread_fn, d, NULL, NULL,
			 CONFIG_RMC_WIFI_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&d->sm_thread, "rmc_wifi");

	return 0;
}

static int rmc_wifi_deinit(const struct device *dev)
{
	struct rmc_data *rdata = dev->data;
	struct rmc_wifi_data *d = rdata->backend_data;
	struct rmc_wifi_sm_msg msg = { .evt = RMC_WIFI_SM_EVT_DEINIT };

	if (d == NULL) {
		return -EALREADY;
	}

	(void)k_msgq_put(&d->sm_msgq, &msg, K_FOREVER);
	k_thread_join(&d->sm_thread, K_FOREVER);

	rmc_wifi_uart_deinit(&d->uart);
	gpio_pin_set_dt(&d->hw->chip_en, 0);

	rdata->backend_data = NULL;
	rmc_wifi_singleton_used = false;

	return 0;
}

static int rmc_wifi_transmit(const struct device *dev, uint8_t msg_type,
			      const uint8_t *msg, uint8_t msg_len)
{
	struct rmc_data *rdata = dev->data;
	struct rmc_wifi_data *d = rdata->backend_data;
	struct rmc_wifi_sm_msg sm_msg = { .evt = RMC_WIFI_SM_EVT_TX_REQUEST };

	/* The WiFi backend only ever originates MTP 06 (TX Data) for
	 * application payloads, so msg_type is accepted but not otherwise
	 * interpreted - it's a passthrough hook for future backends/wire
	 * formats where the application might need to select a sub-type.
	 */
	ARG_UNUSED(msg_type);

	if (d == NULL) {
		return -ENODEV;
	}
	if (msg == NULL || msg_len == 0 || msg_len > RMC_WIFI_DATA_MAX_LEN) {
		return -EINVAL;
	}
	if (!atomic_get(&d->ready)) {
		return -ENOTCONN;
	}

	memcpy(sm_msg.tx.data, msg, msg_len);
	sm_msg.tx.len = msg_len;

	if (k_msgq_put(&d->sm_msgq, &sm_msg, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	return 0;
}

static int rmc_wifi_register_callback(const struct device *dev, rmc_callback_t callback)
{
	/* rmc.c already stores the callback on struct rmc_data (dev->data),
	 * which this backend reads live via rmc_wifi_notify() - no separate
	 * copy needed here.
	 */
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);
	return 0;
}

static const struct rmc_backend_api rmc_wifi_api = {
	.init = rmc_wifi_init,
	.deinit = rmc_wifi_deinit,
	.transmit = rmc_wifi_transmit,
	.register_callback = rmc_wifi_register_callback,
};

const struct rmc_backend_api *rmc_wifi_backend_get(void)
{
	return &rmc_wifi_api;
}
