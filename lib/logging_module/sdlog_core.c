/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>

#include "sdlog_internal.h"

LOG_MODULE_DECLARE(sdlog, CONFIG_SDLOG_LOG_LEVEL);

struct sdlog_state sdlog = {
	.initialized = false,
	.cfg_valid = false,
};

/* Initialize the module's mutex once at POST_KERNEL, before main()
 * and before any application thread could possibly call into the
 * public API. This avoids relying on sdlog_init() being the very
 * first touch of the mutex (safe even if sdlog_init() is retried
 * after a failure) and keeps k_mutex_init() out of the hot init
 * path entirely.
 */
static int sdlog_mutex_setup(void)
{
	k_mutex_init(&sdlog.lock);
	return 0;
}
SYS_INIT(sdlog_mutex_setup, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static const char *disk_pdrv = CONFIG_SDLOG_DISK_NAME;
static const char *mount_pt = CONFIG_SDLOG_MOUNT_POINT;

/* ------------------------------------------------------------------ */
/* RTC (LOG-GEN-02/03: <TimeStamp> in file names)                      */
/* ------------------------------------------------------------------ */

#define SDLOG_RTC_NODE DT_ALIAS(rtc)

#if !DT_NODE_EXISTS(SDLOG_RTC_NODE)
#error "sdcard_logging: no devicetree 'rtc' alias found. Add a real-time " \
       "clock to your board and declare it as `aliases { rtc = &<node>; };` " \
       "in your board devicetree/overlay, and enable CONFIG_RTC=y (plus " \
       "your board's specific RTC driver Kconfig, e.g. an NXP SNVS RTC " \
       "driver on i.MX RT). Log file names require a real timestamp per " \
       "LOG-GEN-02/03."
#endif

static const struct device *const rtc_dev = DEVICE_DT_GET(SDLOG_RTC_NODE);

int sdlog_rtc_timestamp(char *buf, size_t buf_size)
{
	struct rtc_time tm;
	int ret;

	if (buf_size < SDLOG_TIMESTAMP_LEN + 1) {
		return -EINVAL;
	}

	if (!device_is_ready(rtc_dev)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	ret = rtc_get_time(rtc_dev, &tm);
	if (ret) {
		LOG_ERR("rtc_get_time failed (%d)", ret);
		return ret;
	}

	/* struct rtc_time embeds struct tm semantics: tm_year is years
	 * since 1900, tm_mon is 0-11.
	 */
	snprintf(buf, buf_size, "%04d%02d%02d%02d%02d%02d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);

	return 0;
}

int sdlog_rtc_date(char *buf, size_t buf_size)
{
	char ts[SDLOG_TIMESTAMP_LEN + 1];
	int ret;

	if (buf_size < 9) {
		return -EINVAL;
	}

	ret = sdlog_rtc_timestamp(ts, sizeof(ts));
	if (ret != 0) {
		return ret;
	}

	/* First 8 chars of "YYYYMMDDHHMMSS" are the date. */
	memcpy(buf, ts, 8);
	buf[8] = '\0';
	return 0;
}

/* ------------------------------------------------------------------ */
/* Disk detection / capacity (LOG-INIT-01/02)                          */
/* ------------------------------------------------------------------ */

int sdlog_disk_check(uint64_t *capacity_bytes)
{
	uint32_t block_count = 0;
	uint32_t block_size = 0;
	int ret;

	ret = disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret != 0) {
		LOG_ERR("SD card not detected/init failed (%d)", ret);
		return -ENODEV;
	}

	ret = disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_COUNT, &block_count);
	if (ret != 0) {
		LOG_ERR("Unable to get sector count (%d)", ret);
		disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_DEINIT, NULL);
		return -EIO;
	}

	ret = disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_SIZE, &block_size);
	if (ret != 0) {
		LOG_ERR("Unable to get sector size (%d)", ret);
		disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_DEINIT, NULL);
		return -EIO;
	}

	if (capacity_bytes != NULL) {
		*capacity_bytes = (uint64_t)block_count * block_size;
	}

	LOG_INF("SD card detected: %u sectors x %u bytes (%llu MB)",
		block_count, block_size,
		(unsigned long long)(((uint64_t)block_count * block_size) >> 20));

	return 0;
}

/* ------------------------------------------------------------------ */
/* Mount / format (LOG-INIT-03/04/05)                                  */
/* ------------------------------------------------------------------ */

