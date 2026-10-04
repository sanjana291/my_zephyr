/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Private, module-internal declarations. Never installed/exported;
 * the application must never include this header.
 */

#ifndef TCP_SERVER_INTERNAL_H_
#define TCP_SERVER_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <tcp_server/tcp_server.h>

/** Server lifecycle state (informational / used for transmit() gating). */
enum tcp_server_state {
	TCP_SERVER_STATE_STOPPED = 0,
	TCP_SERVER_STATE_LISTENING,
	TCP_SERVER_STATE_HANDSHAKE,
	TCP_SERVER_STATE_CONNECTED,
};

/** Single instance of module state. The library supports exactly one
 *  server instance, matching the single-client requirement.
 */
struct tcp_server_ctx {
	/* Sockets */
	int server_sock;
	int client_sock;                /* -1 when no client is attached */

	/* Config (resolved: Kconfig defaults already applied) */
	uint16_t port;
	uint8_t handshake[TCP_SERVER_HANDSHAKE_MAX_LEN];
	size_t handshake_len;
	uint32_t handshake_timeout_ms;

	/* Only source address accept() is allowed to hand us a session for. */
	struct in_addr allowed_client_addr;

	/* Resolved TCP keepalive parameters, applied after handshake success. */
	int keepalive_idle_sec;
	int keepalive_interval_sec;
	int keepalive_count;

	/* Callbacks */
	tcp_server_receive_cb_t receive_cb;
	void *receive_cb_user_data;
	tcp_server_event_cb_t event_cb;
	void *event_cb_user_data;

	/* Protects client_sock / state against concurrent
	 * transmit() calls from application context while the server
	 * thread is accepting/receiving/tearing down.
	 */
	struct k_mutex lock;

	volatile enum tcp_server_state state;
	volatile bool running;

	/* Server thread */
	struct k_thread thread;
	k_tid_t thread_id;
	K_KERNEL_STACK_MEMBER(stack, CONFIG_TCP_SERVER_THREAD_STACK_SIZE);
};

/* Shared singleton, defined in tcp_server.c */
extern struct tcp_server_ctx tcp_server_ctx;

/* Internal helper used by both tcp_server.c and tcp_server_packet.c */
void tcp_server_notify_event(struct tcp_server_ctx *ctx,
			      enum tcp_server_event event,
			      enum tcp_server_error error);

/**
 * @brief Read and verify the handshake sequence from a freshly accepted
 * client socket, with a timeout.
 *
 * @retval 0   handshake matched.
 * @retval -ETIMEDOUT no complete handshake within the timeout window.
 * @retval -EINVAL    handshake bytes received but did not match.
 * @retval -ECONNRESET peer closed the connection early.
 */
int tcp_server_handshake_perform(struct tcp_server_ctx *ctx, int client_sock);

/**
 * @brief Run the data-phase read loop for the given (already
 * authenticated) client socket: frames inbound bytes, parses/validates
 * JSON, and invokes the receive callback for each valid packet.
 *
 * Blocks until the peer disconnects or a socket error occurs.
 */
void tcp_server_session_run(struct tcp_server_ctx *ctx, int client_sock);

/**
 * @brief Enable and configure the TCP keepalive mechanism on a freshly
 * validated client socket. Best-effort: logs and continues on failure
 * rather than dropping an otherwise-valid session.
 */
void tcp_server_enable_keepalive(struct tcp_server_ctx *ctx, int client_sock);

#endif /* TCP_SERVER_INTERNAL_H_ */
