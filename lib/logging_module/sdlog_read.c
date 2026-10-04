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
 * @brief Read one '\n'-terminated line from @p file, starting at its
 * current position, and leave the file positioned right after the
 * newline.
 *
 * If @p skip_only is true, the line's content is discarded (no data
 * is copied to @p buf, which may be NULL) - used to walk past
 * earlier entries when seeking to a target index. Otherwise the line
 * content (excluding the newline itself) is copied into @p buf and
 * *out_len is set to its length.
 *
 * Uses fs_tell()/absolute fs_seek() (rather than relative seeks) to
 * reposition precisely once a newline is found within a read chunk,
 * so a chunk boundary landing mid-entry never causes drift.
 *
 * @retval 0 on success.
 * @retval -ENODATA if EOF is reached before a terminating '\n' is
 *         found (covers both "no more entries" and a torn/partial
 *         final write - either way there is no complete entry to
 *         return).
 * @retval -ENOMEM if the line is longer than buf_size (only possible
 *         when skip_only is false).
 * @retval -EIO on file I/O error.
 */
static int read_line(struct fs_file_t *file, uint8_t *buf, size_t buf_size,
		      size_t *out_len, bool skip_only)
{
	uint8_t chunk[SDLOG_LINE_SCAN_CHUNK];
	size_t total = 0;
	bool overflow = false;

	for (;;) {
		off_t chunk_start = fs_tell(file);
		ssize_t r = fs_read(file, chunk, sizeof(chunk));

		if (r < 0) {
			return -EIO;
		}
		if (r == 0) {
			/* EOF with no '\n' found: either there is
			 * simply no more data, or the final write was
			 * torn (no trailing newline was ever
			 * committed). Either way there is no complete
			 * entry to return here.
			 */
			return -ENODATA;
		}

		for (ssize_t i = 0; i < r; i++) {
			if (chunk[i] != '\n') {
				continue;
			}

			if (!skip_only) {
				if (overflow || total + (size_t)i > buf_size) {
					overflow = true;
				} else if (i > 0) {
					memcpy(buf + total, chunk, (size_t)i);
				}
			}
			total += (size_t)i;

			if (fs_seek(file, chunk_start + i + 1, FS_SEEK_SET) != 0) {
				return -EIO;
			}

			if (overflow) {
				return -ENOMEM;
			}
			if (out_len != NULL) {
				*out_len = total;
			}
			return 0;
		}

		/* No newline in this chunk yet - accumulate and keep
		 * scanning forward.
		 */
		if (!skip_only) {
			if (total + (size_t)r > buf_size) {
				overflow = true;
			} else {
				memcpy(buf + total, chunk, (size_t)r);
			}
		}
		total += (size_t)r;
	}
}

/**
 * @brief Read the entry at position @p index within @p path.
 *
 * Entries are '\n'-delimited lines (see sdlog_do_write()). Walks
 * @p index lines from the start of the file (cheap: files are capped
 * at CONFIG_SDLOG_MAX_ENTRIES_PER_FILE entries) and copies the line
 * at that index into @p buf.
 *
 * @retval 0 on success.
 * @retval -ENODATA if the file has fewer than (index + 1) complete
 *         (newline-terminated) entries.
 * @retval -ENOMEM if buf_size is too small for the stored entry.
 * @retval -EIO on file I/O error.
 */
static int try_read_entry(const char *path, uint32_t index,
			   uint8_t *buf, size_t buf_size, size_t *out_len)
{
	struct fs_file_t file;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret != 0) {
		return -EIO;
	}

	for (uint32_t i = 0; i < index; i++) {
		ret = read_line(&file, NULL, 0, NULL, true);
		if (ret != 0) {
			fs_close(&file);
			return ret;
		}
	}

	size_t len = 0;

	ret = read_line(&file, buf, buf_size, &len, false);
	fs_close(&file);

	if (ret != 0) {
		return ret;
	}

	if (out_len != NULL) {
		*out_len = len;
	}
	return 0;
}

/**
 * @brief Find the log file with the smallest file-number greater
 * than @p current_name's, implementing the "move to next available
 * file" hand-off from LOG-RD-06.
 *
 * Note: because file numbers wrap and are reused once a file is
 * deleted (per project decision), this ordering is only reliable
 * within a single wrap cycle. This matches the scope of the
 * attached requirements, which do not define cross-wrap read
 * ordering; see README for detail.
 */
