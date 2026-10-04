/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file logging_module.h
 * @brief Public API for the SD card file-system logging module.
 *
 * This module owns everything related to persisting application log
 * records on a FAT32-formatted SD card: card/file-system init and
 * mounting, daily + size-based log file creation/rotation, buffered
 * writes, two independent read mechanisms (a Modem sequential
 * reader and TFTP-style random file access), free-space based
 * cleanup, file-count/date-range/listing information, and a
 * persisted config file that lets logging state survive a reset or
 * power cycle.
 *
 */

#ifndef LOGGING_MODULE_H_
#define LOGGING_MODULE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Snapshot of the logging module's persisted/runtime state.
 *
 * Mirrors the information required to be tracked by LOG-GEN-05 /
 * LOG-INIT-11: last written file/entry, the Modem consumer's
 * independent sequential read position, oldest available file, and
 * current active-file entry count.
 *
 * NOTE: modem_read_file/modem_read_entry reflect ONLY the Modem's
 * sequential read cursor (see sdlog_read_next()). TFTP's random file
 * access (sdlog_file_open() and friends) has no persisted cursor of
 * its own and never appears here - by design, it cannot affect or
 * be affected by this state.
 */
struct sdlog_status {
	/** Name of the file currently open for writing. */
	char last_written_file[48];
	/** Index of the last entry successfully written to that file. */
	uint32_t last_written_entry;
	/** Name of the file the Modem's read cursor is positioned in. */
	char modem_read_file[48];
	/** Index of the last entry the Modem successfully read. */
	uint32_t modem_read_entry;
	/** Name of the oldest log file still present on the card. */
	char oldest_file[48];
	/** Number of entries currently stored in the active write file. */
	uint32_t file_entry_count;
	/** Total SD card capacity, in bytes. */
	uint64_t card_capacity_bytes;
	/** Free space currently available on the SD card, in bytes. */
	uint64_t card_free_bytes;
};

/**
 * @brief A calendar date, used by the date-range information APIs
 * and in struct sdlog_file_info.
 */
struct sdlog_date {
	uint16_t year;   /**< Full year, e.g. 2026. */
	uint8_t month;   /**< 1-12. */
	uint8_t day;     /**< 1-31. */
};

/**
 * @brief Metadata about one stored log file, as returned by
 * sdlog_list_files_in_range().
 */
struct sdlog_file_info {
	/** Filename, e.g. "ws-datalog_00007_20260827103002.csv". */
	char name[48];
	/** Date embedded in the file's name (when it was created). */
	struct sdlog_date date;
	/** Current size of the file on disk, in bytes. */
	uint32_t size_bytes;
};

/**
 * @brief Handle returned by sdlog_file_open(), used by
 * sdlog_file_read()/sdlog_file_size()/sdlog_file_close().
 *
 * Opaque: never construct or inspect a value of this type directly.
 * SDLOG_INVALID_FILE_HANDLE is never a value sdlog_file_open() hands
 * back on success.
 */
typedef int sdlog_file_handle_t;

#define SDLOG_INVALID_FILE_HANDLE ((sdlog_file_handle_t)-1)

/**
 * @brief Initialize and mount the SD card logging subsystem.
 *
 * Detects the SD card, verifies/creates the FAT32 file system
 * (formatting if the card is fresh or corrupted), loads the
 * persisted configuration file (creating one if absent or
 * rebuilding it from the card contents if corrupted), and opens or
 * creates the active log file so the module is immediately ready to
 * accept sdlog_write() calls. As part of this, also rolls over to a
 * fresh log file if the existing active file's date doesn't match
 * today's date (daily log file creation).
 *
 * Safe to call once at startup. Calling it again while already
 * initialized returns -EALREADY without side effects.
 *
 * @retval 0 on success.
 * @retval -EALREADY if already initialized.
 * @retval -ENODEV if the SD card or RTC device is not present/ready.
 * @retval -EIO on mount, format, or file-system error.
 * @retval negative errno on other failures.
 */
int SDLOG_Init(void);

/**
 * @brief Flush state, close open files and unmount the SD card.
 *
 * @retval 0 on success.
 * @retval -EALREADY if not currently initialized.
 * @retval negative errno on failure.
 */
int SDLOG_Deinit(void);

