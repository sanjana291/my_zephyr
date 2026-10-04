# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(fsk_drivers)

zephyr_library_sources_ifdef(CONFIG_FSK_SX1278 sx127x_fsk.c)

zephyr_include_directories(.)