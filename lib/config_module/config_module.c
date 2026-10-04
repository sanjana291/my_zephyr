/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Configuration Module - main file.
 *
 * Owns:
 *   - Module lifecycle (CFG_Init / CFG_Deinit)
 *   - tcp_server callback registration and event handling
 *   - Connection timer (CONFIG_CFG_MODULE_CONN_TIMEOUT_MS)
 *   - CFG_RegisterCallback / CFG_Resp public API
 *
 * Delegates JSON parsing to config_module_json.c and range validation to
 * config_module_validate.c. The application never calls tcp_server
 * functions directly.
 *
 * This module does NOT read or write any RTC / real-time-clock hardware.
 * Section O's "stm" field is passed through as opaque byte data; any RTC
 * interpretation/handling belongs entirely to the application layer.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tcp_server/tcp_server.h>
#include <config_module/config_module.h>
#include "config_module_internal.h"

LOG_MODULE_REGISTER(config_module, CONFIG_CONFIG_MODULE_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * Global module state
 * ------------------------------------------------------------------------- */

/*
 * .lock is statically initialized because CFG_Init() below takes this
 * lock as its very first action. A zeroed k_mutex is not a valid mutex
 * in Zephyr, so locking it before any k_mutex_init() call is undefined
 * behaviour - and calling k_mutex_init() *after* that first lock (as the
 * old code did) silently resets lock_count/owner, meaning the "lock"
 * held during CFG_Init() setup was never real: the concurrently-running
 * tcp_server thread could race in and touch cfg_ctx.state mid-setup, and
 * the final k_mutex_unlock() would be releasing a lock this thread never
 * legitimately held.
 */
struct cfg_ctx cfg_ctx = {
	.state       = CFG_STATE_UNINIT,
	.app_cb      = NULL,
	.pending_sid = CFG_SID_UNKNOWN,
	.lock        = Z_MUTEX_INITIALIZER(cfg_ctx.lock),
};

/* ---------------------------------------------------------------------------
 * Connection timer
 * ------------------------------------------------------------------------- */

#if CONFIG_CFG_MODULE_CONN_TIMEOUT_MS > 0

static void conn_timer_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("Connection timer expired - disconnecting client");

	/*
	 * Calling tcp_server_deinit/reinit here would be heavy-weight.
	 * The cleanest path is to shut down the current client socket via
	 * tcp_server_deinit() and immediately restart - the tcp_server thread
	 * will then go back to accept(). A simpler production approach is to
	 * add a tcp_server_disconnect_client() helper to the tcp_server library;
	 * for now we restart the server entirely which achieves the same effect.
	 */
	tcp_server_deinit();

	/* Brief yield so the tcp_server thread can clean up before reinit. */
	k_sleep(K_MSEC(50));
	tcp_server_init(NULL); /* re-use the same config applied at CFG_Init */

	cfg_notify_app(CFG_EVT_CONN_TIMEOUT, NULL);
}

/** Arm the connection timer after a successful handshake. */
static void conn_timer_start(void)
{
	k_work_schedule(&cfg_ctx.conn_timer,
			K_MSEC(CONFIG_CFG_MODULE_CONN_TIMEOUT_MS));
	LOG_DBG("Connection timer armed (%d ms)", CONFIG_CFG_MODULE_CONN_TIMEOUT_MS);
}

/** Cancel the connection timer (on disconnect or timer-driven disconnect). */
static void conn_timer_stop(void)
{
	k_work_cancel_delayable(&cfg_ctx.conn_timer);
	LOG_DBG("Connection timer cancelled");
}

/** Reset the connection timer: cancel + re-arm. Called on each received packet. */
void cfg_conn_timer_reset(void)
{
	if (cfg_ctx.state == CFG_STATE_CONNECTED) {
		k_work_reschedule(&cfg_ctx.conn_timer,
				  K_MSEC(CONFIG_CFG_MODULE_CONN_TIMEOUT_MS));
	}
}

