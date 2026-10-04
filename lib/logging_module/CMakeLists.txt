# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SDCARD_LOGGING)

zephyr_library()

zephyr_library_sources(
  sdlog_module.c
  sdlog_core.c
  sdlog_config.c
  sdlog_write.c
  sdlog_read.c
  sdlog_cleanup.c
  sdlog_query.c
  sdlog_random.c
)

zephyr_library_sources_ifdef(CONFIG_SDLOG_DEBUG_HELPERS sdlog_debug.c)

# TCP server + TFTP client (Logging Workflow over Ethernet).
# Both files are compiled together: sdlog_tcp.c handles the TCP accept
# loop, Security Sequence validation, and MTP dispatch; sdlog_tftp.c
# handles the TFTP PUT and sends the MTP 12 Log Read Result on
# completion or failure.
zephyr_library_sources_ifdef(CONFIG_SDLOG_TCP_SERVER sdlog_tcp.c)
zephyr_library_sources_ifdef(CONFIG_SDLOG_TCP_SERVER sdlog_tftp.c)

# Public header, reachable by any app/library in the build as
# <logging_module/logging_module.h>. Implementation-private headers
# (sdlog_internal.h, sdlog_tcp.h) are intentionally NOT exposed here.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)

# ff.h - and the compile definitions it needs (e.g.
# FS_FATFS_WINDOW_ALIGNMENT, derived from Kconfig by the fatfs
# module) - are carried by the ELMFAT interface library that
# Zephyr's own subsys/fs links against:
#   target_link_libraries_ifdef(CONFIG_FAT_FILESYSTEM_ELM FS INTERFACE ELMFAT)
# in zephyr/subsys/fs/CMakeLists.txt. None of that is part of the
# global zephyr_interface, so an out-of-tree library must link
# against ELMFAT directly to correctly inherit both the include path
# and every required -D flag, rather than reproducing them by hand
# (manually adding just the include dir is not enough - ff.h also
# needs FS_FATFS_WINDOW_ALIGNMENT and friends defined).
if(CONFIG_FAT_FILESYSTEM_ELM)
  zephyr_library_link_libraries(ELMFAT)
endif()

endif() # CONFIG_SDCARD_LOGGING