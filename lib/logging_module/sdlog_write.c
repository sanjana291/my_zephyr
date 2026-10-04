/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "sdlog_internal.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

int sdlog_create_new_active_file(void)
{
	char timestamp[SDLOG_TIMESTAMP_LEN + 1];
	char fname[SDLOG_FNAME_MAX];
	char path[SDLOG_FNAME_MAX + 8];
	struct fs_file_t file;
	uint32_t file_number;
	int ret;

	ret = sdlog_rtc_timestamp(timestamp, sizeof(timestamp));
	if (ret != 0) {
		return ret;
	}

	file_number = sdlog_alloc_file_number();
	sdlog_build_log_filename(fname, sizeof(fname), file_number, timestamp);
	sdlog_build_full_path(path, sizeof(path), fname);

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		LOG_ERR("Failed to create log file %s (%d)", path, ret);
		return -EIO;
	}
	fs_close(&file);

	LOG_INF("Created new active log file: %s", fname);

	/* LOG-INIT-09 / LOG-WR-08: update config only after the file
	 * was actually created on disk.
	 */
	strncpy(sdlog.cfg.last_written_file, fname, SDLOG_FNAME_MAX - 1);
	sdlog.cfg.last_written_file[SDLOG_FNAME_MAX - 1] = '\0';
	sdlog.cfg.last_written_entry = 0;
	sdlog.cfg.file_entry_count = 0;

	if (sdlog.cfg.oldest_file[0] == '\0') {
		strncpy(sdlog.cfg.oldest_file, fname, SDLOG_FNAME_MAX - 1);
		sdlog.cfg.oldest_file[SDLOG_FNAME_MAX - 1] = '\0';
	}
	if (sdlog.cfg.modem_read_file[0] == '\0') {
		strncpy(sdlog.cfg.modem_read_file, fname, SDLOG_FNAME_MAX - 1);
		sdlog.cfg.modem_read_file[SDLOG_FNAME_MAX - 1] = '\0';
	}

	return sdlog_config_save();
}

int sdlog_check_daily_rotation(void)
{
	char file_date[9];
	char now_date[9];
	int ret;

	if (sdlog.cfg.last_written_file[0] == '\0') {
		/* No active file yet at all - not this function's
		 * job; the caller creates the first file separately.
		 */
		return 0;
	}

	if (sdlog_extract_file_date(sdlog.cfg.last_written_file, file_date) != 0) {
		/* Can't parse - don't force a rotation on uncertainty. */
		return 0;
	}

	ret = sdlog_rtc_date(now_date, sizeof(now_date));
	if (ret != 0) {
		/* RTC unavailable - we cannot tell whether a new day has
		 * begun, and any file created right now would carry a bad
		 * timestamp. Refuse to write rather than silently logging
		 * data under a possibly-wrong date; propagate the error so
		 * the caller (sdlog_do_write) aborts this write attempt.
		 */
		LOG_ERR("sdlog_check_daily_rotation: RTC unavailable (%d) - "
			"refusing to write", ret);
		return ret;
	}

	if (strcmp(file_date, now_date) == 0) {
		return 0; /* still the same day */
	}

	LOG_INF("Date changed since %s was created (now %s) - starting a "
		"new log file for today", sdlog.cfg.last_written_file, now_date);

	return sdlog_create_new_active_file();
}

int sdlog_do_write(const uint8_t *data, size_t len)
{
	char path[SDLOG_FNAME_MAX + 8];
	struct fs_file_t file;
	int ret;

	/* LOG-WR-01 / LOG-CLR-01: free space check (and cleanup if
	 * needed) before every write.
	 */
	ret = sdlog_cleanup_check_and_run();
	if (ret != 0) {
		LOG_ERR("Pre-write cleanup check failed (%d)", ret);
		return ret;
	}

	uint64_t free_bytes = 0;

	ret = sdlog_get_free_space(&free_bytes, NULL);
	if (ret != 0) {
		return ret;
	}
	/* +1 for the trailing newline written after every entry. */
	if (free_bytes < (len + 1)) {
		LOG_ERR("Insufficient free space on SD card (%llu bytes free)",
			(unsigned long long)free_bytes);
		return -ENOSPC;
	}

	if (sdlog.cfg.last_written_file[0] == '\0') {
		/* First-ever write: no active file yet. */
		ret = sdlog_create_new_active_file();
		if (ret != 0) {
			return ret;
		}
	} else if (sdlog.cfg.file_entry_count >= CONFIG_SDLOG_MAX_ENTRIES_PER_FILE) {
		ret = sdlog_create_new_active_file();          /* size cap hit - now checked lazily, before this write */
	}else {
		/* Daily log file creation (item 1): also checked here
		 * (not just at boot in sdlog_init()) so a device that
		 * stays powered on across midnight still rolls over
		 * to a new day's file without needing a reboot.
		 */
		ret = sdlog_check_daily_rotation();
		if (ret != 0) {
			return ret;
		}
	}

	/* LOG-WR-02: open the last written log file for appending. */
	sdlog_build_full_path(path, sizeof(path), sdlog.cfg.last_written_file);

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_WRITE | FS_O_APPEND);
	if (ret != 0) {
		LOG_ERR("Failed to open active log file %s for append (%d)",
			path, ret);
		return -EIO;
	}

	/* Store exactly what the caller gave us, followed by a single
	 * '\n' so each entry is one line/row in a plain-text .csv
	 * file. Any structure inside the line (columns, delimiters)
	 * is entirely up to the caller - this module just stores
	 * bytes and separates entries with newlines.
	 */
	ssize_t wr = fs_write(&file, data, len);

	if (wr == (ssize_t)len) {
		static const uint8_t newline = '\n';
		ssize_t wr_nl = fs_write(&file, &newline, 1);

		if (wr_nl != 1) {
			wr = -1; /* force the failure path below */
		}
	}

	/* LOG-WR-04: close the file to ensure data is committed. */
	fs_close(&file);

	if (wr != (ssize_t)len) {
		LOG_ERR("Log write incomplete/failed (%zd/%zu) on %s",
			wr, len, sdlog.cfg.last_written_file);
		/* LOG-CLR/config note: do not advance counters on a
		 * failed write - config must reflect only successful
		 * operations.
		 */
		return -EIO;
	}

	/* Only now, after a fully successful write, advance state. */
	sdlog.cfg.last_written_entry++;
	sdlog.cfg.file_entry_count++;

	ret = sdlog_config_save();
	if (ret != 0) {
		LOG_ERR("Failed to persist config after write (%d)", ret);
		return ret;
	}

	/* LOG-WR-06/07/08: rotate once the active file is full. */
	if (sdlog.cfg.file_entry_count > CONFIG_SDLOG_MAX_ENTRIES_PER_FILE) {
		ret = sdlog_create_new_active_file();
		if (ret != 0) {
			LOG_ERR("Failed to rotate to a new log file (%d)", ret);
			return ret;
		}
	}

	return 0;
}