#else /* CONFIG_CFG_MODULE_CONN_TIMEOUT_MS == 0 -> disabled */
static void conn_timer_start(void) {}
static void conn_timer_stop(void)  {}
void cfg_conn_timer_reset(void)    {}
#endif

/* ---------------------------------------------------------------------------
 * TCP server callbacks (registered internally; never exposed to application)
 * ------------------------------------------------------------------------- */

/**
 * Receive callback: called by tcp_server for every validated JSON frame.
 * Resets the connection timer so the session stays alive while packets flow.
 */
static void on_tcp_receive(const char *json, size_t json_len, void *user_data)
{
	ARG_UNUSED(user_data);

	cfg_conn_timer_reset();
	cfg_json_dispatch(json, json_len);
}

/**
 * Event callback: handles connection / handshake / error lifecycle events
 * from the tcp_server. All TCP state management is internal; the application
 * only sees the distilled CFG_EVT_* events.
 */
static void on_tcp_event(enum tcp_server_event event,
			  enum tcp_server_error error,
			  void *user_data)
{
	ARG_UNUSED(user_data);
	ARG_UNUSED(error);

	switch (event) {
	case TCP_SERVER_EVT_HANDSHAKE_OK:
		k_mutex_lock(&cfg_ctx.lock, K_FOREVER);
		cfg_ctx.state = CFG_STATE_CONNECTED;
		cfg_ctx.pending_sid = CFG_SID_UNKNOWN;
		k_mutex_unlock(&cfg_ctx.lock);

		conn_timer_start();
		LOG_INF("Client authenticated - session open");
		cfg_notify_app(CFG_EVT_CONNECTED, NULL);
		break;

	case TCP_SERVER_EVT_CLIENT_DISCONNECTED:
		k_mutex_lock(&cfg_ctx.lock, K_FOREVER);
		cfg_ctx.state = CFG_STATE_IDLE;
		cfg_ctx.pending_sid = CFG_SID_UNKNOWN;
		k_mutex_unlock(&cfg_ctx.lock);

		conn_timer_stop();
		LOG_INF("Client disconnected");
		cfg_notify_app(CFG_EVT_DISCONNECTED, NULL);
		break;

	case TCP_SERVER_EVT_HANDSHAKE_FAILED:
	case TCP_SERVER_EVT_HANDSHAKE_TIMEOUT:
		LOG_WRN("Handshake failed/timeout - client dropped by tcp_server");
		/* tcp_server already closed the socket; no extra action needed */
		break;

	case TCP_SERVER_EVT_CLIENT_REJECTED:
		LOG_WRN("Connection rejected: source IP not in allow-list");
		break;

	case TCP_SERVER_EVT_ERROR:
		LOG_ERR("tcp_server error (code %d)", error);
		break;

	case TCP_SERVER_EVT_PACKET_INVALID:
		LOG_WRN("tcp_server: malformed JSON stream, client will be dropped");
		break;

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

void cfg_send_ack_nack(bool ack, uint8_t rtp)
{
	char buf[34];
	int n = cfg_json_encode_ack_nack(buf, sizeof(buf), ack, rtp);

	if (n <= 0) {
		LOG_ERR("ACK/NACK encode failed");
		return;
	}
	int ret = tcp_server_transmit((const uint8_t *)buf, (size_t)n);

	if (ret < 0) {
		LOG_ERR("ACK/NACK transmit failed: %d", ret);
	}
}

void cfg_send_read_response(char sec_id, const config_params_t *params)
{
	char buf[CONFIG_CFG_MODULE_TX_BUF_SIZE];
	int n = cfg_json_encode_read_response(buf, sizeof(buf), sec_id, params);

	if (n <= 0) {
		LOG_ERR("Read response encode failed for SID '%c' (%d)", sec_id, n);
		cfg_send_ack_nack(false, 3);
		return;
	}
	int ret = tcp_server_transmit((const uint8_t *)buf, (size_t)n);

	if (ret < 0) {
		LOG_ERR("Read response transmit failed: %d", ret);
	}
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

config_module_return_t CFG_Init(void)
{
	k_mutex_lock(&cfg_ctx.lock, K_FOREVER);

	if (cfg_ctx.state != CFG_STATE_UNINIT) {
		k_mutex_unlock(&cfg_ctx.lock);
		return CFG_ERR_ALREADY_INIT;
	}

#if CONFIG_CFG_MODULE_CONN_TIMEOUT_MS > 0
	k_work_init_delayable(&cfg_ctx.conn_timer, conn_timer_handler);
#endif

	/* Register our own callbacks with the tcp_server before starting it. */
	tcp_server_register_receive_callback(on_tcp_receive, NULL);
	tcp_server_register_event_callback(on_tcp_event, NULL);

	/* Start the TCP server.  NULL -> use Kconfig defaults (port 5000,
	 * default handshake secret, allowed IP, keepalive parameters). */
	int ret = tcp_server_init(NULL);

	if (ret != 0) {
		LOG_ERR("tcp_server_init() failed: %d", ret);
		cfg_ctx.state = CFG_STATE_UNINIT;
		k_mutex_unlock(&cfg_ctx.lock);
		return CFG_ERR_TCP_FAIL;
	}

	cfg_ctx.state = CFG_STATE_IDLE;
	k_mutex_unlock(&cfg_ctx.lock);

	LOG_INF("Configuration Module initialised");
	return CFG_OK;
}

config_module_return_t CFG_Deinit(void)
{
	k_mutex_lock(&cfg_ctx.lock, K_FOREVER);

	if (cfg_ctx.state == CFG_STATE_UNINIT) {
		k_mutex_unlock(&cfg_ctx.lock);
		return CFG_ERR_NOT_INIT;
	}

	conn_timer_stop();
	tcp_server_deinit();

	cfg_ctx.state = CFG_STATE_UNINIT;
	cfg_ctx.pending_sid = CFG_SID_UNKNOWN;
	k_mutex_unlock(&cfg_ctx.lock);

	LOG_INF("Configuration Module stopped");
	return CFG_OK;
}

config_module_return_t CFG_RegisterCallback(cfg_request_cb_t cb)
{
	if (cb == NULL) {
		return CFG_ERR_INVALID_ARG;
	}

	k_mutex_lock(&cfg_ctx.lock, K_FOREVER);
	cfg_ctx.app_cb = cb;
	k_mutex_unlock(&cfg_ctx.lock);

	return CFG_OK;
}

config_module_return_t CFG_Resp(config_resp_type_t resp_type,
				char sec_id,
				const config_params_t *data_struct)
{
	k_mutex_lock(&cfg_ctx.lock, K_FOREVER);

	if (cfg_ctx.state == CFG_STATE_UNINIT) {
		k_mutex_unlock(&cfg_ctx.lock);
		return CFG_ERR_NOT_INIT;
	}

	if (cfg_ctx.state != CFG_STATE_CONNECTED) {
		k_mutex_unlock(&cfg_ctx.lock);
		return CFG_ERR_NOT_CONNECTED;
	}

	k_mutex_unlock(&cfg_ctx.lock);

	switch (resp_type) {
	case CFG_RESP_READ:
		if (data_struct == NULL) {
			return CFG_ERR_INVALID_ARG;
		}
		cfg_send_read_response(sec_id, data_struct);
		break;

	case CFG_RESP_WRITE_ACK:
		cfg_send_ack_nack(true, 5);
		break;
	case CFG_RESP_CLEAR_ACK:
		cfg_send_ack_nack(true, 6);
		break;
	case CFG_RESP_CONFIG_TYPE_ACK:
		cfg_send_ack_nack(true, 0);
		break;

	case CFG_RESP_WRITE_NACK:
		cfg_send_ack_nack(false, 5);
		break;
	case CFG_RESP_CLEAR_NACK:
		cfg_send_ack_nack(false, 6);
		break;
	case CFG_RESP_CONFIG_TYPE_NACK:
		cfg_send_ack_nack(false, 0);
		break;

	default:
		return CFG_ERR_INVALID_ARG;
	}

	return CFG_OK;
}