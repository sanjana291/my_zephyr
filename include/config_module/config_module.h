/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API of the Configuration Module.
 *
 * The Configuration Module owns the complete TCP configuration flow:
 *   1. TCP server lifecycle (init / deinit via the tcp_server library)
 *   2. IP allow-listing (handled by tcp_server)
 *   3. Security Sequence handshake (handled by tcp_server)
 *   4. Connection timer - configurable via CONFIG_CFG_MODULE_CONN_TIMEOUT_MS
 *   5. TCP keepalive (handled by tcp_server after handshake)
 *   6. JSON packet reception, framing, and structural parsing
 *   7. Protocol decode: MTP 00 (Configuration Type), MTP 03 (Read),
 *      MTP 05 (Write), MTP 06 (Clear)
 *   8. Range validation of every numeric field before any write
 *   9. JSON response construction: MTP 04 (Read Response), MTP 01 (ACK/NACK)
 *  10. SSID/password length fields calculated internally
 *
 * The application:
 *   - Calls CFG_Init() once at startup
 *   - Registers a callback with CFG_RegisterCallback()
 *   - Receives parsed config_params_t data via CFG_Reqst() callback
 *   - Learns whether the session carries Factory or User configuration via
 *     the CFG_EVT_CONFIG_TYPE_REQ event
 *   - Provides responses (read data or write-ack) via CFG_Resp()
 *
 * The application MUST NOT touch tcp_server directly; everything is
 * mediated through this module.
 *
 * RTC / system-time note:
 *   Section O carries a raw 7-byte "stm" array on the wire (see
 *   common_config/config_params.h). This module treats stm purely as
 *   opaque data: it is parsed from / encoded to JSON with basic bounds
 *   checking (each byte 0-255) only. The Configuration Module never reads
 *   or writes any RTC/real-time-clock peripheral - interpreting, storing,
 *   or applying that time data (e.g. to a hardware RTC) is entirely the
 *   application's responsibility.
 *
 * Packet types (from Whizz Studio Communication spec):
 *   MTP "00" - Configuration Type (Studio->Whizz) - Factory/User indicator
 *   MTP "01" - ACK / NACK       (both directions)
 *   MTP "02" - Security Sequence (Studio->Whizz, handled by tcp_server)
 *   MTP "03" - Read Request      (Studio->Whizz)
 *   MTP "04" - Read Response     (Whizz->Studio)
 *   MTP "05" - Write Request     (Studio->Whizz)
 *   MTP "06" - Clear Request     (Studio->Whizz)
 */

#ifndef CONFIG_MODULE_H_
#define CONFIG_MODULE_H_

#include <stdint.h>
#include <stdbool.h>
#include <common_config/config_params.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 * ------------------------------------------------------------------------- */

typedef enum {
	CFG_OK                  = 0,   /**< Success                           */
	CFG_ERR_INVALID_ARG     = 1,   /**< NULL pointer or bad parameter     */
	CFG_ERR_NOT_INIT        = 2,   /**< CFG_Init() not called yet         */
	CFG_ERR_ALREADY_INIT    = 3,   /**< CFG_Init() called more than once  */
	CFG_ERR_TCP_FAIL        = 4,   /**< tcp_server_init() failed          */
	CFG_ERR_NOT_CONNECTED   = 5,   /**< No client currently connected     */
	CFG_ERR_SEND_FAIL       = 6,   /**< tcp_server_transmit() failed      */
	CFG_ERR_INTERNAL        = 7,   /**< Unexpected internal error         */
} config_module_return_t;

/* ---------------------------------------------------------------------------
 * Configuration type (Factory vs User) - MTP "00"
 * ------------------------------------------------------------------------- */

/**
 * @brief Identifies whether the configuration data belongs to the Factory
 *        default set or a User-provided set, as declared by Studio via the
 *        MTP "00" packet ({"MTP":"00","CTP":x}).
 */