/**
 * @brief Write one variable-length log entry.
 *
 * The module checks free space first and runs cleanup if the card
 * is at/above the configured usage threshold, rolls over to a new
 * log file if either the entry-per-file limit has been reached OR
 * the active file's date no longer matches today's date (whichever
 * applies), appends the entry to the (possibly newly-created) active
 * log file (a plain-text .csv file: exactly the bytes given here,
 * followed by a single '\n'), and durably updates the persisted
 * configuration only after each step succeeds.
 *
 * @param data Pointer to the bytes to store, exactly as given (e.g.
 *             a comma-separated row you've already formatted). Must
 *             NOT contain an embedded '\n' byte - that would be
 *             indistinguishable from two separate entries when read
 *             back.
 * @param len  Length of @p data in bytes. Must be > 0 and must not
 *             exceed CONFIG_SDLOG_MAX_ENTRY_SIZE.
 *
 * @retval 0 on success.
 * @retval -EINVAL if data is NULL, len is 0, len exceeds the
 *         configured maximum entry size, or data contains an
 *         embedded '\n' byte.
 * @retval -ENODEV if the module is not initialized.
 * @retval -ENOSPC if the SD card has no usable space left even
 *         after cleanup.
 * @retval -EIO on file open/write/close failure.
 */
int SDLOG_Write(const uint8_t *data, size_t len);

/**
 * @brief Modem's sequential reader: read the next unread log entry,
 * in chronological order.
 *
 * This is the "sequential read mechanism" for the Modem consumer
 * described in the module's requirements. Its read position
 * (modem_read_file / modem_read_entry) is maintained independently
 * of everything else, persists across resets, and is never touched
 * by TFTP's random file access (sdlog_file_open() and friends) -
 * the two consumers cannot interfere with each other.
 *
 * Resumes from the last-read position recorded in the configuration
 * file, automatically advancing to the next available log file once
 * every entry in the current file has been consumed. Each entry is
 * one '\n'-delimited line in the .csv log file; the returned bytes
 * exclude the newline itself.
 *
 * @param buf      Buffer to receive the entry bytes (no trailing
 *                 newline included).
 * @param buf_size Capacity of @p buf, in bytes.
 * @param out_len  On success, set to the number of bytes written to
 *                 @p buf. May be NULL if not needed.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the module is not initialized.
 * @retval -ENODATA if there is no unread entry available.
 * @retval -ENOMEM if @p buf_size is smaller than the stored entry.
 * @retval -EIO on file open/read/close failure.
 */
int SDLOG_Read_next(uint8_t *buf, size_t buf_size, size_t *out_len);

/**
 * @brief Get a snapshot of the module's current status/state.
 *
 * @param status Output structure, filled in on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p status is NULL.
 * @retval -ENODEV if the module is not initialized.
 */
int SDLOG_Get_status(struct sdlog_status *status);

/**
 * @brief Force an immediate free-space check and cleanup pass.
 *
 * Normally cleanup runs automatically before each write per
 * LOG-CLR-01/02. This is exposed for cases where the application
 * wants to proactively reclaim space (e.g. before a large batch of
 * writes) without waiting for the next SDLOG_write() call.
 *
 * @retval 0 on success (including the case where no cleanup was
 *         necessary).
 * @retval -ENODEV if the module is not initialized.
 * @retval negative errno on failure.
 */
int SDLOG_Run_cleanup(void);

/**
 * @brief Get the total number of log files currently stored.
 *
 * @param count Output: total file count.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p count is NULL.
 * @retval -ENODEV if the module is not initialized.
 * @retval -EIO on directory scan failure.
 */
int SDLOG_Get_file_count(uint32_t *count);

/**
 * @brief Get the date of the oldest log file currently stored.
 *
 * @param date Output: oldest date.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p date is NULL.
 * @retval -ENODEV if the module is not initialized.
 * @retval -ENODATA if no log files exist yet.
 */
int SDLOG_Get_oldest_date(struct sdlog_date *date);

/**
 * @brief Get the date of the newest (most recently created) log
 * file currently stored.
 *
 * @param date Output: newest date.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p date is NULL.
 * @retval -ENODEV if the module is not initialized.
 * @retval -ENODATA if no log files exist yet.
 */
int SDLOG_Get_newest_date(struct sdlog_date *date);

/**
 * @brief Count how many log files fall within a date range.
 *
 * @param start Inclusive start date.
 * @param end   Inclusive end date. May equal @p start for a
 *              single-day count. If @p end is before @p start, the
 *              count is simply 0.
 * @param count Output: number of matching files.
 *
 * @retval 0 on success.
 * @retval -EINVAL if any pointer argument is NULL.
 * @retval -ENODEV if the module is not initialized.
 * @retval -EIO on directory scan failure.
 */