static int find_next_log_file(const char *current_name, char *out_name, size_t out_size)
{
	static uint32_t nums[CONFIG_SDLOG_MAX_FILES_SCAN];
	static char names[CONFIG_SDLOG_MAX_FILES_SCAN][SDLOG_FNAME_MAX];
	size_t found = 0;
	uint32_t cur_num;
	int best_idx = -1;
	size_t prefix_len = strlen(CONFIG_SDLOG_FILE_PREFIX);

	if (strncmp(current_name, CONFIG_SDLOG_FILE_PREFIX, prefix_len) != 0 ||
	    current_name[prefix_len] != '_' ||
	    !sdlog_parse_uint32(&current_name[prefix_len + 1], &cur_num)) {
		return -EINVAL;
	}

	sdlog_scan_log_files(nums, names, CONFIG_SDLOG_MAX_FILES_SCAN, &found);

	for (size_t i = 0; i < found; i++) {
		if (nums[i] > cur_num &&
		    (best_idx < 0 || nums[i] < nums[best_idx])) {
			best_idx = (int)i;
		}
	}

	if (best_idx < 0) {
		return -ENODATA;
	}

	strncpy(out_name, names[best_idx], out_size - 1);
	out_name[out_size - 1] = '\0';
	return 0;
}

int sdlog_do_read_next(uint8_t *buf, size_t buf_size, size_t *out_len)
{
	char path[SDLOG_FNAME_MAX + 8];
	int ret;

	/* LOG-RD-01: identify the Modem's last read file/entry from
	 * config. This state is exclusively the Modem's - TFTP's
	 * random file access never reads or writes it.
	 */
	if (sdlog.cfg.modem_read_file[0] == '\0') {
		if (sdlog.cfg.oldest_file[0] == '\0') {
			return -ENODATA; /* nothing has ever been logged */
		}
		strncpy(sdlog.cfg.modem_read_file, sdlog.cfg.oldest_file,
			SDLOG_FNAME_MAX - 1);
		sdlog.cfg.modem_read_file[SDLOG_FNAME_MAX - 1] = '\0';
		sdlog.cfg.modem_read_entry = 0;
	}

	for (;;) {
		size_t len = 0;

		/* LOG-RD-02/03/04: open, read next entry, close. */
		sdlog_build_full_path(path, sizeof(path), sdlog.cfg.modem_read_file);
		ret = try_read_entry(path, sdlog.cfg.modem_read_entry, buf, buf_size, &len);

		if (ret == 0) {
			/* LOG-RD-05: persist new read position only
			 * after a successful read.
			 */
			sdlog.cfg.modem_read_entry++;
			int sret = sdlog_config_save();

			if (sret != 0) {
				return sret;
			}
			if (out_len != NULL) {
				*out_len = len;
			}
			return 0;
		}

		if (ret == -ENOMEM) {
			/* Caller's buffer is too small - do not
			 * advance state so a retry with a bigger
			 * buffer still gets this entry.
			 */
			return -ENOMEM;
		}

		if (ret == -ENODATA) {
			/* LOG-RD-06: current file exhausted. If it's
			 * also the active write file, there is
			 * nothing further to read yet.
			 */
			if (strcmp(sdlog.cfg.modem_read_file,
				   sdlog.cfg.last_written_file) == 0) {
				return -ENODATA;
			}

			char next_name[SDLOG_FNAME_MAX];

			if (find_next_log_file(sdlog.cfg.modem_read_file,
						next_name, sizeof(next_name)) != 0) {
				/* No newer file found (e.g. it was
				 * deleted by cleanup) - nothing more
				 * to read right now.
				 */
				return -ENODATA;
			}

			strncpy(sdlog.cfg.modem_read_file, next_name,
				SDLOG_FNAME_MAX - 1);
			sdlog.cfg.modem_read_file[SDLOG_FNAME_MAX - 1] = '\0';
			sdlog.cfg.modem_read_entry = 0;

			int sret = sdlog_config_save();

			if (sret != 0) {
				return sret;
			}
			continue; /* LOG-RD-07: retry from the new file */
		}

		/* -EIO or other unexpected error. */
		return ret;
	}
}