typedef enum {
	CFG_TYPE_FACTORY = 0,   /**< CTP == 0: Factory default configuration */
	CFG_TYPE_USER    = 1,   /**< CTP == 1: User configuration            */
} config_type_t;

/**
 * @brief Carries the Configuration Type for CFG_EVT_CONFIG_TYPE_REQ.
 */
typedef struct {
	config_type_t type;   /**< CFG_TYPE_FACTORY or CFG_TYPE_USER */
} config_type_req_t;

/* ---------------------------------------------------------------------------
 * Event types delivered to the application callback (CFG_Reqst)
 * ------------------------------------------------------------------------- */

/**
 * @brief Event types passed to the registered request callback.
 *
 * CFG_EVT_CONFIG_TYPE_REQ - Studio declared whether the session that
 *                     follows carries Factory or User configuration;
 *                     data_struct is a pointer to a config_type_req_t.
 *                     The application must call
 *                     CFG_Resp(CFG_RESP_CONFIG_TYPE_ACK, ...) or
 *                     CFG_Resp(CFG_RESP_CONFIG_TYPE_NACK, ...).
 *
 * CFG_EVT_READ_REQ  - Studio wants to read a section; data_struct is a
 *                     pointer to a config_read_req_t carrying the section ID.
 *                     The application must call CFG_Resp(CFG_RESP_READ, ...).
 *
 * CFG_EVT_WRITE_REQ - Studio has written a section; data_struct is a
 *                     pointer to config_write_req_t with the parsed &
 *                     validated new values. Only the section identified by
 *                     the SID in the packet has meaningful content. The
 *                     application must call
 *                     CFG_Resp(CFG_RESP_WRITE_ACK, ...) or
 *                     CFG_Resp(CFG_RESP_WRITE_NACK, ...).
 *
 * CFG_EVT_CLEAR_REQ - Studio wants to clear one or all sections; data_struct
 *                     is a pointer to config_read_req_t carrying the section
 *                     ID ('A'..'O') or '0' for clear-all. The application
 *                     must call CFG_Resp(CFG_RESP_CLEAR_ACK, ...).
 *
 * CFG_EVT_CONNECTED    - TCP client connected and security handshake passed.
 * CFG_EVT_DISCONNECTED - TCP client disconnected (any reason).
 * CFG_EVT_CONN_TIMEOUT - Connection timer expired; client was disconnected.
 */
typedef enum {
	CFG_EVT_CONFIG_TYPE_REQ = 0,
	CFG_EVT_READ_REQ        = 1,
	CFG_EVT_WRITE_REQ       = 2,
	CFG_EVT_CLEAR_REQ       = 3,
	CFG_EVT_CONNECTED       = 4,
	CFG_EVT_DISCONNECTED    = 5,
	CFG_EVT_CONN_TIMEOUT    = 6,
} config_evt_type_t;

/* ---------------------------------------------------------------------------
 * Response types passed to CFG_Resp()
 * ------------------------------------------------------------------------- */

typedef enum {
	CFG_RESP_READ              = 0,   /**< Provide read data; data_struct=config_params_t*  */
	CFG_RESP_WRITE_ACK         = 1,   /**< Acknowledge a write; data_struct may be NULL     */
	CFG_RESP_WRITE_NACK        = 2,   /**< Reject a write; data_struct may be NULL          */
	CFG_RESP_CLEAR_ACK         = 3,   /**< Acknowledge a clear; data_struct may be NULL     */
	CFG_RESP_CLEAR_NACK        = 4,   /**< Reject a clear; data_struct may be NULL          */
	CFG_RESP_CONFIG_TYPE_ACK   = 5,   /**< Acknowledge MTP 00; data_struct may be NULL      */
	CFG_RESP_CONFIG_TYPE_NACK  = 6,   /**< Reject MTP 00; data_struct may be NULL           */
} config_resp_type_t;

/* ---------------------------------------------------------------------------
 * Auxiliary structures
 * ------------------------------------------------------------------------- */

