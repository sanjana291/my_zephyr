/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <logging_module/logging_module.h>
#include "sdlog_internal.h"

#if defined(CONFIG_SDLOG_TCP_SERVER)
#include "sdlog_tcp.h"
#endif

LOG_MODULE_REGISTER(sdlog, CONFIG_SDLOG_LOG_LEVEL);

static bool locked_init_done(void)
{
	return sdlog.initialized;
}

int SDLOG_Init(void)
{
	int ret;

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (sdlog.initialized) {
		k_mutex_unlock(&sdlog.lock);
		return -EALREADY;
	}

	/* LOG-INIT-01/02: detect card, capacity/free space. */
	uint64_t capacity = 0;

	ret = sdlog_disk_check(&capacity);
	if (ret != 0) {
		k_mutex_unlock(&sdlog.lock);
		return ret;
	}

	/* LOG-INIT-03/04/05: mount, formatting a fresh/corrupted card
	 * (as a normal MBR-partitioned FAT32 volume - see the NOTE
	 * in sdlog_fs_mount()) if needed.
	 */
	ret = sdlog_fs_mount(NULL);
	if (ret != 0) {
		k_mutex_unlock(&sdlog.lock);
		return ret;
	}

	/* LOG-INIT-06/07/10/11: find/restore or create the config file. */
	ret = sdlog_config_load_or_create();
	if (ret != 0) {
		sdlog_fs_unmount();
		k_mutex_unlock(&sdlog.lock);
		return ret;
	}

	// /* LOG-INIT-08/09: ensure there is an active log file to write
	//  * to, or - item 1, daily log file creation - roll over to a
	//  * fresh one if the existing active file's date isn't today.
	//  */
	// if (sdlog.cfg.last_written_file[0] == '\0') {
	// 	ret = sdlog_create_new_active_file();
	// 	if (ret != 0) {
	// 		sdlog_fs_unmount();
	// 		k_mutex_unlock(&sdlog.lock);
	// 		return ret;
	// 	}
	// } else {
	// 	ret = sdlog_check_daily_rotation();
	// 	if (ret != 0) {
	// 		sdlog_fs_unmount();
	// 		k_mutex_unlock(&sdlog.lock);
	// 		return ret;
	// 	}
	// }

	sdlog.initialized = true;
	LOG_INF("SD card logging module initialized (capacity=%llu bytes)",
		(unsigned long long)capacity);

	k_mutex_unlock(&sdlog.lock);

	/*
	 * Start the integrated TCP server after the filesystem is ready.
	 *
	 * The application is NEVER notified when a TCP connection is
	 * received; the entire Logging Workflow (Security Sequence,
	 * MTP 07/08/09/10/11/12, and TFTP file transfer) is handled
	 * internally by sdlog_tcp.c and sdlog_tftp.c.
	 *
	 * The mutex is NOT held here: the server thread itself acquires
	 * sdlog.lock only while calling the sdlog_do_* internal APIs,
	 * and the server socket setup does not touch sdlog state.
	 */
#if defined(CONFIG_SDLOG_TCP_SERVER)
	ret = sdlog_tcp_server_start();
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("SDLOG TCP server start failed (%d) - "
			"logging to SD continues, TCP unavailable", ret);
		/* Non-fatal: SD-card logging still works. */
	}
#endif

	return 0;
}

