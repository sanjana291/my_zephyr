/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "sdlog_internal.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

/**
 * @brief Pick the file with the smallest <File Number> currently on
 * disk, excluding @p exclude_name (used to exclude the file that was
 * just deleted, or NULL).
 */
static int find_oldest_file(const char *exclude_name, char *out_name, size_t out_size)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;
	int best_idx = -1;

	sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);

	for (size_t i = 0; i < found; i++) {
		if (exclude_name != NULL && strcmp(names[i], exclude_name) == 0) {
			continue;
		}
		if (best_idx < 0 || nums[i] < nums[best_idx]) {
			best_idx = (int)i;
		}
	}

	if (best_idx < 0) {
		return -ENOENT;
	}

	strncpy(out_name, names[best_idx], out_size - 1);
	out_name[out_size - 1] = '\0';
	return 0;
}

/**
 * @brief Pick the oldest file that is actually safe to delete right
 * now: never the active write file (LOG-CLR-06), and - beyond what
 * the attached requirements specify - never a file with an open
 * random-access handle (an in-progress TFTP transfer), since
 * unlinking a file out from under an open fs_read() has much less
 * predictable behavior on FAT than the Modem-read-cursor case the
 * project already decided "space always wins" for. If the oldest
 * candidate isn't deletable, the next-oldest is tried instead of
 * giving up immediately.
 */
static int find_deletable_oldest_file(char *out_name, size_t out_size)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;

	sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);

	for (;;) {
		int best_idx = -1;

		for (size_t i = 0; i < found; i++) {
			if (names[i][0] == '\0') {
				continue; /* already ruled out below */
			}
			if (best_idx < 0 || nums[i] < nums[best_idx]) {
				best_idx = (int)i;
			}
		}

		if (best_idx < 0) {
			return -ENOENT; /* nothing deletable right now */
		}

		if (strcmp(names[best_idx], sdlog.cfg.last_written_file) == 0 ||
		    sdlog_is_file_open_for_random_access(names[best_idx])) {
			/* Rule this one out and look for the next
			 * candidate rather than giving up immediately.
			 */
			names[best_idx][0] = '\0';
			continue;
		}

		strncpy(out_name, names[best_idx], out_size - 1);
		out_name[out_size - 1] = '\0';
		return 0;
	}
}

static int sdlog_run_cleanup_locked(void)
{
	uint64_t free_bytes = 0, total_bytes = 0;
	int ret;

	ret = sdlog_get_free_space(&free_bytes, &total_bytes);
	if (ret != 0) {
		return ret;
	}

	if (total_bytes == 0) {
		return -EIO;
	}

	unsigned int used_percent =
		(unsigned int)(((total_bytes - free_bytes) * 100U) / total_bytes);

	/* LOG-CLR-01/02: check free space before write, clean up at
	 * the configured usage threshold.
	 */
	if (used_percent < CONFIG_SDLOG_CLEANUP_THRESHOLD_PERCENT) {
		return 0;
	}

	LOG_WRN("SD card usage at %u%% (>= %d%% threshold) - running cleanup",
		used_percent, CONFIG_SDLOG_CLEANUP_THRESHOLD_PERCENT);

	if (sdlog.cfg.oldest_file[0] == '\0') {
		LOG_ERR("Cleanup requested but no oldest file is tracked");
		return -ENOSPC;
	}

	/* LOG-CLR-03/04/06: identify and delete the oldest file that
	 * is actually safe to delete (never the active write file,
	 * never one with an open random-access/TFTP handle).
	 */
	char victim[SDLOG_FNAME_MAX];
	char path[SDLOG_FNAME_MAX + 8];

	ret = find_deletable_oldest_file(victim, sizeof(victim));
	if (ret != 0) {
		LOG_ERR("Card is full but no deletable file was found (the "
			"active write file and/or all others are currently "
			"open for random access)");
		return -ENOSPC;
	}

	sdlog_build_full_path(path, sizeof(path), victim);

	ret = fs_unlink(path);
	if (ret != 0) {
		LOG_ERR("Failed to delete oldest file %s (%d)", path, ret);
		return -EIO;
	}

	LOG_WRN("Deleted oldest log file %s to reclaim space", victim);

	/* If the deleted file was still the Modem's read cursor
	 * position, its unread data is now gone; move the read
	 * cursor forward so subsequent reads don't fail against a
	 * missing file.
	 */
	bool was_pending_read = (strcmp(sdlog.cfg.modem_read_file, victim) == 0);

	char new_oldest[SDLOG_FNAME_MAX] = {0};

	ret = find_oldest_file(victim, new_oldest, sizeof(new_oldest));
	if (ret != 0) {
		/* Only the active file remains. */
		strncpy(new_oldest, sdlog.cfg.last_written_file, sizeof(new_oldest) - 1);
	}

	strncpy(sdlog.cfg.oldest_file, new_oldest, SDLOG_FNAME_MAX - 1);
	sdlog.cfg.oldest_file[SDLOG_FNAME_MAX - 1] = '\0';

	if (was_pending_read) {
		LOG_WRN("Deleted file held unread entries; advancing Modem's "
			"read cursor to %s", new_oldest);
		strncpy(sdlog.cfg.modem_read_file, new_oldest, SDLOG_FNAME_MAX - 1);
		sdlog.cfg.modem_read_file[SDLOG_FNAME_MAX - 1] = '\0';
		sdlog.cfg.modem_read_entry = 0;
	}

	/* LOG-CLR-05/08: persist updated oldest-file bookkeeping. */
	return sdlog_config_save();
}

int sdlog_cleanup_check_and_run(void)
{
	return sdlog_run_cleanup_locked();
}
