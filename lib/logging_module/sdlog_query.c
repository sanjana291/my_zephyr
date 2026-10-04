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

/**
 * @brief Parse an 8-digit "YYYYMMDD" string into a struct sdlog_date.
 */
static void date_from_str(const char *date8, struct sdlog_date *out)
{
	out->year = (uint16_t)((date8[0] - '0') * 1000 + (date8[1] - '0') * 100 +
				(date8[2] - '0') * 10 + (date8[3] - '0'));
	out->month = (uint8_t)((date8[4] - '0') * 10 + (date8[5] - '0'));
	out->day = (uint8_t)((date8[6] - '0') * 10 + (date8[7] - '0'));
}

/**
 * @brief Turn a date into a single comparable integer (YYYYMMDD as
 * a number), so range checks are a plain <= / >= comparison.
 */
static uint32_t date_key(const struct sdlog_date *d)
{
	return (uint32_t)d->year * 10000u + (uint32_t)d->month * 100u + (uint32_t)d->day;
}

int sdlog_do_get_file_count(uint32_t *count)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	size_t found = 0;
	int ret;

	ret = sdlog_scan_log_files(nums, NULL, CONFIG_SDLOG_MAX_FILES_SCAN, &found);
	if (ret != 0) {
		return ret;
	}

	*count = (uint32_t)found;
	return 0;
}

int sdlog_do_get_oldest_date(struct sdlog_date *date)
{
	char date8[9];

	if (sdlog.cfg.oldest_file[0] == '\0') {
		return -ENODATA;
	}

	if (sdlog_extract_file_date(sdlog.cfg.oldest_file, date8) != 0) {
		return -EIO;
	}

	date_from_str(date8, date);
	return 0;
}

int sdlog_do_get_newest_date(struct sdlog_date *date)
{
	char date8[9];

	if (sdlog.cfg.last_written_file[0] == '\0') {
		return -ENODATA;
	}

	if (sdlog_extract_file_date(sdlog.cfg.last_written_file, date8) != 0) {
		return -EIO;
	}

	date_from_str(date8, date);
	return 0;
}

int sdlog_do_get_file_count_in_range(const struct sdlog_date *start,
				      const struct sdlog_date *end,
				      uint32_t *count)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;
	uint32_t start_key = date_key(start);
	uint32_t end_key = date_key(end);
	uint32_t matched = 0;
	int ret;

	ret = sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < found; i++) {
		char date8[9];

		if (sdlog_extract_file_date(names[i], date8) != 0) {
			continue;
		}

		struct sdlog_date d;

		date_from_str(date8, &d);
		uint32_t k = date_key(&d);

		if (k >= start_key && k <= end_key) {
			matched++;
		}
	}

	*count = matched;
	return 0;
}

int sdlog_do_list_files_in_range(const struct sdlog_date *start,
				  const struct sdlog_date *end,
				  struct sdlog_file_info *out,
				  size_t max_files, size_t *out_count)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;
	uint32_t start_key = date_key(start);
	uint32_t end_key = date_key(end);
	size_t written = 0;
	int ret;

	ret = sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < found; i++) {
		char date8[9];

		if (sdlog_extract_file_date(names[i], date8) != 0) {
			continue;
		}

		struct sdlog_date d;

		date_from_str(date8, &d);
		uint32_t k = date_key(&d);

		if (k < start_key || k > end_key) {
			continue;
		}

		if (written >= max_files) {
			LOG_WRN("sdlog_list_files_in_range: more matches than "
				"max_files (%zu); truncating", max_files);
			break;
		}

		strncpy(out[written].name, names[i], sizeof(out[written].name) - 1);
		out[written].name[sizeof(out[written].name) - 1] = '\0';
		out[written].date = d;

		char path[SDLOG_FNAME_MAX + 8];
		struct fs_dirent dirent;

		sdlog_build_full_path(path, sizeof(path), names[i]);
		if (fs_stat(path, &dirent) == 0) {
			out[written].size_bytes = (uint32_t)dirent.size;
		} else {
			out[written].size_bytes = 0;
		}

		written++;
	}

	*out_count = written;
	return 0;
}
