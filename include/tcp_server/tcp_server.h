/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API of the TCP server library.
 *
 * The application never touches a BSD/Zephyr socket directly. It calls
 * tcp_server_init(), registers callbacks, and uses tcp_server_transmit().
 * Everything else (accept loop, single-client enforcement, handshake,
 * JSON packet framing/validation) is handled internally by the library.
 */

#ifndef TCP_SERVER_H_
#define TCP_SERVER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hard ceiling for a configured handshake ("Security Sequence") message.
 * The Whizz Security Sequence string is 47 bytes; sized with headroom.
 */
#define TCP_SERVER_HANDSHAKE_MAX_LEN 64

/**
 * @brief Lifecycle / status events reported to the application.
 *
 * Delivered through the optional event callback, from the library's
 * internal server thread context (not from an ISR).
 */
enum tcp_server_event {
	/** TCP client accepted, handshake not verified yet. */
	TCP_SERVER_EVT_CLIENT_CONNECTED,
	/** Client socket closed / connection lost (any phase). */
	TCP_SERVER_EVT_CLIENT_DISCONNECTED,
	/** Handshake sequence matched; data phase begins. */
	TCP_SERVER_EVT_HANDSHAKE_OK,
	/** Handshake bytes received but did not match. Client is dropped. */
	TCP_SERVER_EVT_HANDSHAKE_FAILED,
	/** Handshake not completed within the configured timeout. */
	TCP_SERVER_EVT_HANDSHAKE_TIMEOUT,
	/** A frame was received in the data phase but failed JSON/schema
	 *  validation. Not delivered to the receive callback.
	 */
	TCP_SERVER_EVT_PACKET_INVALID,
	/**
	 * A connection attempt was refused because it did not originate
	 * from the configured allowed client IP address. The socket is
	 * closed immediately; no CLIENT_CONNECTED/DISCONNECTED pair is
	 * reported for it.
	 */
	TCP_SERVER_EVT_CLIENT_REJECTED,
	/** Internal/socket error, see the accompanying error code. */
	TCP_SERVER_EVT_ERROR,
};

/** Error / reason codes, valid alongside some events (0 otherwise). */
enum tcp_server_error {
	TCP_SERVER_ERR_NONE = 0,
	TCP_SERVER_ERR_SOCKET,
	TCP_SERVER_ERR_BIND,
	TCP_SERVER_ERR_LISTEN,
	TCP_SERVER_ERR_ACCEPT,
	TCP_SERVER_ERR_HANDSHAKE_TIMEOUT,
	TCP_SERVER_ERR_HANDSHAKE_MISMATCH,
	TCP_SERVER_ERR_FRAME_TOO_LARGE,
	TCP_SERVER_ERR_JSON_PARSE,
	TCP_SERVER_ERR_JSON_SCHEMA,
	TCP_SERVER_ERR_SEND,
	TCP_SERVER_ERR_PEER_CLOSED,
	TCP_SERVER_ERR_NOT_CONNECTED,
	/** Connection source IP did not match the configured allowed client. */
	TCP_SERVER_ERR_UNAUTHORIZED_IP,
	/** Keepalive probes went unanswered; peer considered lost. */
	TCP_SERVER_ERR_KEEPALIVE_TIMEOUT,
	TCP_SERVER_ERR_INTERNAL,
};

/**
 * @brief Called for every frame that passed the library's JSON syntax
 * check (balanced/well-formed braces, brackets and strings; correct
 * top-level type).
 *
 * The library does NOT parse against a schema - it doesn't know your
 * packet's fields. `json` is handed to you as raw text so you can decode
 * it with your own struct + `json_obj_descr` array via
 * `zephyr/data/json.h`'s `json_obj_parse()`, or with any other JSON
 * decoder of your choice.
 *
 * @param json      Pointer to the frame text (NOT null-terminated - use
 *                  `json_len`). Owned by the library and only valid for
 *                  the duration of this callback; copy out anything you
 *                  need to keep.
 * @param json_len  Length of `json` in bytes.
 * @param user_data Opaque pointer passed to
 *                  tcp_server_register_receive_callback().
 */