/**
 * @brief Carries the Section ID for Read and Clear requests.
 *
 * sec_id:
 *   'A'..'O' - specific section
 *   '0'      - all sections (Clear only; Read with '0' is a NACK)
 *   '?' (CFG_SID_UNKNOWN) - unrecognised section ID
 */
#define CFG_SID_UNKNOWN '?'

typedef struct {
	char sec_id;   /**< Section identifier character */
} config_read_req_t;

/* ---------------------------------------------------------------------------
 * Callback type
 * ------------------------------------------------------------------------- */

/**
 * @brief Application callback invoked by the Configuration Module.
 *
 * Runs from the tcp_server thread context (not an ISR).
 * The application MUST NOT call CFG_Init() or CFG_Deinit() from inside
 * this callback.
 *
 * @param evt_type   What happened (config_evt_type_t).
 * @param data_struct Pointer to the event payload; type depends on evt_type:
 *                    CFG_EVT_CONFIG_TYPE_REQ -> config_type_req_t *
 *                    CFG_EVT_READ_REQ        -> config_read_req_t *
 *                    CFG_EVT_WRITE_REQ       -> config_write_req_t *
 *                    CFG_EVT_CLEAR_REQ       -> config_read_req_t *
 *                    CFG_EVT_CONNECTED / CFG_EVT_DISCONNECTED /
 *                    CFG_EVT_CONN_TIMEOUT    -> NULL
 */
typedef void (*cfg_request_cb_t)(config_evt_type_t evt_type, void *data_struct);

/**
 * @brief Write-request payload delivered for CFG_EVT_WRITE_REQ.
 *
 * Only the section identified by sec_id has been populated; all other
 * sections in params carry undefined values and must be ignored.
 */
typedef struct {
	char           sec_id;   /**< Section that was written */
	config_params_t params;  /**< Parsed parameter values  */
} config_write_req_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialise the Configuration Module and start the TCP server.
 *
 * Internally calls tcp_server_init(), registers the module's own receive
 * and event callbacks, and starts the connection timer work queue.
 * Must be called exactly once, after the network stack is up.
 *
 * @return CFG_OK on success, or a CFG_ERR_* code on failure.
 */
config_module_return_t CFG_Init(void);

/**
 * @brief Stop the TCP server and release all Configuration Module resources.
 *
 * Safe to call even while a client is connected.
 *
 * @return CFG_OK on success, or a CFG_ERR_* code on failure.
 */
config_module_return_t CFG_Deinit(void);

/**
 * @brief Register the application callback.
 *
 * May be called before or after CFG_Init(). Only one callback is supported
 * at a time; a second call replaces the first.
 *
 * @param cb  Callback function pointer. NULL removes the current callback.
 * @return CFG_OK, or CFG_ERR_INVALID_ARG if cb is NULL.
 */
config_module_return_t CFG_RegisterCallback(cfg_request_cb_t cb);

/**
 * @brief Send a response to the TCP client.
 *
 * Serialises the appropriate JSON packet and transmits it via
 * tcp_server_transmit().
 *
 * @param resp_type  What kind of response (config_resp_type_t).
 * @param sec_id     The section being responded to ('A'..'O'; ignored for
 *                   NACK/ACK-only responses when data_struct is NULL).
 * @param data_struct For CFG_RESP_READ: pointer to config_params_t
 *                    containing the section's current values. The module
 *                    reads only the section identified by sec_id.
 *                    For all other resp_types: may be NULL.
 *
 * @return CFG_OK on success.
 *         CFG_ERR_NOT_INIT if CFG_Init() hasn't been called.
 *         CFG_ERR_NOT_CONNECTED if no client is currently authenticated.
 *         CFG_ERR_SEND_FAIL if the TCP transmit failed.
 *         CFG_ERR_INVALID_ARG if resp_type is CFG_RESP_READ and
 *         data_struct is NULL.
 */
config_module_return_t CFG_Resp(config_resp_type_t resp_type,
				char sec_id,
				const config_params_t *data_struct);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MODULE_H_ */