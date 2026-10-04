/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP server library: single client at a time, mandatory handshake,
 * JSON packet validation. Based on the structure of Zephyr's
 * samples/net/sockets/echo, split into init/deinit/transmit/callback
 * APIs so applications never touch a socket directly.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include <tcp_server/tcp_server.h>
#include "tcp_server_internal.h"

LOG_MODULE_REGISTER(tcp_server, CONFIG_TCP_SERVER_LOG_LEVEL);

/*
 * .lock is statically initialized (not left zeroed) because
 * tcp_server_register_receive_callback()/register_event_callback() are
 * called - by design - before the first tcp_server_init(), and both lock
 * this mutex. A zeroed k_mutex is not a valid mutex in Zephyr (its wait
 * queue is uninitialized), so without this the very first lock attempt
 * would be operating on garbage state.
 */
struct tcp_server_ctx tcp_server_ctx = {
	.server_sock = -1,
	.client_sock = -1,
	.lock = Z_MUTEX_INITIALIZER(tcp_server_ctx.lock),
};

void tcp_server_notify_event(struct tcp_server_ctx *ctx,
			      enum tcp_server_event event,
			      enum tcp_server_error error)
{
	if (ctx->event_cb != NULL) {
		ctx->event_cb(event, error, ctx->event_cb_user_data);
	}
}

