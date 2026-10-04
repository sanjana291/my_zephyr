/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Private declarations shared between config_module.c,
 *        config_module_json.c, and config_module_validate.c.
 *
 * Never installed or exported. Application code must never include this.
 */

#ifndef CONFIG_MODULE_INTERNAL_H_
#define CONFIG_MODULE_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <config_module/config_module.h>

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */

/** Lifecycle states of the configuration module. */
enum cfg_state {
	CFG_STATE_UNINIT     = 0,
	CFG_STATE_IDLE,        /* Initialised, no client connected   */
	CFG_STATE_CONNECTED,   /* Client authenticated, session live  */
};

/** Single global instance. */
struct cfg_ctx {
	enum cfg_state   state;
	cfg_request_cb_t app_cb;

	/** Delayed work item drives the connection timer. */
	struct k_work_delayable conn_timer;

	/** Protects state and app_cb from concurrent access. */
	struct k_mutex lock;

	/** Tracks the last SID seen in a Read/Clear request so CFG_Resp()
	 *  can embed it in the response without requiring the app to echo it. */
	char pending_sid;
};

extern struct cfg_ctx cfg_ctx;

/*
 * NOTE: The Configuration Module does NOT own, read, or write any RTC /
 * real-time-clock peripheral. Section O's "stm" field is treated as opaque
 * byte data on the wire; interpreting or applying it to hardware is the
 * application's responsibility.
 */

/* ---------------------------------------------------------------------------
 * JSON packet encode/decode (config_module_json.c)
 * ------------------------------------------------------------------------- */

/**
 * @brief Parse an inbound JSON packet from the TCP client.
 *
 * Extracts MTP, dispatches to the appropriate handler (Configuration Type /
 * Read / Write / Clear), and invokes the application callback via
 * cfg_ctx.app_cb. Sends ACK/NACK internally as required by the protocol.
 *
 * @param json     Frame text (NOT null-terminated).
 * @param json_len Length of frame in bytes.
 */
void cfg_json_dispatch(const char *json, size_t json_len);

/**
 * @brief Serialise a MTP 04 Read Response for the given section.
 *
 * @param buf      Output buffer.
 * @param buf_size Size of output buffer.
 * @param sec_id   Section character 'A'..'O'.
 * @param params   Full params struct; only the relevant section is read.
 * @return Number of bytes written (excluding NUL), or negative on error.
 */
int cfg_json_encode_read_response(char *buf, size_t buf_size,
				   char sec_id, const config_params_t *params);

/**
 * @brief Serialise a MTP 01 ACK or NACK.
 *
 * @param buf      Output buffer (needs at least 24 bytes).
 * @param buf_size Size of output buffer.
 * @param ack      true for ACK (MSG:0), false for NACK (MSG:1).
 * @param rtp      Request type this ACK/NACK refers to ("00","02","03","05","06").
 * @return Number of bytes written, or negative on error.
 */
int cfg_json_encode_ack_nack(char *buf, size_t buf_size, bool ack, uint8_t rtp);

/* ---------------------------------------------------------------------------
 * Field range validation (config_module_validate.c)
 * ------------------------------------------------------------------------- */

/**
 * @brief Validate all fields of the section identified by sec_id.
 *
 * @param sec_id   Section character.
 * @param params   Struct carrying the values to validate.
 * @return true if every field is within its documented range, false otherwise.
 */
bool cfg_validate_section(char sec_id, const config_params_t *params);

/* ---------------------------------------------------------------------------
 * Internal helpers shared within the module
 * ------------------------------------------------------------------------- */

/** Send a raw ACK or NACK to the current TCP client. */
void cfg_send_ack_nack(bool ack, uint8_t rtp);

/** Send a MTP 04 Read Response to the current TCP client. */
void cfg_send_read_response(char sec_id, const config_params_t *params);

/** Notify the application callback (safe to call with NULL cb). */
static inline void cfg_notify_app(config_evt_type_t evt, void *data)
{
	if (cfg_ctx.app_cb != NULL) {
		cfg_ctx.app_cb(evt, data);
	}
}

#endif /* CONFIG_MODULE_INTERNAL_H_ */