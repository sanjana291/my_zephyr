/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdlog_tftp.c
 * @brief TFTP client integrated inside the sdlog module.
 *
 * Compiled only when CONFIG_SDLOG_TCP_SERVER is enabled.
 *
 * Responsibilities (and ONLY these - see sdlog_tcp.h):
 *   1. Open the requested log file via the public random-access API
 *      (SDLOG_File_open/size/read/close).
 *   2. Read the whole file into a static buffer.
 *   3. Push it to the configured TFTP server via Zephyr's net/tftp.h
 *      tftp_put().
 *   4. Return a plain status code and NOTHING ELSE. This function
 *      never sends an MTP packet - sdlog_tcp.c (specifically
 *      handle_log_read_req()) owns all MTP 11 / MTP 12 messaging,
 *      since it alone knows batch-wide state: which file index this
 *      is, whether this call is being used as the "is the TFTP
 *      server reachable at all" probe (it uses file #1 for that),
 *      and the total file count for the batch.
 *
 * Error mapping - this is the contract sdlog_tcp.c's
 * handle_log_read_req() is written against, so it must be preserved
 * exactly:
 *
 *   -EHOSTUNREACH  The TFTP server itself could not be reached -
 *                  either its address could not be resolved, or
 *                  tftp_put() itself returned an error. The caller
 *                  branches on exactly this value (checked only for
 *                  the first file in a batch) to decide whether to
 *                  send MTP 11 TSA=1 (server not available) instead
 *                  of TSA=0.
 *   -ENODATA       The file exists but is currently empty (e.g.
 *                  today's active file with no entries written yet).
 *                  Not a network problem - reported as an ordinary
 *                  file-level failure (MTP 12 STS=1), not TSA=1.
 *   -EIO           A transfer that started but didn't fully complete
 *                  (tftp_put() returned a byte count not matching
 *                  the file size, or our own read from the SD card
 *                  was short). Bytes clearly moved, so this is
 *                  deliberately NOT reported as "unreachable".
 *   other negative errno  Whatever SDLOG_File_open()/SDLOG_File_size()/
 *                  SDLOG_File_read() returned - a local file-access
 *                  problem, not a network one.
 *
 * Must NOT be called while holding sdlog.lock - and per
 * handle_log_read_req(), it never is: the caller explicitly unlocks
 * before calling this, since SDLOG_File_open/size/read/close each
 * acquire sdlog.lock internally for the brief duration of their own
 * call, and a TFTP transfer can block for a while.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tftp.h>
#include <zephyr/logging/log.h>

#include <logging_module/logging_module.h>
#include "sdlog_internal.h"
#include "sdlog_tcp.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

#if defined(CONFIG_SDLOG_TCP_SERVER)

/* Static rather than stack-allocated: CONFIG_SDLOG_TFTP_MAX_FILE_SIZE
 * can be tens of KB (a full log file), too large for this thread's
 * stack.
 */
static uint8_t s_file_buf[CONFIG_SDLOG_TFTP_MAX_FILE_SIZE];

static void tftp_evt_cb(const struct tftp_evt *evt)
{
	if (evt->type == TFTP_EVT_ERROR) {
		LOG_ERR("SDLOG TFTP: server reported error %d: %s",
			evt->param.error.code,
			evt->param.error.msg ? evt->param.error.msg : "(none)");
	}
}

int sdlog_tftp_transfer_file(const char *filename,
			      uint32_t file_number,
			      uint32_t total_files)
{
	sdlog_file_handle_t handle;
	size_t file_size = 0;
	size_t read_len = 0;
	int ret;

	LOG_INF("SDLOG TFTP: starting file %u/%u: '%s'",
		file_number, total_files, filename);

	/* 1. Open. */
	ret = SDLOG_File_open(filename, &handle);
	if (ret != 0) {
		LOG_ERR("SDLOG TFTP: SDLOG_File_open('%s') failed (%d)",
			filename, ret);
		return ret;
	}

	/* 2. Size. */
	ret = SDLOG_File_size(handle, &file_size);
	if (ret != 0) {
		LOG_ERR("SDLOG TFTP: SDLOG_File_size('%s') failed (%d)",
			filename, ret);
		SDLOG_File_close(handle);
		return ret;
	}

	if (file_size == 0) {
		LOG_WRN("SDLOG TFTP: '%s' is empty - nothing to send", filename);
		SDLOG_File_close(handle);
		return -ENODATA;
	}

	if (file_size > sizeof(s_file_buf)) {
		LOG_ERR("SDLOG TFTP: '%s' is %zu bytes, exceeds "
			"CONFIG_SDLOG_TFTP_MAX_FILE_SIZE (%zu)",
			filename, file_size, sizeof(s_file_buf));
		SDLOG_File_close(handle);
		return -EFBIG;
	}

	/* 3. Read the whole file into the static buffer. */
	ret = SDLOG_File_read(handle, 0, s_file_buf, file_size, &read_len);
	SDLOG_File_close(handle);

	if (ret != 0) {
		LOG_ERR("SDLOG TFTP: SDLOG_File_read('%s') failed (%d)",
			filename, ret);
		return ret;
	}
	if (read_len != file_size) {
		LOG_ERR("SDLOG TFTP: short read on '%s' (%zu/%zu bytes)",
			filename, read_len, file_size);
		return -EIO;
	}

	/* 4. Resolve the TFTP server and push the file. */
	struct tftpc client = { .callback = tftp_evt_cb };
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM };
	struct addrinfo *res = NULL;

	ret = getaddrinfo(CONFIG_SDLOG_TFTP_SERVER_IP,
			   CONFIG_SDLOG_TFTP_SERVER_PORT_STR,
			   &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_ERR("SDLOG TFTP: could not resolve TFTP server '%s:%s' (%d)",
			CONFIG_SDLOG_TFTP_SERVER_IP,
			CONFIG_SDLOG_TFTP_SERVER_PORT_STR, ret);
		return -EHOSTUNREACH;
	}

	memcpy(&client.server, res->ai_addr, sizeof(client.server));
	freeaddrinfo(res);

	LOG_INF("SDLOG TFTP: sending '%s' (%zu bytes) to %s:%s",
		filename, read_len, CONFIG_SDLOG_TFTP_SERVER_IP,
		CONFIG_SDLOG_TFTP_SERVER_PORT_STR);

	ret = tftp_put(&client, filename, "octet",
		       (const char *)s_file_buf, read_len);

	if (ret < 0) {
		/* Per sdlog_tcp.h's contract: any tftp_put() error is
		 * treated the same as a DNS/address-resolution
		 * failure above - "the server could not be reached" -
		 * so handle_log_read_req() can react to it uniformly
		 * (TSA=1) when this is the first file in the batch.
		 */
		LOG_ERR("SDLOG TFTP: tftp_put('%s') failed (%d)", filename, ret);
		return -EHOSTUNREACH;
	}

	if ((size_t)ret != read_len) {
		/* Bytes clearly moved, so this is a distinct failure
		 * mode from "unreachable" - reported as a plain I/O
		 * error instead of -EHOSTUNREACH.
		 */
		LOG_ERR("SDLOG TFTP: incomplete transfer of '%s' (%d/%zu bytes)",
			filename, ret, read_len);
		return -EIO;
	}

	LOG_INF("SDLOG TFTP: '%s' sent successfully (%zu bytes)",
		filename, read_len);
	return 0;
}

#endif /* CONFIG_SDLOG_TCP_SERVER */