int SDLOG_Deinit(void)
{
	int ret;

	/*
	 * Stop the TCP server before acquiring the filesystem lock so
	 * that any in-flight TCP handler that is waiting for sdlog.lock
	 * can complete and release it first.
	 */
#if defined(CONFIG_SDLOG_TCP_SERVER)
	sdlog_tcp_server_stop();
#endif

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (!sdlog.initialized) {
		k_mutex_unlock(&sdlog.lock);
		return -EALREADY;
	}

	sdlog_close_all_random_handles();

	ret = sdlog_fs_unmount();
	sdlog.initialized = false;

	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Write(const uint8_t *data, size_t len)
{
	int ret;

	if (data == NULL || len == 0 || len > CONFIG_SDLOG_MAX_ENTRY_SIZE) {
		return -EINVAL;
	}

	/* Entries are stored as newline-delimited lines (see
	 * sdlog_do_write()) - an embedded '\n' would be
	 * indistinguishable from two separate entries on read-back,
	 * so reject it up front rather than silently corrupt framing.
	 */
	if (memchr(data, '\n', len) != NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}

	ret = sdlog_do_write(data, len);

	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Read_next(uint8_t *buf, size_t buf_size, size_t *out_len)
{
	int ret;

	if (buf == NULL || buf_size == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}

	ret = sdlog_do_read_next(buf, buf_size, out_len);

	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Get_status(struct sdlog_status *status)
{
	int ret;

	if (status == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}

	memset(status, 0, sizeof(*status));
	strncpy(status->last_written_file, sdlog.cfg.last_written_file,
		sizeof(status->last_written_file) - 1);
	status->last_written_entry = sdlog.cfg.last_written_entry;
	strncpy(status->modem_read_file, sdlog.cfg.modem_read_file,
		sizeof(status->modem_read_file) - 1);
	status->modem_read_entry = sdlog.cfg.modem_read_entry;
	strncpy(status->oldest_file, sdlog.cfg.oldest_file,
		sizeof(status->oldest_file) - 1);
	status->file_entry_count = sdlog.cfg.file_entry_count;

	ret = sdlog_get_free_space(&status->card_free_bytes, &status->card_capacity_bytes);

	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Run_cleanup(void)
{
	int ret;

	k_mutex_lock(&sdlog.lock, K_FOREVER);

	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}

	ret = sdlog_cleanup_check_and_run();

	k_mutex_unlock(&sdlog.lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* File-count / date-range / listing APIs (item 3)                     */
/* ------------------------------------------------------------------ */

int SDLOG_Get_file_count(uint32_t *count)
{
	int ret;

	if (count == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_get_file_count(count);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Get_oldest_date(struct sdlog_date *date)
{
	int ret;

	if (date == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_get_oldest_date(date);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Get_newest_date(struct sdlog_date *date)
{
	int ret;

	if (date == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_get_newest_date(date);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_Get_file_count_in_range(const struct sdlog_date *start,
				   const struct sdlog_date *end,
				   uint32_t *count)
{
	int ret;

	if (start == NULL || end == NULL || count == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_get_file_count_in_range(start, end, count);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_List_files_in_range(const struct sdlog_date *start,
			       const struct sdlog_date *end,
			       struct sdlog_file_info *out,
			       size_t max_files, size_t *out_count)
{
	int ret;

	if (start == NULL || end == NULL || out == NULL || out_count == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_list_files_in_range(start, end, out, max_files, out_count);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Random file access - TFTP (items 2, 4)                              */
/* ------------------------------------------------------------------ */

int SDLOG_File_open(const char *filename, sdlog_file_handle_t *handle)
{
	int ret;

	if (filename == NULL || handle == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	if (!locked_init_done()) {
		k_mutex_unlock(&sdlog.lock);
		return -ENODEV;
	}
	ret = sdlog_do_file_open(filename, handle);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_File_read(sdlog_file_handle_t handle, size_t offset,
		     uint8_t *buf, size_t buf_size, size_t *out_len)
{
	int ret;

	if (buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	/* Deliberately not gated on locked_init_done(): a handle can
	 * only exist if sdlog_file_open() succeeded while
	 * initialized, and sdlog_do_file_read() itself validates the
	 * handle - no need for a second, redundant check here.
	 */
	ret = sdlog_do_file_read(handle, offset, buf, buf_size, out_len);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_File_size(sdlog_file_handle_t handle, size_t *size_bytes)
{
	int ret;

	if (size_bytes == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	ret = sdlog_do_file_size(handle, size_bytes);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}

int SDLOG_File_close(sdlog_file_handle_t handle)
{
	int ret;

	k_mutex_lock(&sdlog.lock, K_FOREVER);
	ret = sdlog_do_file_close(handle);
	k_mutex_unlock(&sdlog.lock);
	return ret;
}