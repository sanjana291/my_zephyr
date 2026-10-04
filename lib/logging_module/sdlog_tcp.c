/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdlog_tcp.c
 * @brief TCP server integrated inside the sdlog module.
 *
 * Implements the full Logging Workflow sequence:
 *
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <logging_module/logging_module.h>
#include "sdlog_internal.h"
#include "sdlog_tcp.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

#if defined(CONFIG_SDLOG_TCP_SERVER)

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define TCP_RX_BUF_SIZE          512
#define TCP_TX_BUF_SIZE          128
#define SDLOG_TCP_MAX_LIST_FILES CONFIG_SDLOG_MAX_FILES_SCAN

/* ------------------------------------------------------------------ */
/* Static storage for the current batch of file names                  */
/* ------------------------------------------------------------------ */

static char             s_pending_names[SDLOG_TCP_MAX_LIST_FILES][SDLOG_FNAME_MAX];
static struct sdlog_file_info s_file_info[SDLOG_TCP_MAX_LIST_FILES];

/* ------------------------------------------------------------------ */
/* Server state                                                        */
/* ------------------------------------------------------------------ */

static struct {
	int           server_sock;
	int           client_sock;
	volatile bool running;
	struct k_thread thread;
	K_KERNEL_STACK_MEMBER(stack, CONFIG_SDLOG_TCP_THREAD_STACK_SIZE);
} s_srv = {
	.server_sock = -1,
	.client_sock = -1,
};

/* ------------------------------------------------------------------ */
/* Minimal JSON frame scanner                                          */
/* ------------------------------------------------------------------ */

/**
 * Scan buf[0..len) for the first complete top-level JSON object.
 * Returns the frame length (≥2) when found, -1 if incomplete,
 * -2 if the stream is irrecoverably malformed.
 */
static int frame_scan(const char *buf, size_t len, size_t *out_start)
{
	size_t i = 0;

	/* Skip leading whitespace. */
	while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
			   buf[i] == '\r' || buf[i] == '\n')) {
		i++;
	}
	if (i >= len) {
		return -1;
	}
	if (buf[i] != '{') {
		return -2;
	}

	size_t start     = i;
	int    depth     = 0;
	bool   in_string = false;
	bool   escape    = false;

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
		case '"': in_string = true; break;
		case '{': depth++;          break;
		case '}':
			if (--depth < 0) {
				return -2;
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
	return -1;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON field extractors                                       */
/* ------------------------------------------------------------------ */

/**
 * Extract the string value of "key" from buf.
 * Returns 0 on success, -1 if not found.
 */
static int json_get_str(const char *buf, size_t len,
			 const char *key, char *out, size_t out_cap)
{
	size_t klen = strlen(key);

	for (size_t i = 0; i + klen + 2 < len; i++) {
		if (buf[i] != '"' ||
		    strncmp(&buf[i + 1], key, klen) != 0 ||
		    buf[i + 1 + klen] != '"') {
			continue;
		}
		size_t p = i + 1 + klen + 1;

		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) { p++; }
		if (p >= len || buf[p] != ':') { continue; }
		p++;
		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) { p++; }
		if (p >= len || buf[p] != '"') { continue; }
		p++;

		size_t oi  = 0;
		bool   esc = false;

		while (p < len) {
			char c = buf[p++];

			if (esc)            { esc = false; }
			else if (c == '\\') { esc = true; continue; }
			else if (c == '"')  { break; }
			if (oi + 1 < out_cap) { out[oi++] = c; }
		}
		out[oi] = '\0';
		return 0;
	}
	return -1;
}

/**
 * Extract a [DD, MM, YYYY] date array value for "key" from buf.
 * Returns 0 on success, -1 on parse error.
 */
static int json_get_date_array(const char *buf, size_t len,
				const char *key,
				struct sdlog_proto_date *date)
{
	size_t klen = strlen(key);

