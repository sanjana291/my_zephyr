/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include "tcp_server_frame.h"
#include <tcp_server/tcp_server.h> /* Kconfig-backed CONFIG_TCP_SERVER_* */

static bool is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int tcp_server_frame_scan(const char *buf, size_t len, size_t *out_start)
{
	size_t i = 0;

	while (i < len && is_ws(buf[i])) {
		i++;
	}
	if (i >= len) {
		return -1; /* nothing but whitespace so far */
	}

	if (buf[i] != '{'
#if defined(CONFIG_TCP_SERVER_ALLOW_ARRAY_ROOT)
	    && buf[i] != '['
#endif
	   ) {
		return -2;
	}

	size_t start = i;
	int depth = 0;
	bool in_string = false;
	bool escape = false;

	for (; i < len; i++) {
		char c = buf[i];

		if (in_string) {
			if (escape) {
				escape = false;
			} else if (c == '\\') {
				escape = true;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}

		switch (c) {
		case '"':
			in_string = true;
			break;
		case '{':
		case '[':
			depth++;
			break;
		case '}':
		case ']':
			depth--;
			if (depth < 0) {
				return -2; /* unbalanced closer */
			}
			if (depth == 0) {
				*out_start = start;
				return (int)(i - start + 1);
			}
			break;
		default:
			break;
		}
	}

	return -1; /* ran out of bytes before the value closed */
}
