# Copyright (c) 2026 Carl Henriksson
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MA730_INPUT ma730_input.c)

# The driver is layer-aware: it calls into ZMK's keymap and keycode-event
# APIs (private to ZMK's "app" target) and shares the per-layer mode
# constants in the board directory's rotr-ma730.h. Expose both to this
# out-of-tree library.
zephyr_library_include_directories_ifdef(CONFIG_MA730_INPUT
    ${APPLICATION_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/polarityworks/rotr)