	for (size_t i = 0; i + klen + 2 < len; i++) {
		if (buf[i] != '"' ||
		    strncmp(&buf[i + 1], key, klen) != 0 ||
		    buf[i + 1 + klen] != '"') {
			continue;
		}
		size_t p = i + 1 + klen + 1;

		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) { p++; }
		if (p >= len || buf[p] != ':') { continue; }
		p++;
		while (p < len && (buf[p] == ' ' || buf[p] == '\t')) { p++; }
		if (p >= len || buf[p] != '[') { continue; }
		p++;

		uint32_t vals[3] = {0, 0, 0};

		for (int vi = 0; vi < 3; vi++) {
			while (p < len && (buf[p] == ' ' || buf[p] == '\t' ||
					   buf[p] == ',')) {
				p++;
			}
			if (p >= len || !sdlog_parse_uint32(&buf[p], &vals[vi])) {
				return -1;
			}
			while (p < len && buf[p] >= '0' && buf[p] <= '9') { p++; }
		}

		date->day   = (uint8_t) vals[0]; /* DD   */
		date->month = (uint8_t) vals[1]; /* MM   */
		date->year  = (uint16_t)vals[2]; /* YYYY */
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Packet senders                                                      */
/* ------------------------------------------------------------------ */

static void send_ack(int sock, const char *rtp)
{
	char buf[64];

	snprintf(buf, sizeof(buf),
		 "{\"MTP\":\"01\",\"MSG\":0,\"RTP\":\"%s\"}", rtp);
	zsock_send(sock, buf, strlen(buf), 0);
}

static void send_nack(int sock, const char *rtp)
{
	char buf[64];

	snprintf(buf, sizeof(buf),
		 "{\"MTP\":\"01\",\"MSG\":1,\"RTP\":\"%s\"}", rtp);
	zsock_send(sock, buf, strlen(buf), 0);
}

/**
 * MTP 11 - Log File Transfer Start Status.
 * {"MTP":"11","TSA":x}
 *   TSA=0  TFTP server available and transmission started.
 *   TSA=1  TFTP server not available.
 *
 * Sent ONCE per Log Read Request, before any TFTP transfer begins.
 */
static void send_mtp11(int sock, int tsa)
{
	char buf[TCP_TX_BUF_SIZE];

	snprintf(buf, sizeof(buf),
		 "{\"MTP\":\"11\",\"TSA\":%d}", tsa);
	zsock_send(sock, buf, strlen(buf), 0);
	LOG_INF("MTP11 sent: TSA=%d", tsa);
}

/**
 * MTP 12 - Log Read Result.
 * {"MTP":"12","STS":x,"FNC":y,"FNO":z}
 *   STS=0  All transfers succeeded.  FNO must be 0.
 *   STS=1  Transfer failed.          FNO = 1-based number of failing file.
 *
 * Sent ONCE per Log Read Request, after the batch completes or fails.
 */
static void send_mtp12(int sock, int sts, uint32_t fnc, uint32_t fno)
{
	char buf[TCP_TX_BUF_SIZE];

	snprintf(buf, sizeof(buf),
		 "{\"MTP\":\"12\",\"STS\":%d,\"FNC\":%u,\"FNO\":%u}",
		 sts, (unsigned int)fnc, (unsigned int)fno);
	zsock_send(sock, buf, strlen(buf), 0);
	LOG_INF("MTP12 sent: STS=%d FNC=%u FNO=%u",
		sts, (unsigned int)fnc, (unsigned int)fno);
}

/* ------------------------------------------------------------------ */
/* MTP 07 handler - Log Files Information Request                      */
/* {"MTP":"07","LFI":x}  Studio → Whizz                               */
/* Response: {"MTP":"08","TFC":x,"SDT":[DD,MM,YYYY],"EDT":[DD,MM,YYYY]}*/
/* ------------------------------------------------------------------ */
static void handle_log_files_info_req(int sock,
				       const char *frame, size_t flen)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(flen);

	struct sdlog_date oldest, newest;
	uint32_t total = 0;
	char resp[256];
	int  ret;

