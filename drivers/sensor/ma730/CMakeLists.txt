# Copyright (c) 2026 Carl Henriksson
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MA730_INPUT ma730_input.c)
