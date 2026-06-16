# Copyright (c) 2026 Carl Henriksson
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ROTR_RGB_INDICATOR rgb_indicator.c)

# Uses ZMK keymap / underglow APIs (private to ZMK's "app" target) and the
# shared per-layer constants in the board directory's rotr-ma730.h.
zephyr_library_include_directories_ifdef(CONFIG_ROTR_RGB_INDICATOR
    ${APPLICATION_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/polarityworks/rotr)
