/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdlog_internal.h
 * @brief Private declarations shared by the sdlog_*.c translation units.
 *
 * Nothing in this file is part of the public API. The application must
 * never include this header directly - only
 * <logging_module/logging_module.h>.
 */

#ifndef SDLOG_INTERNAL_H_
#define SDLOG_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <ff.h> /* CONFIG_FAT_FILESYSTEM_ELM is select'ed by this module's Kconfig */
#include <logging_module/logging_module.h>

/* ------------------------------------------------------------------ */
/* Constants / layout                                                  */
/* ------------------------------------------------------------------ */

#define SDLOG_FNAME_MAX          48
#define SDLOG_TIMESTAMP_LEN      14  /* YYYYMMDDHHMMSS */
#define SDLOG_CONFIG_MAGIC       0x53444C47UL /* "SDLG" */
#define SDLOG_CONFIG_VERSION     2

/* Records are stored as newline-delimited plain text: whatever bytes
 * the caller passes to sdlog_write(), followed by a single '\n'.
 * This means a stored entry must not itself contain an embedded '\n'
 * byte - sdlog_write() rejects that with -EINVAL, since it would be
 * indistinguishable from two separate entries on read-back.
 */
#define SDLOG_LINE_SCAN_CHUNK    32

/**
 * @brief On-disk / in-memory layout of the persisted config file.
 *
 * Implements LOG-GEN-04, LOG-GEN-05 and LOG-INIT-11: the module keeps
 * enough information here to identify the last written file/entry,
 * the Modem consumer's independent sequential read position, oldest
 * available file, and the current active file's entry count, so
 * state survives a reset or power cycle (LOG-WR-09).
 *
 * NOTE: this only tracks the Modem's sequential read position.
 * TFTP's random file access (sdlog_file_open()/read()/close()) is
 * stateless from this struct's point of view - it never touches
 * modem_read_file/modem_read_entry, per the "accessing a file for
 * TFTP must not modify the Modem read position" requirement.
 *
 * Stored as a fixed-size binary struct (single, fixed file name -
 * see README for rationale) with a CRC32 so a corrupted config file
 * can be detected and the module can gracefully rebuild state from
 * the card contents instead of trusting garbage data.
 */
struct sdlog_config_data {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;

	char last_written_file[SDLOG_FNAME_MAX];
	uint32_t last_written_entry;

	/* Modem's independent sequential read cursor. */
	char modem_read_file[SDLOG_FNAME_MAX];
	uint32_t modem_read_entry;

	char oldest_file[SDLOG_FNAME_MAX];

	/* Number of entries currently stored in last_written_file. */
	uint32_t file_entry_count;

	/* Next value to hand out for <File Number> in new log file
	 * names. Wraps at CONFIG_SDLOG_MAX_FILE_NUMBER and skips any
	 * number still in use on disk (see sdlog_core.c).
	 */
	uint32_t next_file_number;

	/* CRC32 (IEEE) over every field above. Must be last. */
	uint32_t crc32;
} __packed;

/**
 * @brief Full runtime state of the module (single instance).
 */
struct sdlog_state {
	struct k_mutex lock;
	bool initialized;

	struct fs_mount_t mount;
	FATFS fat_fs;

	struct sdlog_config_data cfg;

	/* True once cfg has been loaded/rebuilt and matches what's on
	 * the card; used to avoid writing a config we know is stale
	 * on a hard init failure.
	 */
	bool cfg_valid;
};

extern struct sdlog_state sdlog;

/* ------------------------------------------------------------------ */
/* sdlog_core.c                                                        */
/* ------------------------------------------------------------------ */

int sdlog_disk_check(uint64_t *capacity_bytes);
int sdlog_fs_mount(bool *did_format);
int sdlog_fs_unmount(void);
int sdlog_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

int sdlog_rtc_timestamp(char *buf, size_t buf_size);

void sdlog_build_log_filename(char *buf, size_t buf_size,
			       uint32_t file_number, const char *timestamp);
void sdlog_build_config_path(char *buf, size_t buf_size);
void sdlog_build_full_path(char *buf, size_t buf_size, const char *fname);

/**
 * @brief Get the current date as an 8-digit "YYYYMMDD" string.
 *
 * @param buf      Output buffer, at least 9 bytes (8 digits + NUL).
 * @param buf_size Capacity of @p buf.
 *
 * @retval 0 on success.
 * @retval -EINVAL if buf_size is too small.
 * @retval -ENODEV if the RTC device is not ready.
 * @retval negative errno on other RTC failures.
 */
int sdlog_rtc_date(char *buf, size_t buf_size);

/**
 * @brief Extract the 8-digit "YYYYMMDD" date embedded in a log file
 * name of the form "<prefix>_<number>_<14-digit-timestamp>.csv".
 *
 * @param fname    Filename to parse (just the name, not a full path).
 * @param out_date Output buffer, at least 9 bytes (8 digits + NUL).
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p fname doesn't match the expected shape.
 */