	ret = sdlog_do_get_file_count(&total);
	if (ret != 0) {
		LOG_ERR("MTP07: sdlog_do_get_file_count failed (%d)", ret);
		send_nack(sock, SDLOG_MTP_LOG_FILES_INFO_REQ);
		return;
	}

	if (total == 0) {
		snprintf(resp, sizeof(resp),
			 "{\"MTP\":\"08\",\"TFC\":0,"
			 "\"SDT\":[0,0,0],\"EDT\":[0,0,0]}");
		zsock_send(sock, resp, strlen(resp), 0);
		return;
	}

	ret = sdlog_do_get_oldest_date(&oldest);
	if (ret != 0) {
		LOG_ERR("MTP07: sdlog_do_get_oldest_date failed (%d)", ret);
		send_nack(sock, SDLOG_MTP_LOG_FILES_INFO_REQ);
		return;
	}

	ret = sdlog_do_get_newest_date(&newest);
	if (ret != 0) {
		LOG_ERR("MTP07: sdlog_do_get_newest_date failed (%d)", ret);
		send_nack(sock, SDLOG_MTP_LOG_FILES_INFO_REQ);
		return;
	}

	snprintf(resp, sizeof(resp),
		 "{\"MTP\":\"08\",\"TFC\":%u,"
		 "\"SDT\":[%u,%u,%u],"
		 "\"EDT\":[%u,%u,%u]}",
		 (unsigned int)total,
		 (unsigned int)oldest.day,   (unsigned int)oldest.month,
		 (unsigned int)oldest.year,
		 (unsigned int)newest.day,   (unsigned int)newest.month,
		 (unsigned int)newest.year);

	zsock_send(sock, resp, strlen(resp), 0);
	LOG_INF("MTP08 sent: TFC=%u SDT=[%u,%u,%u] EDT=[%u,%u,%u]",
		(unsigned int)total,
		oldest.day, oldest.month, oldest.year,
		newest.day, newest.month, newest.year);
}

/* ------------------------------------------------------------------ */
/* MTP 09 handler - Log Read Request                                   */
/* {"MTP":"09","LFD":[DD,MM,YYYY],"LTD":[DD,MM,YYYY]}                 */
/*                                                                      */
/* Full response sequence (per spec):                                  */
/*                                                                      */
/*   MTP 10  {"MTP":"10","RFC":N}           always sent                */
/*                                                                      */
/*   If N == 0:  workflow ends here.                                   */
/*                                                                      */
/*   If TFTP server unreachable:                                        */
/*     MTP 11  {"MTP":"11","TSA":1}                                    */
/*     MTP 12  {"MTP":"12","STS":1,"FNC":N,"FNO":1}                   */
/*                                                                      */
/*   If transfers proceed:                                              */
/*     MTP 11  {"MTP":"11","TSA":0}        ← ONCE before any transfer  */
/*     [TFTP file 1] … [TFTP file N]       ← stop on first failure    */
/*     MTP 12  {"MTP":"12","STS":0,"FNC":N,"FNO":0}   all success     */
/*     MTP 12  {"MTP":"12","STS":1,"FNC":N,"FNO":k}   file k failed   */
/* ------------------------------------------------------------------ */
/*
 * Returns  0  session may continue.
 * Returns -1  session must be torn down (MTP error response already sent,
 *             TCP client must be disconnected).
 *
 * Every error path that sends MTP 11 TSA=1 or MTP 12 STS=1 returns -1
 * so the caller (dispatch_packet → run_session) closes the connection.
 * Only the zero-file case (MTP 10 RFC=0) and full success return 0,
 * because in those cases the protocol exchange completed cleanly.
 */