int sdlog_fs_mount(bool *did_format)
{
	int ret;

	if (did_format != NULL) {
		*did_format = false;
	}

	sdlog.mount.type = FS_FATFS;
	sdlog.mount.fs_data = &sdlog.fat_fs;
	sdlog.mount.mnt_point = mount_pt;
	sdlog.mount.flags = 0;

	ret = fs_mount(&sdlog.mount);
	if (ret == 0) {
		LOG_INF("SD card mounted at %s", mount_pt);
		return 0;
	}

	LOG_WRN("Mount failed (%d) - formatting card (LOG-INIT-03/04/05)", ret);

	/* Deliberately NOT using CONFIG_FS_FATFS_MOUNT_MKFS's built-in
	 * format-on-mount-failure: it hardcodes MKFS_PARM.fmt to
	 * FM_ANY | FM_SFD, which creates a "superfloppy" FAT32 volume
	 * with NO MBR partition table. That mounts and works fine in
	 * Zephyr, but SD/MMC media is conventionally expected to carry
	 * a normal MBR partition table (per elm-chan's own FatFs docs:
	 * "The FDISK [MBR] format is usually used for harddisk, MMC,
	 * SDC and CFC... The SFD format ... is used for floppy disk,
	 * Microdrive, optical disk and super-floppy media"). Many SD
	 * card readers/OSes (Windows in particular) will not show an
	 * SFD-formatted card as a normal drive at all.
	 *
	 * Format explicitly instead, with FM_FAT32 (FM_SFD omitted),
	 * which creates a single MBR partition spanning the whole
	 * card - readable like any commercially pre-formatted SD card.
	 */
	MKFS_PARM mkfs_opt = {
		.fmt = FM_FAT32,
		.n_fat = 1,
		.align = 0,
		.n_root = CONFIG_FS_FATFS_MAX_ROOT_ENTRIES,
		.au_size = 0,
	};

	ret = fs_mkfs(FS_FATFS, (uintptr_t)mount_pt, &mkfs_opt, 0);
	if (ret != 0) {
		LOG_ERR("fs_mkfs failed (%d)", ret);
		return -EIO;
	}

	ret = fs_mount(&sdlog.mount);
	if (ret != 0) {
		LOG_ERR("fs_mount failed after formatting (%d)", ret);
		return -EIO;
	}

	if (did_format != NULL) {
		*did_format = true;
	}

	LOG_INF("SD card formatted (MBR-partitioned FAT32) and mounted at %s",
		mount_pt);
	return 0;
}

int sdlog_fs_unmount(void)
{
	int ret = fs_unmount(&sdlog.mount);

	if (ret != 0) {
		LOG_ERR("fs_unmount failed (%d)", ret);
		return -EIO;
	}

	disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_DEINIT, NULL);
	return 0;
}

int sdlog_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
	struct fs_statvfs stat;
	int ret;

	ret = fs_statvfs(mount_pt, &stat);
	if (ret != 0) {
		LOG_ERR("fs_statvfs failed (%d)", ret);
		return -EIO;
	}

	if (total_bytes != NULL) {
		*total_bytes = (uint64_t)stat.f_blocks * stat.f_frsize;
	}
	if (free_bytes != NULL) {
		*free_bytes = (uint64_t)stat.f_bfree * stat.f_frsize;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Filename helpers (LOG-GEN-02/03)                                    */
/* ------------------------------------------------------------------ */

void sdlog_build_log_filename(char *buf, size_t buf_size,
			       uint32_t file_number, const char *timestamp)
{
	snprintf(buf, buf_size, "%s_%05u_%s.csv",
		 CONFIG_SDLOG_FILE_PREFIX, file_number, timestamp);
}

void sdlog_build_config_path(char *buf, size_t buf_size)
{
	snprintf(buf, buf_size, "%s/%s", mount_pt, CONFIG_SDLOG_CONFIG_FILE_NAME);
}

void sdlog_build_full_path(char *buf, size_t buf_size, const char *fname)
{
	snprintf(buf, buf_size, "%s/%s", mount_pt, fname);
}

bool sdlog_parse_uint32(const char *s, uint32_t *out)
{
	uint32_t val;

	if (s == NULL || *s < '0' || *s > '9') {
		return false;
	}

	val = 0;
	while (*s >= '0' && *s <= '9') {
		val = (val * 10U) + (uint32_t)(*s - '0');
		s++;
	}

	*out = val;
	return true;
}

int sdlog_extract_file_date(const char *fname, char *out_date)
{
	size_t prefix_len = strlen(CONFIG_SDLOG_FILE_PREFIX);
	const char *p;

	if (fname == NULL || out_date == NULL) {
		return -EINVAL;
	}

	if (strncmp(fname, CONFIG_SDLOG_FILE_PREFIX, prefix_len) != 0 ||
	    fname[prefix_len] != '_') {
		return -EINVAL;
	}

	/* Skip "<prefix>_" then the digits of <File Number>. */
	p = fname + prefix_len + 1;
	if (*p < '0' || *p > '9') {
		return -EINVAL;
	}
	while (*p >= '0' && *p <= '9') {
		p++;
	}
	if (*p != '_') {
		return -EINVAL;
	}
	p++; /* now at the start of the 14-digit timestamp */

	for (int i = 0; i < 8; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return -EINVAL;
		}
		out_date[i] = p[i];
	}
	out_date[8] = '\0';
	return 0;
}