int SDLOG_Get_file_count_in_range(const struct sdlog_date *start,
				   const struct sdlog_date *end,
				   uint32_t *count);

/**
 * @brief List the log files that fall within a date range.
 *
 * Together with SDLOG_file_open()/SDLOG_file_read()/
 * SDLOG_file_close(), this is how a caller (e.g. TFTP) retrieves
 * "the log files available within a given date range": list first,
 * then open/read/close each file of interest by the name this
 * returns.
 *
 * @param start     Inclusive start date.
 * @param end       Inclusive end date. If before @p start, the
 *                  result is simply empty.
 * @param out       Caller-provided array to fill in.
 * @param max_files Capacity of @p out (entries).
 * @param out_count Output: number of entries written to @p out
 *                  (capped at @p max_files - if more files matched,
 *                  a warning is logged and the rest are omitted).
 *
 * @retval 0 on success (including an empty result).
 * @retval -EINVAL if any pointer argument is NULL.
 * @retval -ENODEV if the module is not initialized.
 * @retval -EIO on directory scan failure.
 */
int SDLOG_List_files_in_range(const struct sdlog_date *start,
			       const struct sdlog_date *end,
			       struct sdlog_file_info *out,
			       size_t max_files, size_t *out_count);

/**
 * @brief Open a specific stored log file for random (offset-based)
 * access - the mechanism TFTP uses to request "any specific log
 * file" independent of the Modem's sequential read position.
 *
 * Opening, reading, or closing a file this way NEVER touches the
 * Modem's sequential read cursor (modem_read_file/modem_read_entry)
 * in any way.
 *
 * @param filename Name of an existing log file (as returned by
 *                 SDLOG_list_files_in_range(), SDLOG_get_status(),
 *                 etc.) - just the name, not a full path, and it
 *                 must be one of this module's own log files (name
 *                 matching "<prefix>_<number>_<timestamp>.csv").
 * @param handle   Output: handle to use in subsequent calls.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p filename or @p handle is NULL, or filename
 *         doesn't look like one of this module's log files.
 * @retval -ENODEV if the module is not initialized.
 * @retval -EMFILE if too many random-access files are already open
 *         (see CONFIG_SDLOG_MAX_OPEN_RANDOM_FILES).
 * @retval -ENOENT if the file doesn't exist.
 * @retval -EIO on other file open failure.
 */
int SDLOG_File_open(const char *filename, sdlog_file_handle_t *handle);

/**
 * @brief Read raw bytes from a file opened with SDLOG_file_open(),
 * starting at an arbitrary byte offset.
 *
 * This is a plain byte/offset read of the file's actual on-disk
 * content (which is exactly what was written by SDLOG_write() calls,
 * newline-delimited) - suited to how a file-transfer protocol like
 * TFTP requests fixed-size blocks of a file rather than "entries".
 *
 * @param handle   Handle from SDLOG_file_open().
 * @param offset   Byte offset into the file to start reading from.
 * @param buf      Buffer to receive the bytes.
 * @param buf_size Capacity of @p buf; also the max number of bytes
 *                 read in this call (fewer are returned at EOF).
 * @param out_len  Output: number of bytes actually copied to @p buf
 *                 (0 at/past EOF, which is not itself an error).
 *
 * @retval 0 on success (including reading 0 bytes at EOF).
 * @retval -EINVAL if @p handle is invalid or @p buf/out_len is NULL.
 * @retval -EIO on file I/O error.
 */
int SDLOG_File_read(sdlog_file_handle_t handle, size_t offset,
		     uint8_t *buf, size_t buf_size, size_t *out_len);

/**
 * @brief Get the current size of a file opened with SDLOG_file_open().
 *
 * @param handle     Handle from SDLOG_file_open().
 * @param size_bytes Output: file size in bytes.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p handle is invalid or @p size_bytes is NULL.
 * @retval -EIO on failure.
 */
int SDLOG_File_size(sdlog_file_handle_t handle, size_t *size_bytes);

/**
 * @brief Close a handle opened with SDLOG_file_open().
 *
 * @param handle Handle from SDLOG_file_open().
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p handle is invalid.
 */
int SDLOG_File_close(sdlog_file_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* LOGGING_MODULE_H_ */