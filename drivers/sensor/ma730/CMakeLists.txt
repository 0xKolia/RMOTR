# Copyright (c) 2026 Carl Henriksson
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MA730_INPUT ma730_input.c)

# The driver is layer-aware (Stage 2): it calls into ZMK's keymap and
# keycode-event APIs, whose headers live in the ZMK application's private
# include dir. ZMK adds that dir only to its own "app" target, so expose it
# to this out-of-tree library explicitly.
zephyr_library_include_directories_ifdef(CONFIG_MA730_INPUT
    ${APPLICATION_SOURCE_DIR}/include)
