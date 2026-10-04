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
 * @brief One slot in the random-access handle table.
 *
 * Deliberately kept separate from the Modem's sequential read state
 * in struct sdlog_config_data - nothing here is ever persisted, and
 * nothing here ever touches modem_read_file/modem_read_entry, so
 * TFTP (or any other random-access caller) can never affect the
 * Modem's read position, and vice versa.
 */
struct sdlog_random_slot {
	struct fs_file_t fp;
	char name[SDLOG_FNAME_MAX];
	bool in_use;
};

static struct sdlog_random_slot slots[CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES];

bool sdlog_is_file_open_for_random_access(const char *fname)
{
	for (int i = 0; i < CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES; i++) {
		if (slots[i].in_use && strcmp(slots[i].name, fname) == 0) {
			return true;
		}
	}
	return false;
}

int sdlog_do_file_open(const char *filename, sdlog_file_handle_t *handle)
{
	size_t prefix_len = strlen(CONFIG_SDLOG_FILE_PREFIX);
	char path[SDLOG_FNAME_MAX + 8];
	int slot = -1;
	int ret;

	/* Only this module's own log files may be opened this way -
	 * never an arbitrary path (no '/' allowed) and never the
	 * internal config file.
	 */
	if (strchr(filename, '/') != NULL ||
	    strncmp(filename, CONFIG_SDLOG_FILE_PREFIX, prefix_len) != 0 ||
	    filename[prefix_len] != '_') {
		return -EINVAL;
	}

	for (int i = 0; i < CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES; i++) {
		if (!slots[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		LOG_ERR("sdlog_file_open: no free handle slots "
			"(CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES=%d)",
			CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES);
		return -EMFILE;
	}

	sdlog_build_full_path(path, sizeof(path), filename);

	fs_file_t_init(&slots[slot].fp);
	ret = fs_open(&slots[slot].fp, path, FS_O_READ);
	if (ret != 0) {
		return (ret == -ENOENT) ? -ENOENT : -EIO;
	}

	strncpy(slots[slot].name, filename, SDLOG_FNAME_MAX - 1);
	slots[slot].name[SDLOG_FNAME_MAX - 1] = '\0';
	slots[slot].in_use = true;

	*handle = (sdlog_file_handle_t)slot;
	return 0;
}

/**
 * @brief Validate a handle and return its slot, or NULL if invalid.
 */
static struct sdlog_random_slot *slot_from_handle(sdlog_file_handle_t handle)
{
	if (handle < 0 || handle >= CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES) {
		return NULL;
	}
	if (!slots[handle].in_use) {
		return NULL;
	}
	return &slots[handle];
}

int sdlog_do_file_read(sdlog_file_handle_t handle, size_t offset,
			uint8_t *buf, size_t buf_size, size_t *out_len)
{
	struct sdlog_random_slot *s = slot_from_handle(handle);
	ssize_t r;

	if (s == NULL) {
		return -EINVAL;
	}

	if (fs_seek(&s->fp, (off_t)offset, FS_SEEK_SET) != 0) {
		return -EIO;
	}

	r = fs_read(&s->fp, buf, buf_size);
	if (r < 0) {
		return -EIO;
	}

	*out_len = (size_t)r;
	return 0;
}

int sdlog_do_file_size(sdlog_file_handle_t handle, size_t *size_bytes)
{
	const struct sdlog_random_slot *s = slot_from_handle(handle);
	char path[SDLOG_FNAME_MAX + 8];
	struct fs_dirent dirent;

	if (s == NULL) {
		return -EINVAL;
	}

	/* Use fs_stat() by name (rather than seeking the open file to
	 * the end and back) so the handle's current read position is
	 * never disturbed as a side effect of asking for the size.
	 */
	sdlog_build_full_path(path, sizeof(path), s->name);
	if (fs_stat(path, &dirent) != 0) {
		return -EIO;
	}

	*size_bytes = (size_t)dirent.size;
	return 0;
}

int sdlog_do_file_close(sdlog_file_handle_t handle)
{
	struct sdlog_random_slot *s = slot_from_handle(handle);

	if (s == NULL) {
		return -EINVAL;
	}

	fs_close(&s->fp);
	s->in_use = false;
	s->name[0] = '\0';
	return 0;
}

void sdlog_close_all_random_handles(void)
{
	for (int i = 0; i < CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES; i++) {
		if (slots[i].in_use) {
			fs_close(&slots[i].fp);
			slots[i].in_use = false;
			slots[i].name[0] = '\0';
		}
	}
}
