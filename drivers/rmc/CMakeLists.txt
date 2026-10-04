# Copyright (c) 2026 Calixto System Pvt Ltd
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(.)

zephyr_library_sources_ifdef(CONFIG_RMC rmc.c)

add_subdirectory_ifdef(CONFIG_RMC_WIFI_BACKEND backends/wifi)
add_subdirectory_ifdef(CONFIG_RMC_SUBGHZ_BACKEND backends/subghz)
add_subdirectory_ifdef(CONFIG_RMC_SERIAL_BACKEND backends/serial)