static int handle_log_read_req(int sock, const char *frame, size_t flen)
{
	struct sdlog_proto_date lfd = {0}, ltd = {0};
	struct sdlog_date       start_date, end_date;
	uint32_t file_count = 0;
	size_t   found      = 0;
	int      ret;

	/* ── Parse From/To dates ── */
	if (json_get_date_array(frame, flen, "LFD", &lfd) != 0 ||
	    json_get_date_array(frame, flen, "LTD", &ltd) != 0) {
		LOG_WRN("MTP09: missing or malformed LFD/LTD");
		send_nack(sock, SDLOG_MTP_LOG_READ_REQ);
		return -1;
	}

	start_date.day   = lfd.day;   start_date.month = lfd.month;
	start_date.year  = lfd.year;
	end_date.day     = ltd.day;   end_date.month   = ltd.month;
	end_date.year    = ltd.year;

	/* ── Count matching files ── */
	ret = sdlog_do_get_file_count_in_range(&start_date, &end_date,
					       &file_count);
	if (ret != 0) {
		LOG_ERR("MTP09: file_count_in_range failed (%d)", ret);
		send_nack(sock, SDLOG_MTP_LOG_READ_REQ);
		return -1;
	}

	/* ── MTP 10 - always sent ── */
	{
		char buf[64];

		snprintf(buf, sizeof(buf),
			 "{\"MTP\":\"10\",\"RFC\":%u}",
			 (unsigned int)file_count);
		zsock_send(sock, buf, strlen(buf), 0);
		LOG_INF("MTP10 sent: RFC=%u", (unsigned int)file_count);
	}

	if (file_count == 0) {
		/* No files in range - sequence ends cleanly after MTP 10. */
		return 0;
	}

	/* ── List the files in the requested date range ── */
	size_t cap = MIN((size_t)file_count, (size_t)SDLOG_TCP_MAX_LIST_FILES);

	ret = sdlog_do_list_files_in_range(&start_date, &end_date,
					   s_file_info, cap, &found);
	if (ret != 0 || found == 0) {
		/*
		 * Cannot enumerate files - treat as server/list unavailable.
		 * MTP 11 TSA=1, MTP 12 STS=1 FNO=1 (first file failed).
		 * Disconnect after: file handling error.
		 */
		LOG_ERR("MTP09: list_files_in_range failed (ret=%d found=%zu)",
			ret, found);
		send_mtp11(sock, SDLOG_TSA_NOT_AVAILABLE);
		send_mtp12(sock, SDLOG_RESULT_FAIL,
			   file_count, 1);
		return -1;
	}

	/*
	 * Snapshot file names before releasing sdlog.lock so that the
	 * TFTP loop (which runs outside the lock) can safely iterate them.
	 */
	for (size_t i = 0; i < found; i++) {
		strncpy(s_pending_names[i], s_file_info[i].name,
			SDLOG_FNAME_MAX - 1);
		s_pending_names[i][SDLOG_FNAME_MAX - 1] = '\0';
	}
	size_t total_found = found;

	/*
	 * Release sdlog.lock before any blocking network operation.
	 * sdlog_tftp_transfer_file() uses SDLOG_File_* (public API)
	 * which re-acquires the lock internally for the brief FS calls.
	 */
	k_mutex_unlock(&sdlog.lock);

	/*
	 * Probe by attempting the first file transfer.  If -EHOSTUNREACH
	 * is returned the server is unreachable: send MTP 11 TSA=1 then
	 * MTP 12 STS=1, and disconnect.  Any other error is a file-level
	 * failure.
	 */
	ret = sdlog_tftp_transfer_file(s_pending_names[0], 1,
				       (uint32_t)total_found);

	if (ret == -EHOSTUNREACH) {
		/*
		 * TFTP server not available.
		 * MTP 11 TSA=1 → MTP 12 STS=1 FNO=1.
		 * Disconnect after: TFTP server unreachable.
		 */
		LOG_ERR("MTP09: TFTP server unreachable - informing client "
			"and disconnecting");
		send_mtp11(sock, SDLOG_TSA_NOT_AVAILABLE);
		send_mtp12(sock, SDLOG_RESULT_FAIL,
			   (uint32_t)total_found, 1);
		k_mutex_lock(&sdlog.lock, K_FOREVER);
		return -1;
	}

	/*
	 * Server is reachable (or first file had a non-network error).
	 * Send MTP 11 TSA=0 now - ONCE for the whole batch.
	 */
	send_mtp11(sock, SDLOG_TSA_AVAILABLE);

	if (ret != 0) {
		/*
		 * First file failed for a non-server reason (e.g. empty file,
		 * read error).  MTP 12 STS=1 FNO=1.
		 * Disconnect after: file transfer failure.
		 */
		LOG_ERR("MTP09: first file transfer failed (%d) - "
			"informing client and disconnecting", ret);
		send_mtp12(sock, SDLOG_RESULT_FAIL,
			   (uint32_t)total_found, 1);
		k_mutex_lock(&sdlog.lock, K_FOREVER);
		return -1;
	}

	/*
	 * First file succeeded.  Continue with the remaining files.
	 * Stop at the first failure.
	 */
	uint32_t failed_fno = 0;

	for (size_t i = 1; i < total_found; i++) {
		uint32_t fno = (uint32_t)(i + 1); /* 1-based */

		ret = sdlog_tftp_transfer_file(s_pending_names[i], fno,
					       (uint32_t)total_found);
		if (ret != 0) {
			failed_fno = fno;
			LOG_ERR("MTP09: file %u/%zu failed (%d) - stopping",
				(unsigned int)fno,
				total_found, ret);
			break;
		}
	}

	/* ── MTP 12 - sent ONCE, after the batch completes or fails ── */
	if (failed_fno == 0) {
		/*
		 * All files transferred successfully.
		 * STS=0, FNC=total, FNO=0.
		 */
		send_mtp12(sock, SDLOG_RESULT_SUCCESS,
			   (uint32_t)total_found, 0);
	} else {
		/*
		 * At least one file failed. MTP 12 STS=1 FNO=failing file.
		 * Disconnect after: mid-batch transfer failure.
		 */
		send_mtp12(sock, SDLOG_RESULT_FAIL,
			   (uint32_t)total_found, failed_fno);
	}

	/* Re-acquire lock to balance the unlock above. */
	k_mutex_lock(&sdlog.lock, K_FOREVER);

	/*
	 * Return -1 on any batch failure so run_session drops the client.
	 * Return 0 only when every file in the batch succeeded.
	 */
	return (failed_fno == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Security Sequence handshake (MTP 02)                               */
/* ------------------------------------------------------------------ */

#define HANDSHAKE_BUF_MAX 256

static int perform_handshake(int sock)
{
	static char buf[HANDSHAKE_BUF_MAX];
	size_t  got        = 0;
	int64_t deadline   = k_uptime_get() +
			     (int64_t)CONFIG_SDLOG_TCP_HANDSHAKE_TIMEOUT_MS;
	size_t  frame_start = 0;
	int     frame_len   = -1;

	while (frame_len < 0) {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0) {
			LOG_WRN("Handshake timed out");
			return -ETIMEDOUT;
		}
		if (got >= sizeof(buf)) {
			LOG_WRN("Handshake frame too large");
			return -EMSGSIZE;
		}

		struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
		int pret = zsock_poll(&pfd, 1, (int)remaining);

		if (pret == 0) {
			LOG_WRN("Handshake timed out");
			return -ETIMEDOUT;
		}
		if (pret < 0 ||
		    (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL))) {
			LOG_WRN("Handshake: poll error");
			return -EIO;
		}

		ssize_t n = zsock_recv(sock, buf + got, sizeof(buf) - got, 0);

		if (n == 0)  { return -ECONNRESET; }
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
			return -EIO;
		}
		got += (size_t)n;

		frame_len = frame_scan(buf, got, &frame_start);
		if (frame_len == -2) {
			LOG_WRN("Handshake: malformed JSON");
			return -EINVAL;
		}
	}

	const char *f    = buf + frame_start;
	size_t      flen = (size_t)frame_len;
	char        mtp[8], seq[128];

	if (json_get_str(f, flen, "MTP", mtp, sizeof(mtp)) != 0 ||
	    strcmp(mtp, SDLOG_MTP_SECURITY_SEQUENCE) != 0) {
		LOG_WRN("Handshake: expected MTP 02, got '%s'", mtp);
		return -EINVAL;
	}
	if (json_get_str(f, flen, "SEQ", seq, sizeof(seq)) != 0) {
		LOG_WRN("Handshake: SEQ field missing");
		return -EINVAL;
	}
	if (strcmp(seq, CONFIG_SDLOG_TCP_HANDSHAKE_SEQ) != 0) {
		LOG_WRN("Handshake: SEQ mismatch");
		return -EINVAL;
	}

	/* ACK: {"MTP":"01","MSG":0,"RTP":"02"} */
	send_ack(sock, SDLOG_MTP_SECURITY_SEQUENCE);
	LOG_INF("Handshake accepted");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Packet dispatch                                                     */