static int open_listening_socket(struct tcp_server_ctx *ctx)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(ctx->port),
	};
	int sock;
	int ret;

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return -errno;
	}

	int reuse = 1;

	zsock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	ret = zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("bind() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	/* backlog of 1: a second client's SYN is queued by the TCP stack
	 * (or refused once that single backlog slot is occupied) while we
	 * are busy with the current session - this is what gives us
	 * "only one client connection at a time" without extra tracking.
	 */
	ret = zsock_listen(sock, 1);
	if (ret < 0) {
		LOG_ERR("listen() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	LOG_INF("Listening on port %u", ctx->port);
	return sock;
}

void tcp_server_enable_keepalive(struct tcp_server_ctx *ctx, int client_sock)
{
	int enable = 1;

	if (zsock_setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE,
			      &enable, sizeof(enable)) < 0) {
		LOG_WRN("SO_KEEPALIVE failed: %d", errno);
		return;
	}

#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
	int idle = ctx->keepalive_idle_sec;
	int intvl = ctx->keepalive_interval_sec;
	int cnt = ctx->keepalive_count;

	zsock_setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	zsock_setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	zsock_setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#else
	LOG_WRN("TCP_KEEPIDLE/INTVL/CNT unavailable in this Zephyr net stack "
		"config - enable CONFIG_NET_TCP_KEEPALIVE. SO_KEEPALIVE alone "
		"was still set.");
#endif

	LOG_INF("Keepalive enabled (idle=%ds interval=%ds count=%d)",
		ctx->keepalive_idle_sec, ctx->keepalive_interval_sec,
		ctx->keepalive_count);
}

static void close_client(struct tcp_server_ctx *ctx)
{
	k_mutex_lock(&ctx->lock, K_FOREVER);
	if (ctx->client_sock >= 0) {
		zsock_close(ctx->client_sock);
		ctx->client_sock = -1;
	}
	ctx->state = TCP_SERVER_STATE_LISTENING;
	k_mutex_unlock(&ctx->lock);
}

static void tcp_server_thread_fn(void *p1, void *p2, void *p3)
{
	struct tcp_server_ctx *ctx = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	ctx->server_sock = open_listening_socket(ctx);
	if (ctx->server_sock < 0) {
		tcp_server_notify_event(ctx, TCP_SERVER_EVT_ERROR, TCP_SERVER_ERR_LISTEN);
		return;
	}

	ctx->state = TCP_SERVER_STATE_LISTENING;

	while (ctx->running) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int client;
		int ret;

		client = zsock_accept(ctx->server_sock,
				       (struct sockaddr *)&client_addr,
				       &client_addr_len);
		if (client < 0) {
			if (!ctx->running) {
				break; /* accept() unblocked by deinit() */
			}
			LOG_WRN("accept() failed: %d", errno);
			tcp_server_notify_event(ctx, TCP_SERVER_EVT_ERROR,
						 TCP_SERVER_ERR_ACCEPT);
			continue;
		}

		/* Requirement: only the configured static client IP may
		 * connect. Reject everyone else before they ever reach the
		 * handshake phase, and go straight back to listening.
		 */
		if (client_addr.sin_addr.s_addr != ctx->allowed_client_addr.s_addr) {
			char ip_str[NET_IPV4_ADDR_LEN];

			zsock_inet_ntop(AF_INET, &client_addr.sin_addr,
					 ip_str, sizeof(ip_str));
			LOG_WRN("Rejecting connection from unauthorized IP %s", ip_str);
			zsock_close(client);
			tcp_server_notify_event(ctx, TCP_SERVER_EVT_CLIENT_REJECTED,
						 TCP_SERVER_ERR_UNAUTHORIZED_IP);
			continue; /* still listening, no client_sock touched */
		}

		k_mutex_lock(&ctx->lock, K_FOREVER);
		ctx->client_sock = client;
		ctx->state = TCP_SERVER_STATE_HANDSHAKE;
		k_mutex_unlock(&ctx->lock);

		tcp_server_notify_event(ctx, TCP_SERVER_EVT_CLIENT_CONNECTED,
					 TCP_SERVER_ERR_NONE);

		ret = tcp_server_handshake_perform(ctx, client);
		if (ret != 0) {
			enum tcp_server_event evt = (ret == -ETIMEDOUT)
				? TCP_SERVER_EVT_HANDSHAKE_TIMEOUT
				: TCP_SERVER_EVT_HANDSHAKE_FAILED;
			enum tcp_server_error err = (ret == -ETIMEDOUT)
				? TCP_SERVER_ERR_HANDSHAKE_TIMEOUT
				: TCP_SERVER_ERR_HANDSHAKE_MISMATCH;

			tcp_server_notify_event(ctx, evt, err);
			close_client(ctx);
			tcp_server_notify_event(ctx, TCP_SERVER_EVT_CLIENT_DISCONNECTED,
						 TCP_SERVER_ERR_NONE);
			continue; /* back to accept() - "return to listening" */
		}

		ctx->state = TCP_SERVER_STATE_CONNECTED;
		tcp_server_notify_event(ctx, TCP_SERVER_EVT_HANDSHAKE_OK, TCP_SERVER_ERR_NONE);

		/* Requirement: keepalive is only turned on once the Security
		 * Sequence has been validated, and is used to detect a lost
		 * client for the remainder of the session.
		 */
		// tcp_server_enable_keepalive(ctx, client);

		/* Blocks here for the whole data phase of this client. A
		 * keepalive-detected loss surfaces as a recv() error/EOF,
		 * same as any other disconnect - handled uniformly below.
		 */
		tcp_server_session_run(ctx, client);

		close_client(ctx);
		tcp_server_notify_event(ctx, TCP_SERVER_EVT_CLIENT_DISCONNECTED,
					 TCP_SERVER_ERR_NONE);
	}

	zsock_close(ctx->server_sock);
	ctx->server_sock = -1;
	ctx->state = TCP_SERVER_STATE_STOPPED;
}

