/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * Per-layer RGB underglow indicator (ROTR "Path C", Stage 3).
 *
 * Listens for ZMK layer-state changes and shows the active layer's colour
 * on the underglow:
 *   - When the underglow is ON: the layer colour is shown persistently and
 *     updates on every change.
 *   - When the underglow is OFF (user toggled it off with HOLD MIDDLE +
 *     TAP RIGHT): it STAYS off, except a layer change briefly illuminates
 *     the new layer's colour and fades back to off -- a momentary indicator.
 *
 * The "current layer" for colour purposes is the highest active layer
 * EXCLUDING the held selector layer, so while selecting (MIDDLE held + turn)
 * the underglow previews the candidate landing layer.
 *
 * All underglow work runs on the system workqueue (the listener only submits
 * work), so the flash state needs no locking. ZMK's on()/off()/set_hsb()
 * persist via a debounced save, so a brief flash nets out to the user's
 * actual on/off preference.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>

#include "rotr-ma730.h"

/* Total off-state flash duration (ms). Tweak to taste. */
#define ROTR_RGB_FLASH_MS	600
/* Number of fade steps over that duration. */
#define ROTR_RGB_FADE_STEPS	8

/*
 * Per-layer colours (HSB: hue 0-360, sat 0-100, brightness 0-100). Layers
 * 5-8 are pre-assigned ready for activation; the selector layer is never
 * shown directly. Pure red is reserved for future alert use; blue is kept
 * clear of cyan.
 */
static const struct zmk_led_hsb layer_colors[ROTR_NUM_LAYERS] = {
	[0] = {   0,   0,  80 },	/* white          */
	[1] = {  40, 100,  80 },	/* amber / yellow  */
	[2] = { 120, 100,  80 },	/* green           */
	[3] = { 180, 100,  80 },	/* cyan            */
	[4] = { 320,  50,  90 },	/* pastel magenta  */
	[5] = {  25, 100,  80 },	/* orange (resv)   */
	[6] = { 225, 100,  80 },	/* blue   (resv)   */
	[7] = { 165, 100,  70 },	/* teal   (resv)   */
	[8] = { 280, 100,  80 },	/* purple (resv)   */
	[ROTR_SELECT_LAYER] = { 0, 0, 0 },	/* never shown */
};

static struct k_work		apply_work;
static struct k_work_delayable	fade_work;
static struct k_work_delayable	boot_work;

static bool			flashing;
static int			fade_step;
static struct zmk_led_hsb	fade_color;

/* Highest active layer below the selector = the current landing layer. */
static uint8_t
current_layer(void)
{
	for (int i = ROTR_NUM_LAYERS - 1; i >= 0; i--) {
		if (i == ROTR_SELECT_LAYER) {
			continue;
		}
		if (zmk_keymap_layer_active((zmk_keymap_layer_id_t)i)) {
			return (uint8_t)i;
		}
	}
	return 0;
}

static void
fade_work_handler(struct k_work *work)
{
	uint8_t	b;

	ARG_UNUSED(work);

	fade_step++;
	if (fade_step < ROTR_RGB_FADE_STEPS) {
		b = (uint8_t)((int)fade_color.b *
		    (ROTR_RGB_FADE_STEPS - fade_step) / ROTR_RGB_FADE_STEPS);
		zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){
		    .h = fade_color.h, .s = fade_color.s, .b = b });
		k_work_schedule(&fade_work,
		    K_MSEC(ROTR_RGB_FLASH_MS / ROTR_RGB_FADE_STEPS));
		return;
	}

	/*
	 * Restore the canonical full-brightness colour before turning off, so
	 * the persisted colour stays correct for the next toggle-on, then off.
	 */
	zmk_rgb_underglow_set_hsb(fade_color);
	zmk_rgb_underglow_off();
	flashing = false;
}

static void
start_flash(struct zmk_led_hsb color)
{
	k_work_cancel_delayable(&fade_work);

	fade_color = color;
	flashing = true;
	fade_step = 0;

	zmk_rgb_underglow_set_hsb(color);
	zmk_rgb_underglow_on();
	k_work_schedule(&fade_work,
	    K_MSEC(ROTR_RGB_FLASH_MS / ROTR_RGB_FADE_STEPS));
}

static void
apply_color(bool flash_if_off)
{
	struct zmk_led_hsb	color = layer_colors[current_layer()];
	bool			on = false;
	bool			user_on;

	zmk_rgb_underglow_get_state(&on);
	/* Mid-flash, the underlying user preference is "off". */
	user_on = flashing ? false : on;

	if (user_on) {
		zmk_rgb_underglow_set_hsb(color);
	} else if (flash_if_off) {
		start_flash(color);
	} else {
		/* Boot/idle: store the colour without lighting up. */
		zmk_rgb_underglow_set_hsb(color);
	}
}

static void
apply_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	apply_color(true);
}

static void
boot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	apply_color(false);
}

static int
rgb_indicator_listener(const zmk_event_t *eh)
{
	ARG_UNUSED(eh);
	k_work_submit(&apply_work);
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rotr_rgb_indicator, rgb_indicator_listener);
ZMK_SUBSCRIPTION(rotr_rgb_indicator, zmk_layer_state_changed);

static int
rgb_indicator_init(void)
{
	k_work_init(&apply_work, apply_work_handler);
	k_work_init_delayable(&fade_work, fade_work_handler);
	k_work_init_delayable(&boot_work, boot_work_handler);

	/* Settle the boot colour once underglow + settings are ready. */
	k_work_schedule(&boot_work, K_MSEC(1500));
	return 0;
}

SYS_INIT(rgb_indicator_init, APPLICATION, 99);
