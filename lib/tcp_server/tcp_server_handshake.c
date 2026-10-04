/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include "tcp_server_internal.h"
#include "tcp_server_frame.h"

LOG_MODULE_DECLARE(tcp_server, CONFIG_TCP_SERVER_LOG_LEVEL);

/* Longest handshake frame we'll buffer: the JSON wrapper plus the
 * configured SEQ string plus slack for extra whitespace/fields a real
 * client might send. Independent of TCP_SERVER_HANDSHAKE_MAX_LEN, which
 * only bounds the raw SEQ *value* stored in ctx->handshake.
 */
#define HANDSHAKE_BUF_MAX (TCP_SERVER_HANDSHAKE_MAX_LEN + 48)

static void send_ack(int client_sock, bool ack)
{
	static const char ack_msg[] = "{\"MTP\":\"01\",\"MSG\":0,\"RTP\":\"02\"}";
	const char *msg = ack_msg ;

	(void)zsock_send(client_sock, msg, strlen(msg), 0);
}

/* buf[*pos] must be the opening '"' of a JSON string. Copies the
 * unescaped content into out (truncating if it doesn't fit) and leaves
 * *pos just past the closing '"'. Minimal escape handling (\" \\ and
 * passthrough for anything else) - sufficient for this protocol, which
 * never nests structures or uses non-string values.
 */
static int scan_json_string(const char *buf, size_t len, size_t *pos,
			     char *out, size_t out_cap)
{
	size_t i = *pos;
	size_t oi = 0;

	if (i >= len || buf[i] != '"') {
		return -1;
	}
	i++;

	while (i < len && buf[i] != '"') {
		char c = buf[i];

		if (c == '\\' && i + 1 < len) {
			i++;
			c = buf[i];
		}
		if (oi + 1 < out_cap) {
			out[oi++] = c;
		}
		i++;
	}

	if (i >= len) {
		return -1;
	}

	out[oi] = '\0';
	*pos = i + 1;
	return 0;
}

/* Finds "key":"value" anywhere in buf (single level, string values
 * only - matches every message in the Whizz Studio protocol) and
 * copies the value out.
 */
static int extract_json_string_field(const char *buf, size_t len, const char *key,
				      char *out, size_t out_cap)
{
	size_t key_len = strlen(key);

	for (size_t i = 0; i + key_len + 2 < len; i++) {
		if (buf[i] != '"' || strncmp(&buf[i + 1], key, key_len) != 0 ||
		    buf[i + 1 + key_len] != '"') {
			continue;
		}

		size_t p = i + 1 + key_len + 1;

		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) {
			p++;
		}
		if (p >= len || buf[p] != ':') {
			continue;
		}
		p++;
		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) {
			p++;
		}
		if (scan_json_string(buf, len, &p, out, out_cap) < 0) {
			continue;
		}
		return 0;
	}

	return -1;
}


int tcp_server_handshake_perform(struct tcp_server_ctx *ctx, int client_sock)
{
	static char buf[HANDSHAKE_BUF_MAX];
	size_t got = 0;
	int64_t deadline = k_uptime_get() + (int64_t)ctx->handshake_timeout_ms;
	size_t frame_start = 0;
	int frame_len = -1;

	while (frame_len < 0) {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0) {
			LOG_INF("Handshake timed out");
			return -ETIMEDOUT;
		}

		if (got >= sizeof(buf)) {
			LOG_WRN("Handshake packet too large (> %zu bytes)", sizeof(buf));
			return -EMSGSIZE;
		}

		struct zsock_pollfd pfd = {
			.fd = client_sock,
			.events = ZSOCK_POLLIN,
		};
		int pret = zsock_poll(&pfd, 1, (int)remaining);

		if (pret == 0) {
			LOG_INF("Handshake timed out");
			return -ETIMEDOUT;
		}
		if (pret < 0) {
			LOG_WRN("poll() during handshake failed: %d", errno);
			return -EIO;
		}
		if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL)) {
			LOG_WRN("Socket error during handshake (revents=0x%x)", pfd.revents);
			return -EIO;
		}

		ssize_t len = zsock_recv(client_sock, buf + got, sizeof(buf) - got, 0);

		if (len == 0) {
			LOG_INF("Peer closed during handshake");
			return -ECONNRESET;
		}
		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue; /* spurious wakeup, deadline still governs */
			}
			LOG_WRN("recv() during handshake failed: %d", errno);
			return -EIO;
		}

		got += (size_t)len;

		frame_len = tcp_server_frame_scan(buf, got, &frame_start);
		if (frame_len == -2) {
			LOG_WRN("First packet was not valid JSON");
			return -EINVAL;
		}
	}

	const char *frame = buf + frame_start;
	size_t flen = (size_t)frame_len;

	char mtp[8];
	char seq[TCP_SERVER_HANDSHAKE_MAX_LEN + 1];

	if (extract_json_string_field(frame, flen, "MTP", mtp, sizeof(mtp)) < 0 ||
	    strcmp(mtp, "02") != 0) {
		LOG_WRN("First packet was not a Security Sequence (MTP 02)");
		return -EINVAL;
	}

	if (extract_json_string_field(frame, flen, "SEQ", seq, sizeof(seq)) < 0) {
		LOG_WRN("Security Sequence packet missing SEQ field");
		return -EINVAL;
	}

	if (strlen(seq) != ctx->handshake_len ||
	    memcmp(seq, ctx->handshake, ctx->handshake_len) != 0) {
		LOG_WRN("Security Sequence mismatch");
		return -EINVAL;
	}

	send_ack(client_sock, true);

	return 0;
}
