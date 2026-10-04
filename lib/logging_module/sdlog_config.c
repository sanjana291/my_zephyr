/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include "sdlog_internal.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

uint32_t sdlog_crc32(const struct sdlog_config_data *cfg)
{
	/* CRC covers every field except crc32 itself, which sits last
	 * in the struct.
	 */
	size_t len = offsetof(struct sdlog_config_data, crc32);

	return crc32_ieee((const uint8_t *)cfg, len);
}

/**
 * @brief Best-effort reconstruction of config state from disk contents.
 *
 * Used both when no config file exists yet (LOG-INIT-07) and when an
 * existing one fails CRC validation ("module should handle ... a
 * corrupted configuration file gracefully").  We can't recover exact
 * entry-position counters this way (that's the whole reason the
 * config file exists), so a rebuilt config always starts read/write
 * cursors fresh, but it correctly identifies which files exist so no
 * data is silently lost or orphaned.
 */
static void rebuild_config_from_disk(struct sdlog_config_data *cfg)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;

	memset(cfg, 0, sizeof(*cfg));
	cfg->magic = SDLOG_CONFIG_MAGIC;
	cfg->version = SDLOG_CONFIG_VERSION;

	sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);

	if (found == 0) {
		/* Nothing on disk yet; sdlog_init() will create the
		 * first active log file.
		 */
		cfg->next_file_number = 0;
		LOG_WRN("No existing config or log files found; starting fresh");
		return;
	}

	size_t oldest_idx = 0;
	size_t newest_idx = 0;

	for (size_t i = 1; i < found; i++) {
		if (nums[i] < nums[oldest_idx]) {
			oldest_idx = i;
		}
		if (nums[i] > nums[newest_idx]) {
			newest_idx = i;
		}
	}

	strncpy(cfg->oldest_file, names[oldest_idx], SDLOG_FNAME_MAX - 1);
	strncpy(cfg->last_written_file, names[newest_idx], SDLOG_FNAME_MAX - 1);
	strncpy(cfg->modem_read_file, names[oldest_idx], SDLOG_FNAME_MAX - 1);
	cfg->last_written_entry = 0; /* unknown - conservative restart */
	cfg->modem_read_entry = 0;
	cfg->file_entry_count = 0;   /* unknown - will be corrected on next rotation check */
	cfg->next_file_number = (nums[newest_idx] + 1) % (CONFIG_SDLOG_MAX_FILE_NUMBER + 1);

	LOG_WRN("Rebuilt config from %zu file(s) on disk; write/read cursors "
		"reset to file start (exact position could not be recovered)",
		found);
}

int sdlog_config_save(void)
{
	char path[SDLOG_FNAME_MAX + 8];
	struct fs_file_t file;
	int ret;

	sdlog_build_config_path(path, sizeof(path));

	sdlog.cfg.magic = SDLOG_CONFIG_MAGIC;
	sdlog.cfg.version = SDLOG_CONFIG_VERSION;
	sdlog.cfg.crc32 = sdlog_crc32(&sdlog.cfg);

	fs_file_t_init(&file);

	/* Truncate + rewrite in place. The config struct is small and
	 * fixed size, so this is a single-sector-scale write; a
	 * torn write on power loss is caught next boot via the CRC
	 * check and handled by rebuild_config_from_disk().
	 */
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		LOG_ERR("Failed to open config file for write (%d)", ret);
		return -EIO;
	}

	ret = fs_truncate(&file, 0);
	if (ret != 0) {
		LOG_WRN("fs_truncate on config file failed (%d)", ret);
		/* Not fatal - fall through and write anyway. */
	}

	ssize_t written = fs_write(&file, &sdlog.cfg, sizeof(sdlog.cfg));

	fs_close(&file);

	if (written != (ssize_t)sizeof(sdlog.cfg)) {
		LOG_ERR("Config file write incomplete (%zd/%zu)",
			written, sizeof(sdlog.cfg));
		return -EIO;
	}

	sdlog.cfg_valid = true;
	return 0;
}

int sdlog_config_load_or_create(void)
{
	char path[SDLOG_FNAME_MAX + 8];
	struct fs_file_t file;
	int ret;

	sdlog_build_config_path(path, sizeof(path));
	fs_file_t_init(&file);

	ret = fs_open(&file, path, FS_O_READ);
	if (ret == -ENOENT) {
		LOG_INF("No config file found, creating a new one (%s)", path);
		rebuild_config_from_disk(&sdlog.cfg);
		return sdlog_config_save();
	} else if (ret != 0) {
		LOG_ERR("Failed to open config file (%d)", ret);
		return -EIO;
	}

	ssize_t read_len = fs_read(&file, &sdlog.cfg, sizeof(sdlog.cfg));

	fs_close(&file);

	if (read_len != (ssize_t)sizeof(sdlog.cfg)) {
		LOG_WRN("Config file size mismatch (%zd/%zu) - treating as corrupted",
			read_len, sizeof(sdlog.cfg));
		rebuild_config_from_disk(&sdlog.cfg);
		return sdlog_config_save();
	}

	if (sdlog.cfg.magic != SDLOG_CONFIG_MAGIC ||
	    sdlog.cfg.version != SDLOG_CONFIG_VERSION ||
	    sdlog.cfg.crc32 != sdlog_crc32(&sdlog.cfg)) {
		LOG_WRN("Config file failed validation (magic/version/CRC) - "
			"rebuilding from card contents");
		rebuild_config_from_disk(&sdlog.cfg);
		return sdlog_config_save();
	}

	LOG_INF("Loaded config: last_written=%s entry=%u modem_read=%s entry=%u oldest=%s",
		sdlog.cfg.last_written_file, sdlog.cfg.last_written_entry,
		sdlog.cfg.modem_read_file, sdlog.cfg.modem_read_entry,
		sdlog.cfg.oldest_file);

	sdlog.cfg_valid = true;
	return 0;
}