/* ------------------------------------------------------------------ */
/* Directory scan (used for config-file rebuild, cleanup, read hand-off)*/
/* ------------------------------------------------------------------ */

int sdlog_scan_log_files(uint32_t *numbers, char (*names)[SDLOG_FNAME_MAX],
			  size_t max_numbers, size_t *out_count)
{
	struct fs_dir_t dirp;
	struct fs_dirent entry;
	size_t count = 0;
	size_t prefix_len = strlen(CONFIG_SDLOG_FILE_PREFIX);
	int ret;

	fs_dir_t_init(&dirp);

	ret = fs_opendir(&dirp, mount_pt);
	if (ret != 0) {
		LOG_ERR("fs_opendir(%s) failed (%d)", mount_pt, ret);
		return -EIO;
	}

	for (;;) {
		ret = fs_readdir(&dirp, &entry);
		if (ret != 0 || entry.name[0] == '\0') {
			break;
		}

		if (entry.type != FS_DIR_ENTRY_FILE) {
			continue;
		}

		/* Expect "<prefix>_<number>_<timestamp>" */
		if (strncmp(entry.name, CONFIG_SDLOG_FILE_PREFIX, prefix_len) != 0 ||
		    entry.name[prefix_len] != '_') {
			continue;
		}

		uint32_t num = 0;

		if (!sdlog_parse_uint32(&entry.name[prefix_len + 1], &num)) {
			continue;
		}

		if (count < max_numbers) {
			numbers[count] = num;
			if (names != NULL) {
				strncpy(names[count], entry.name, SDLOG_FNAME_MAX - 1);
				names[count][SDLOG_FNAME_MAX - 1] = '\0';
			}
			count++;
		} else {
			LOG_WRN("sdlog_scan_log_files: more log files on card than "
				"CONFIG_SDLOG_MAX_FILES_SCAN (%d); ignoring rest",
				CONFIG_SDLOG_MAX_FILES_SCAN);
			break;
		}
	}

	fs_closedir(&dirp);

	*out_count = count;
	return 0;
}

uint32_t sdlog_alloc_file_number(void)
{
	static uint32_t scan_nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	size_t found = 0;
	uint32_t candidate = sdlog.cfg.next_file_number;
	uint32_t attempts;

	sdlog_scan_log_files(scan_nums, NULL, CONFIG_SDLOG_MAX_FILES_SCAN, &found);

	/* Walk forward from the stored counter, wrapping at the
	 * configured max, and skip any number that is still on disk
	 * (per the "wraps/reuses numbers once old files deleted"
	 * policy).
	 */
	for (attempts = 0; attempts <= CONFIG_SDLOG_MAX_FILE_NUMBER; attempts++) {
		bool in_use = false;

		for (size_t i = 0; i < found; i++) {
			if (scan_nums[i] == candidate) {
				in_use = true;
				break;
			}
		}

		if (!in_use) {
			break;
		}

		candidate = (candidate + 1) % (CONFIG_SDLOG_MAX_FILE_NUMBER + 1);
	}

	sdlog.cfg.next_file_number = (candidate + 1) % (CONFIG_SDLOG_MAX_FILE_NUMBER + 1);

	return candidate;
}