typedef void (*tcp_server_receive_cb_t)(const char *json, size_t json_len, void *user_data);

/**
 * @brief Called on connection / handshake / error state changes.
 *
 * @param event     What happened.
 * @param error     Extra detail, TCP_SERVER_ERR_NONE if not applicable.
 * @param user_data Opaque pointer passed to
 *                  tcp_server_register_event_callback().
 */
typedef void (*tcp_server_event_cb_t)(enum tcp_server_event event,
				       enum tcp_server_error error,
				       void *user_data);

/**
 * @brief Runtime configuration for tcp_server_init().
 *
 * Any field left at 0 / NULL falls back to its Kconfig default, so a
 * caller happy with the defaults can zero-initialize this struct and
 * only set what it needs (e.g. the handshake sequence).
 */
struct tcp_server_config {
	/** TCP port to bind/listen on. 0 -> CONFIG_TCP_SERVER_PORT. */
	uint16_t port;

	/**
	 * Expected value of the "SEQ" field in the client's Security
	 * Sequence packet - {"MTP":"02","SEQ":"<this string>"} - which
	 * must be the first thing the client sends after connecting.
	 * NULL -> library falls back to CONFIG_TCP_SERVER_HANDSHAKE_DEFAULT.
	 */
	const uint8_t *handshake;

	/** Length of `handshake`, must be <= TCP_SERVER_HANDSHAKE_MAX_LEN. */
	size_t handshake_len;

	/**
	 * Handshake window in milliseconds, measured from the instant the
	 * TCP connection is accepted. 0 -> CONFIG_TCP_SERVER_HANDSHAKE_TIMEOUT_MS.
	 */
	uint32_t handshake_timeout_ms;

	/**
	 * Dotted-decimal IPv4 address of the only client the server will
	 * accept connections from (e.g. "192.168.1.15"). Every other
	 * source address is rejected immediately after accept(), before
	 * the handshake phase. NULL -> CONFIG_TCP_SERVER_ALLOWED_CLIENT_IP.
	 */
	const char *allowed_client_ip;
};

/**
 * @brief Initialize and start the TCP server.
 *
 * Spawns the internal server thread, which binds/listens on the
 * configured port and begins accepting a single client at a time.
 *
 * @param config Server configuration. May be NULL to accept all defaults.
 * @return 0 on success, negative errno on failure.
 */
int tcp_server_init(const struct tcp_server_config *config);

/**
 * @brief Stop the server, close any active connection and release
 * resources. Safe to call even if a client is currently connected.
 *
 * @return 0 on success, negative errno on failure.
 */
int tcp_server_deinit(void);

/**
 * @brief Send raw bytes to the currently connected client.
 *
 * Typically used to transmit an already-serialized JSON packet - a
 * single complete object/array, no trailing delimiter needed or
 * expected. Frame boundaries are detected structurally on the receive
 * side (brace/bracket depth returning to zero), matching how this
 * function's own output should look to a peer reading it the same way.
 *
 * @param data Buffer to send.
 * @param len  Number of bytes in `data`.
 * @return Number of bytes sent on success, negative errno on failure
 *         (in particular -ENOTCONN / TCP_SERVER_ERR_NOT_CONNECTED
 *         if no client is currently authenticated).
 */
int tcp_server_transmit(const uint8_t *data, size_t len);

/**
 * @brief Register (or replace) the callback invoked for every validated
 * inbound JSON packet.
 */
int tcp_server_register_receive_callback(tcp_server_receive_cb_t cb, void *user_data);

/**
 * @brief Register (or replace) the callback invoked on connection /
 * handshake / error events. Optional - the server works without one.
 */
int tcp_server_register_event_callback(tcp_server_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SERVER_H_ */