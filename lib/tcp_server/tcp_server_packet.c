/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include "tcp_server_internal.h"
#include "tcp_server_frame.h"

LOG_MODULE_DECLARE(tcp_server, CONFIG_TCP_SERVER_LOG_LEVEL);

/*
 * JSON-structural framing: each application packet is a single JSON
 * value, and a frame is complete the instant its brace/bracket depth
 * returns to zero (see tcp_server_frame.c). No delimiter - newline or
 * otherwise - is required between frames, since real clients (and test
 * tools like Hercules) don't reliably send one. tcp_server_frame_scan()
 * validates structural syntax (brace/bracket balance, well-formed
 * quoted strings) as a side effect of finding the frame boundary; it's
 * not a schema validator - that's zephyr/data/json.h's job, or any
 * other JSON decoder, on the app side.
 */

static void handle_frame(struct tcp_server_ctx *ctx, const char *frame, size_t frame_len)
{
	if (ctx->receive_cb != NULL) {
		ctx->receive_cb(frame, frame_len, ctx->receive_cb_user_data);
	}
}

void tcp_server_session_run(struct tcp_server_ctx *ctx, int client_sock)
{
	static char rxbuf[CONFIG_TCP_SERVER_MAX_FRAME_SIZE];
	size_t used = 0;

	while (true) {
		ssize_t len = zsock_recv(client_sock, rxbuf + used,
					  sizeof(rxbuf) - used, 0);

		if (len == 0) {
			LOG_INF("Client closed the connection");
			return;
		}
		if (len < 0) {
			LOG_WRN("recv() failed: %d", errno);
			tcp_server_notify_event(ctx, TCP_SERVER_EVT_ERROR,
						 TCP_SERVER_ERR_PEER_CLOSED);
			return;
		}

		used += (size_t)len;

		/* Pull out every complete frame currently buffered - a
		 * client may pipeline several packets back-to-back in one
		 * TCP segment with nothing separating them.
		 */
		while (true) {
			size_t start;
			int flen = tcp_server_frame_scan(rxbuf, used, &start);

			if (flen == -1) {
				break; /* incomplete so far, need more bytes */
			}
			if (flen == -2) {
				LOG_WRN("Malformed JSON stream, dropping client");
				tcp_server_notify_event(ctx, TCP_SERVER_EVT_PACKET_INVALID,
							 TCP_SERVER_ERR_JSON_PARSE);
				return; /* no delimiter to resynchronize on */
			}

			handle_frame(ctx, &rxbuf[start], (size_t)flen);

			size_t consumed = start + (size_t)flen;

			memmove(rxbuf, rxbuf + consumed, used - consumed);
			used -= consumed;
		}

		if (used == sizeof(rxbuf)) {
			LOG_WRN("Frame exceeds CONFIG_TCP_SERVER_MAX_FRAME_SIZE, "
				"dropping client");
			tcp_server_notify_event(ctx, TCP_SERVER_EVT_ERROR,
						 TCP_SERVER_ERR_FRAME_TOO_LARGE);
			return;
		}
	}
}