/* ------------------------------------------------------------------ */

/*
 * Returns  0  session may continue.
 * Returns -1  session must be torn down (already signalled to client).
 */
static int dispatch_packet(int sock, const char *frame, size_t flen)
{
	char mtp[8];

	if (json_get_str(frame, flen, "MTP", mtp, sizeof(mtp)) != 0) {
		LOG_WRN("Packet missing MTP field - ignored");
		return 0;
	}

	if (strcmp(mtp, SDLOG_MTP_LOG_FILES_INFO_REQ) == 0) {
		/* MTP 07 - no fatal outcome; always returns cleanly. */
		k_mutex_lock(&sdlog.lock, K_FOREVER);
		handle_log_files_info_req(sock, frame, flen);
		k_mutex_unlock(&sdlog.lock);
		return 0;

	} else if (strcmp(mtp, SDLOG_MTP_LOG_READ_REQ) == 0) {
		/*
		 * MTP 09.
		 * handle_log_read_req() acquires the lock at entry,
		 * drops it before TFTP, and re-acquires before returning,
		 * so the unlock here is always balanced.
		 * Returns -1 when the session must be closed.
		 */
		k_mutex_lock(&sdlog.lock, K_FOREVER);
		int rc = handle_log_read_req(sock, frame, flen);
		k_mutex_unlock(&sdlog.lock);
		return rc;

	} else {
		LOG_WRN("Unhandled MTP '%s' - ignored", mtp);
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* Data-phase receive loop                                             */
/* ------------------------------------------------------------------ */

static void run_session(int sock)
{
	static char rxbuf[TCP_RX_BUF_SIZE];
	size_t used = 0;

	while (true) {
		ssize_t n = zsock_recv(sock, rxbuf + used,
				       sizeof(rxbuf) - used, 0);

		if (n == 0) {
			LOG_INF("TCP client disconnected");
			return;
		}
		if (n < 0) {
			LOG_WRN("recv() error: %d", errno);
			return;
		}
		used += (size_t)n;

		while (true) {
			size_t start;
			int f = frame_scan(rxbuf, used, &start);

			if (f == -1) { break; }
			if (f == -2) {
				LOG_WRN("Malformed JSON stream - dropping client");
				return;
			}

			if (dispatch_packet(sock, &rxbuf[start],
					    (size_t)f) != 0) {
				LOG_INF("TCP: closing client after "
					"session error");
				return;
			}

			size_t consumed = start + (size_t)f;

			memmove(rxbuf, rxbuf + consumed, used - consumed);
			used -= consumed;
		}

		if (used == sizeof(rxbuf)) {
			LOG_WRN("Frame > TCP_RX_BUF_SIZE - dropping client");
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* TCP server socket + accept loop                                     */
/* ------------------------------------------------------------------ */

static int open_server_socket(void)
{
	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port        = htons(CONFIG_SDLOG_TCP_PORT),
	};
	int sock;
	int reuse = 1;

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return -errno;
	}

	zsock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	if (zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	if (zsock_listen(sock, 1) < 0) {
		LOG_ERR("listen() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	LOG_INF("SDLOG TCP server listening on port %u", CONFIG_SDLOG_TCP_PORT);
	return sock;
}

static void server_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct in_addr allowed_addr;

	if (zsock_inet_pton(AF_INET, CONFIG_SDLOG_TCP_ALLOWED_CLIENT_IP,
			    &allowed_addr) != 1) {
		LOG_ERR("SDLOG TCP: invalid allowed_client_ip '%s'",
			CONFIG_SDLOG_TCP_ALLOWED_CLIENT_IP);
		return;
	}

	s_srv.server_sock = open_server_socket();
	if (s_srv.server_sock < 0) {
		LOG_ERR("SDLOG TCP: failed to open server socket");
		return;
	}

	while (s_srv.running) {
		struct sockaddr_in client_addr;
		socklen_t          addrlen = sizeof(client_addr);

		int client = zsock_accept(s_srv.server_sock,
					  (struct sockaddr *)&client_addr,
					  &addrlen);
		if (client < 0) {
			if (!s_srv.running) { break; }
			LOG_WRN("accept() failed: %d", errno);
			continue;
		}

		/*
		 * IP allow-list: only the configured address may proceed.
		 * The application is never notified of this rejection.
		 */
		if (client_addr.sin_addr.s_addr != allowed_addr.s_addr) {
			char ip_str[NET_IPV4_ADDR_LEN];

			zsock_inet_ntop(AF_INET, &client_addr.sin_addr,
					ip_str, sizeof(ip_str));
			LOG_WRN("SDLOG TCP: rejected unauthorized client %s",
				ip_str);
			zsock_close(client);
			continue;
		}

		s_srv.client_sock = client;
		LOG_INF("SDLOG TCP: client accepted");

		/* The application is never notified of this connection. */
		if (perform_handshake(client) != 0) {
			LOG_WRN("SDLOG TCP: handshake failed - disconnecting");
			zsock_close(client);
			s_srv.client_sock = -1;
			continue;
		}

		run_session(client);

		zsock_close(client);
		s_srv.client_sock = -1;
		LOG_INF("SDLOG TCP: client disconnected - back to listening");
	}

	zsock_close(s_srv.server_sock);
	s_srv.server_sock = -1;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

int sdlog_tcp_server_start(void)
{
	if (s_srv.running) {
		return -EALREADY;
	}

	s_srv.server_sock = -1;
	s_srv.client_sock = -1;
	s_srv.running     = true;

	k_thread_create(&s_srv.thread, s_srv.stack,
			K_KERNEL_STACK_SIZEOF(s_srv.stack),
			server_thread_fn, NULL, NULL, NULL,
			CONFIG_SDLOG_TCP_THREAD_PRIORITY,
			0, K_NO_WAIT);
	k_thread_name_set(&s_srv.thread, "sdlog_tcp");
	return 0;
}

int sdlog_tcp_server_stop(void)
{
	if (!s_srv.running) {
		return -EALREADY;
	}

	s_srv.running = false;

	if (s_srv.client_sock >= 0) {
		zsock_shutdown(s_srv.client_sock, ZSOCK_SHUT_RDWR);
	}
	if (s_srv.server_sock >= 0) {
		zsock_shutdown(s_srv.server_sock, ZSOCK_SHUT_RDWR);
	}

	k_thread_join(&s_srv.thread, K_MSEC(3000));
	return 0;
}

int sdlog_tcp_send(const char *json)
{
	if (s_srv.client_sock < 0) {
		return -ENOTCONN;
	}

	size_t len = strlen(json);
	int    ret = zsock_send(s_srv.client_sock, json, len, 0);

	return (ret < 0) ? -errno : ret;
}

#endif /* CONFIG_SDLOG_TCP_SERVER */