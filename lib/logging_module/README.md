# SD Card Logging Module (`lib/logging_module`)

Reusable Zephyr library implementing the requirements in `Logging-Req.xlsx`:
SD card / FAT32 detection, mounting and formatting, log file creation and
rotation, buffered writes, sequential reads, free-space based cleanup, and
a persisted config file so logging state survives a reset or power cycle.

The application talks to this module **only** through
`include/logging_module/logging_module.h`. Everything else under
`lib/logging_module` (the `sdlog_*.c` files and `sdlog_internal.h`) is
private implementation detail.

## Files

| File                  | Responsibility                                              |
|------------------------|--------------------------------------------------------------|
| `sdlog_module.c`      | Public API glue: locking, argument validation                |
| `sdlog_core.c`        | Disk detection, FAT32 mount, RTC timestamps, filename helpers, directory scan, file-number allocation |
| `sdlog_config.c`      | Config file load/create/save, CRC validation, rebuild-from-disk fallback |
| `sdlog_write.c`       | Active file creation, append writes, rotation at N entries   |
| `sdlog_read.c`        | Sequential unread-entry reads, automatic file hand-off       |
| `sdlog_cleanup.c`     | Free-space check, oldest-file identification and deletion    |
| `sdlog_internal.h`    | Private struct/function declarations shared by the .c files  |

## Integrating into an app (e.g. `apps/fs_sample`)

Already wired up in `apps/fs_sample` as a reference:

```cmake
# CMakeLists.txt
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/logging_module
                  ${CMAKE_CURRENT_BINARY_DIR}/logging_module)
```

```kconfig
# Kconfig
rsource "../../lib/logging_module/Kconfig"
```

```conf
# prj.conf
CONFIG_SDCARD_LOGGING=y
```

To use it from a *different* app in `apps/`, copy those three snippets
(adjusting relative paths) into that app's build files.

### Example usage from application code

```c
#include <logging_module/logging_module.h>

int ret = sdlog_init();
if (ret != 0) {
        /* handle: no card, mount failure, RTC not ready, ... */
}

uint8_t sample[] = { 0x01, 0x02, 0x03 };
ret = sdlog_write(sample, sizeof(sample));

uint8_t rdbuf[CONFIG_SDLOG_MAX_ENTRY_SIZE];
size_t rdlen;
ret = sdlog_read_next(rdbuf, sizeof(rdbuf), &rdlen);
if (ret == 0) {
        /* rdbuf[0..rdlen) is the next unread entry */
} else if (ret == -ENODATA) {
        /* nothing new to read yet */
}

struct sdlog_status st;
sdlog_get_status(&st);
```

## Prerequisites you must provide

1. **An RTC device with a devicetree `rtc` alias.** File names embed a
   real timestamp (`LOG-GEN-02/03`), and there is no RTC configured in
   the `fs_sample` app you provided. Add to your board overlay:

   ```dts
   / {
           aliases {
                   rtc = &your_rtc_node;
           };
   };
   ```

   Plus enable that peripheral's own driver Kconfig (board/SoC
   specific — for an i.MX RT1176 this is typically the NXP SNVS RTC
   driver). `CONFIG_RTC=y` itself is already pulled in automatically
   by `CONFIG_SDCARD_LOGGING`. **If no `rtc` alias exists, the build
   fails at compile time** with a clear `#error` from `sdlog_core.c`
   rather than silently producing garbage timestamps.

2. **`CONFIG_SDLOG_DISK_NAME`** must match the disk name your board's
   SD/MMC driver registers (defaults to `"SD"`, matching your existing
   `fs_sample` sample).

3. **`CONFIG_SDLOG_MOUNT_POINT`** must match a FatFs volume string your
   `ffconf.h` accepts (defaults to `"/SD:"`, matching your existing
   sample).

## Design decisions made (per your clarifications)

- **Timestamps**: real RTC time, `YYYYMMDDHHMMSS`, via the standard
  Zephyr RTC API and a portable `DT_ALIAS(rtc)` device.
- **Log entry format**: variable-length raw byte array. On disk, each
  entry is stored as `[4-byte little-endian length][payload bytes]` so
  arbitrary binary content (not just text) round-trips exactly.
- **Cleanup priority**: SD card space always wins. At
  `CONFIG_SDLOG_CLEANUP_THRESHOLD_PERCENT`, the oldest file is deleted
  even if it still holds entries that haven't been read yet. If the
  deleted file was the read cursor's position, the read cursor is
  automatically advanced to the new oldest file so subsequent reads
  don't fail. The *active write* file (`LOG-CLR-06`) is never deleted
  — if the only file left is the active file and the card is still
  full, `sdlog_write()` returns `-ENOSPC`.
- **Config file**: fixed name (`ws-config.dat` by default), overwritten
  in place, with a magic value + version + CRC32 so a corrupted config
  file is detected rather than trusted. On first boot, or if
  corruption is detected, the module rebuilds a best-effort config by
  scanning the card for existing log files (it can recover which
  files exist and which is newest/oldest, but **not** the exact
  read/write entry-position counters, since that's precisely the
  information a corrupted config file lost — the read/write cursors
  for the affected file restart at entry 0 in that case).
- **File numbering**: `<File Number>` increments from 0, wraps at
  `CONFIG_SDLOG_MAX_FILE_NUMBER` (default 9999), and reuses a number
  only once the file that previously used it has been deleted.

## Known limitations / things to be aware of

- **File ordering after a number wraparound**: log files are ordered
  purely by the numeric `<File Number>` in their name (Zephyr's FAT
  driver does not expose file modification timestamps via
  `fs_dirent`, so we can't order by mtime). Within one "lap" of the
  wraparound counter this is correct chronological order. If the
  counter wraps while old files from the previous lap are still on
  disk (i.e. more than `CONFIG_SDLOG_MAX_FILE_NUMBER + 1` files exist
  at once — unlikely given cleanup keeps usage under the threshold),
  ordering across the wrap boundary is not guaranteed. Increase
  `CONFIG_SDLOG_MAX_FILE_NUMBER` if your application logs enough
  volume that this could realistically happen.
- **Rebuilt config after corruption** cannot recover exact entry
  counters for the affected file (see above) — this is an inherent
  limit of what a corrupted config file leaves behind, not something
  the module can work around.
- **`CONFIG_SDLOG_MAX_FILES_SCAN`** (default 64) bounds a static array
  used for directory scans. If more log files than this can exist on
  the card simultaneously, raise it.
- This module does **not** modify `apps/fs_sample/src/main.c` — that
  sample demonstrates raw `fs_mount`/`disk_access` usage independently
  of this library. Wire up calls to `sdlog_init()` / `sdlog_write()` /
  `sdlog_read_next()` from your own application code as shown above.

## Public API

See `include/logging_module/logging_module.h` for the full, documented
API surface: `sdlog_init()`, `sdlog_deinit()`, `sdlog_write()`,
`sdlog_read_next()`, `sdlog_get_status()`, `sdlog_run_cleanup()`. All
functions are internally mutex-protected and safe to call from
multiple threads.

## Kconfig options

See `lib/logging_module/Kconfig` for the full list (mount point, disk
name, file naming, rotation size, cleanup threshold, file-number wrap,
max entry size, directory-scan array size, plus the standard
`CONFIG_SDLOG_LOG_LEVEL` module log level).
