/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * Shared constants for the ROTR layer-aware encoder, included by both the
 * board devicetree (rotr_nrf52840_zmk.dts) and the C that interprets it
 * (drivers/sensor/ma730/ma730_input.c, src/rgb_indicator.c).
 *
 * The per-layer knob MODE lives in the ma730 node's `layer-modes` array
 * using these values, so activating a reserved layer is a devicetree edit,
 * never a C change.
 */

#ifndef ROTR_MA730_H
#define ROTR_MA730_H

/* Knob behaviour per layer (values stored in the `layer-modes` DT array). */
#define MA730_MODE_INACTIVE	0	/* no output                     */
#define MA730_MODE_KEY		1	/* tap layer-keycodes-cw / -ccw  */
#define MA730_MODE_SCROLL_V	2	/* smooth INPUT_REL_WHEEL         */
#define MA730_MODE_SCROLL_H	3	/* smooth INPUT_REL_HWHEEL        */
#define MA730_MODE_SELECT	4	/* hold-middle layer selector     */

/* Total keymap layers (active 0-4, reserved 5-8, selector 9). */
#define ROTR_NUM_LAYERS		10

/* The held selector layer index. Its knob mode must be MA730_MODE_SELECT. */
#define ROTR_SELECT_LAYER	9

#endif /* ROTR_MA730_H */