int sdlog_extract_file_date(const char *fname, char *out_date);

/**
 * @brief Parse a run of ASCII decimal digits at the start of @p s.
 *
 * Deliberately avoids sscanf()/the scanf family, which
 * CONFIG_MINIMAL_LIBC does not provide - this keeps the module
 * portable across every Zephyr libc choice (minimal libc, picolibc,
 * newlib).
 *
 * @retval true if at least one digit was parsed, with *out set.
 * @retval false if s is NULL or does not start with a digit.
 */
bool sdlog_parse_uint32(const char *s, uint32_t *out);

/**
 * @brief Scan the mount point for existing "<prefix>_*" log files.
 *
 * Fills @p numbers (caller-provided array) with the numeric
 * <File Number> parsed out of every matching file name found, and
 * sets *out_count to how many were found (capped at max_numbers).
 * Also optionally returns the matching filenames via @p names
 * (same indexing as numbers), each of size SDLOG_FNAME_MAX, if
 * names != NULL.
 *
 * Used to rebuild state when the config file is missing/corrupt,
 * to find the current oldest file for cleanup, and to find the
 * next file for sequential reads.
 */
int sdlog_scan_log_files(uint32_t *numbers, char (*names)[SDLOG_FNAME_MAX],
			  size_t max_numbers, size_t *out_count);

/* Allocate (and persist) the next file number to use for a new log
 * file, per the wrap/reuse-once-deleted policy.
 */
uint32_t sdlog_alloc_file_number(void);

/* ------------------------------------------------------------------ */
/* sdlog_config.c                                                      */
/* ------------------------------------------------------------------ */

int sdlog_config_load_or_create(void);
int sdlog_config_save(void);
uint32_t sdlog_crc32(const struct sdlog_config_data *cfg);

/* ------------------------------------------------------------------ */
/* sdlog_write.c                                                       */
/* ------------------------------------------------------------------ */

int sdlog_do_write(const uint8_t *data, size_t len);
int sdlog_create_new_active_file(void);

/**
 * @brief Roll over to a new active log file if the current one's
 * embedded date no longer matches today's date (daily log file
 * creation). No-op (returns 0) if there is no active file yet, the
 * date can't be determined, or the date still matches.
 */
int sdlog_check_daily_rotation(void);

/* ------------------------------------------------------------------ */
/* sdlog_read.c - Modem's independent sequential read cursor           */
/* ------------------------------------------------------------------ */

int sdlog_do_read_next(uint8_t *buf, size_t buf_size, size_t *out_len);

/* ------------------------------------------------------------------ */
/* sdlog_cleanup.c                                                     */
/* ------------------------------------------------------------------ */

int sdlog_cleanup_check_and_run(void);

/* ------------------------------------------------------------------ */
/* sdlog_query.c - file-count / date-range / listing APIs              */
/* ------------------------------------------------------------------ */

int sdlog_do_get_file_count(uint32_t *count);
int sdlog_do_get_oldest_date(struct sdlog_date *date);
int sdlog_do_get_newest_date(struct sdlog_date *date);
int sdlog_do_get_file_count_in_range(const struct sdlog_date *start,
				      const struct sdlog_date *end,
				      uint32_t *count);
int sdlog_do_list_files_in_range(const struct sdlog_date *start,
				  const struct sdlog_date *end,
				  struct sdlog_file_info *out,
				  size_t max_files, size_t *out_count);

/* ------------------------------------------------------------------ */
/* sdlog_random.c - TFTP-style random access to a specific file        */
/* ------------------------------------------------------------------ */

int sdlog_do_file_open(const char *filename, sdlog_file_handle_t *handle);
int sdlog_do_file_read(sdlog_file_handle_t handle, size_t offset,
			uint8_t *buf, size_t buf_size, size_t *out_len);
int sdlog_do_file_size(sdlog_file_handle_t handle, size_t *size_bytes);
int sdlog_do_file_close(sdlog_file_handle_t handle);

/**
 * @brief Whether @p fname currently has an open random-access handle
 * (i.e. a TFTP transfer in progress against it). Used by
 * sdlog_cleanup.c to avoid deleting a file out from under an
 * in-progress transfer - deleting a file mid sdlog_write()-caused
 * -EIO or ENOENT lets us handle Modem's read state gracefully, but
 * unlinking a file that's actively open for a raw fs_read() has much
 * less predictable behavior on FAT, so this one case gets extra
 * protection beyond the general "space always wins" cleanup policy.
 */
bool sdlog_is_file_open_for_random_access(const char *fname);

/**
 * @brief Close every currently open random-access handle.
 *
 * Called by sdlog_deinit() so a handle from sdlog_file_open() never
 * dangles across an unmount (which would otherwise leave it pointing
 * at a torn-down filesystem).
 */
void sdlog_close_all_random_handles(void);

#endif /* SDLOG_INTERNAL_H_ */
