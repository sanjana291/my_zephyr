/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TCP_SERVER_FRAME_H_
#define TCP_SERVER_FRAME_H_

#include <stddef.h>

/**
 * @brief Scan `buf[0..len)` for the first complete top-level JSON value
 * (an object, or an array if CONFIG_TCP_SERVER_ALLOW_ARRAY_ROOT is set),
 * skipping any leading whitespace.
 *
 * No delimiter (newline or otherwise) is required or assumed between
 * frames - a frame is considered complete the instant its brace/bracket
 * depth returns to zero, tracking quoted-string and escape state so a
 * `}`/`]` inside a string doesn't count. This works whether a client
 * appends a trailing newline to each packet or not.
 *
 * @param buf       Bytes accumulated so far for this connection.
 * @param len       Number of valid bytes in `buf`.
 * @param out_start Set to the offset of the frame's first byte (i.e.
 *                  leading whitespace skipped) when a complete frame is
 *                  found.
 * @return Length of the frame in bytes (>= 2, e.g. "{}") on success.
 *         -1 if `buf` is well-formed so far but incomplete - the caller
 *         should read more bytes and rescan the same buffer.
 *         -2 if `buf` can never become valid framing from this point
 *         (e.g. the first non-whitespace byte isn't '{' or a stray
 *         closing brace/bracket) - the caller should treat this as
 *         fatal, since without a delimiter there is no reliable way to
 *         resynchronize with the stream.
 */
int tcp_server_frame_scan(const char *buf, size_t len, size_t *out_start);

#endif /* TCP_SERVER_FRAME_H_ */