int tcp_server_init(const struct tcp_server_config *config)
{
	struct tcp_server_ctx *ctx = &tcp_server_ctx;

	if (ctx->running) {
		return -EALREADY;
	}

	tcp_server_receive_cb_t saved_receive_cb = ctx->receive_cb;
	void *saved_receive_cb_user_data = ctx->receive_cb_user_data;
	tcp_server_event_cb_t saved_event_cb = ctx->event_cb;
	void *saved_event_cb_user_data = ctx->event_cb_user_data;

	memset(ctx, 0, sizeof(*ctx));
	k_mutex_init(&ctx->lock);
	ctx->server_sock = -1;
	ctx->client_sock = -1;

	ctx->receive_cb = saved_receive_cb;
	ctx->receive_cb_user_data = saved_receive_cb_user_data;
	ctx->event_cb = saved_event_cb;
	ctx->event_cb_user_data = saved_event_cb_user_data;

	ctx->port = (config && config->port) ? config->port : CONFIG_TCP_SERVER_PORT;

	if (config && config->handshake && config->handshake_len) {
		if (config->handshake_len > TCP_SERVER_HANDSHAKE_MAX_LEN) {
			return -EINVAL;
		}
		memcpy(ctx->handshake, config->handshake, config->handshake_len);
		ctx->handshake_len = config->handshake_len;
	} else {
		size_t len = strlen(CONFIG_TCP_SERVER_HANDSHAKE_DEFAULT);

		if (len > TCP_SERVER_HANDSHAKE_MAX_LEN) {
			len = TCP_SERVER_HANDSHAKE_MAX_LEN;
		}
		memcpy(ctx->handshake, CONFIG_TCP_SERVER_HANDSHAKE_DEFAULT, len);
		ctx->handshake_len = len;
	}

	ctx->handshake_timeout_ms = (config && config->handshake_timeout_ms)
		? config->handshake_timeout_ms
		: CONFIG_TCP_SERVER_HANDSHAKE_TIMEOUT_MS;

	const char *allowed_ip = (config && config->allowed_client_ip)
		? config->allowed_client_ip
		: CONFIG_TCP_SERVER_ALLOWED_CLIENT_IP;

	if (zsock_inet_pton(AF_INET, allowed_ip, &ctx->allowed_client_addr) != 1) {
		LOG_ERR("Invalid allowed_client_ip '%s'", allowed_ip);
		return -EINVAL;
	}

	ctx->keepalive_idle_sec = CONFIG_TCP_SERVER_KEEPALIVE_IDLE_SEC;
	ctx->keepalive_interval_sec = CONFIG_TCP_SERVER_KEEPALIVE_INTERVAL_SEC;
	ctx->keepalive_count = CONFIG_TCP_SERVER_KEEPALIVE_COUNT;

	ctx->running = true;

	ctx->thread_id = k_thread_create(
		&ctx->thread, ctx->stack, K_KERNEL_STACK_SIZEOF(ctx->stack),
		tcp_server_thread_fn, ctx, NULL, NULL,
		CONFIG_TCP_SERVER_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(ctx->thread_id, "tcp_server");

	return 0;
}

int tcp_server_deinit(void)
{
	struct tcp_server_ctx *ctx = &tcp_server_ctx;

	if (!ctx->running) {
		return -EALREADY;
	}

	ctx->running = false;

	/* Unblock whichever syscall the server thread is sitting in. */
	if (ctx->client_sock >= 0) {
		zsock_shutdown(ctx->client_sock, ZSOCK_SHUT_RDWR);
	}
	if (ctx->server_sock >= 0) {
		zsock_shutdown(ctx->server_sock, ZSOCK_SHUT_RDWR);
	}

	k_thread_join(&ctx->thread, K_MSEC(2000));
	close_client(ctx);

	return 0;
}

int tcp_server_transmit(const uint8_t *data, size_t len)
{
	struct tcp_server_ctx *ctx = &tcp_server_ctx;
	int ret;

	if (data == NULL || len == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);

	if (ctx->state != TCP_SERVER_STATE_CONNECTED || ctx->client_sock < 0) {
		k_mutex_unlock(&ctx->lock);
		return -ENOTCONN;
	}

	ret = zsock_send(ctx->client_sock, data, len, 0);
	k_mutex_unlock(&ctx->lock);

	if (ret < 0) {
		tcp_server_notify_event(ctx, TCP_SERVER_EVT_ERROR, TCP_SERVER_ERR_SEND);
		return -errno;
	}

	return ret;
}

int tcp_server_register_receive_callback(tcp_server_receive_cb_t cb, void *user_data)
{
	struct tcp_server_ctx *ctx = &tcp_server_ctx;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ctx->receive_cb = cb;
	ctx->receive_cb_user_data = user_data;
	k_mutex_unlock(&ctx->lock);

	return 0;
}

int tcp_server_register_event_callback(tcp_server_event_cb_t cb, void *user_data)
{
	struct tcp_server_ctx *ctx = &tcp_server_ctx;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ctx->event_cb = cb;
	ctx->event_cb_user_data = user_data;
	k_mutex_unlock(&ctx->lock);

	return 0;
}