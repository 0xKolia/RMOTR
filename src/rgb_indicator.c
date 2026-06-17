/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * Per-layer RGB underglow indicator + ext-power management (ROTR Path C).
 *
 * Root cause of the dead-underglow bug (Stage 3 hardware debugging):
 * the WS2812 strip on the ROTR is powered through the gpio1.11 ext-power
 * rail. ZMK's underglow, with CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER, only
 * enables that rail inside zmk_rgb_underglow_on() -- but the ON_START path
 * just starts the animation tick and never calls on(), and the rail's
 * default-on state is overridden by a stale persisted "off" in NVS. Result:
 * the tick renders onto an unpowered strip and stays dark in every state.
 * (A direct-drive test that forced the rail on lit the strip perfectly,
 * proving the strip/SPI/pinctrl/power path itself is fine.)
 *
 * Fix: decouple the rail from underglow on/off (CONFIG_ZMK_RGB_UNDERGLOW_
 * EXT_POWER is removed from the board defconfig) and have this module -- the
 * owner of the RGB subsystem -- bring the rail up once at startup and keep
 * it powered. The RGB on/off toggle then only gates colour/animation; it
 * does NOT cut the rail ("just go dark"). This is what avoids the mid-flash
 * toggle edge case: the off-state flash writes the strip directly and never
 * touches ZMK's on/off state, so RGB_TOG always does the expected thing.
 *
 * Behaviour (toggle model):
 *   - "Current layer" = highest active layer EXCLUDING the held selector, so
 *     while the selector is held the colour previews the candidate landing
 *     layer, updating as the knob turns.
 *   - Underglow ON : the current layer's colour is lit continuously, at rest
 *     and at idle (CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n keeps it on),
 *     rendered by ZMK's tick from the colour we keep current via set_hsb.
 *   - Underglow OFF: dark at rest. The ONLY time it lights while off is while
 *     MIDDLE is held (selector active): the candidate layer's colour shows
 *     SOLID for the whole hold, updating as the knob turns, and goes dark on
 *     release. No flashing.
 *   - The held-selector illumination is the SAME steady colour in both toggle
 *     states; the only difference is the resting state (ON = lit, OFF = dark).
 *
 * All work runs on the system workqueue. Direct strip writes only happen
 * while underglow is off (ZMK's tick is stopped then), so the LED device is
 * never driven from two contexts at once.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <drivers/ext_power.h>

#include "rotr-ma730.h"

#define STRIP_NODE		DT_CHOSEN(zmk_underglow)
#define STRIP_NUM		DT_PROP(STRIP_NODE, chain_length)

struct rotr_color {
	uint16_t	h;	/* 0-360 */
	uint8_t		s;	/* 0-100 */
	uint8_t		b;	/* 0-100 */
};

/*
 * Per-layer colours. Layers 5-8 are pre-assigned ready for activation; the
 * selector layer is never shown directly. Pure red is reserved for future
 * alert use; blue is kept clear of cyan.
 */
static const struct rotr_color layer_colors[ROTR_NUM_LAYERS] = {
	[0] = {   0,   0,  80 },	/* white          */
	[1] = {  40, 100,  80 },	/* amber / yellow  */
	[2] = { 120, 100,  80 },	/* green           */
	[3] = { 180, 100,  80 },	/* cyan            */
	[4] = { 320, 100,  80 },	/* magenta         */
	[5] = {  25, 100,  80 },	/* orange (resv)   */
	[6] = { 225, 100,  80 },	/* blue   (resv)   */
	[7] = { 165, 100,  70 },	/* teal   (resv)   */
	[8] = { 280, 100,  80 },	/* purple (resv)   */
	[ROTR_SELECT_LAYER] = { 0, 0, 0 },	/* never shown */
};

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power =
    DEVICE_DT_GET_ANY(zmk_ext_power_generic);
#else
static const struct device *const ext_power = NULL;
#endif

static struct led_rgb		px[STRIP_NUM];

static struct k_work		apply_work;
static struct k_work_delayable	boot_work;

/* Integer HSB(0-360,0-100,0-100) -> led_rgb(0-255). */
static struct led_rgb
hsb_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
	uint32_t	vv = (uint32_t)v * 255 / 100;
	uint32_t	ss = (uint32_t)s * 255 / 100;
	uint8_t		r, g, b;

	if (ss == 0) {
		r = g = b = (uint8_t)vv;
	} else {
		uint8_t		region = (h % 360) / 60;
		uint32_t	rem = (uint32_t)((h % 360) % 60) * 255 / 60;
		uint32_t	p = vv * (255 - ss) / 255;
		uint32_t	q = vv * (255 - (ss * rem) / 255) / 255;
		uint32_t	t = vv * (255 - (ss * (255 - rem)) / 255) / 255;

		switch (region) {
		case 0:  r = vv; g = t;  b = p;  break;
		case 1:  r = q;  g = vv; b = p;  break;
		case 2:  r = p;  g = vv; b = t;  break;
		case 3:  r = p;  g = q;  b = vv; break;
		case 4:  r = t;  g = p;  b = vv; break;
		default: r = vv; g = p;  b = q;  break;
		}
	}

	return (struct led_rgb){ .r = r, .g = g, .b = b };
}

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
strip_fill(struct led_rgb c)
{
	for (int i = 0; i < STRIP_NUM; i++) {
		px[i] = c;
	}
	(void)led_strip_update_rgb(strip, px, STRIP_NUM);
}

/* Is the momentary selector layer currently held? */
static bool
selector_held(void)
{
	return zmk_keymap_layer_active((zmk_keymap_layer_id_t)ROTR_SELECT_LAYER);
}

/*
 * Show the current (or, while the selector is held, candidate) layer colour
 * according to the toggle state:
 *   - ON : keep ZMK's colour current; its tick renders it solid and lit at
 *     rest, so there is nothing to draw directly.
 *   - OFF: drive the strip directly -- the candidate colour SOLID while the
 *     selector is held, dark otherwise. Same steady illumination as ON; only
 *     the resting state differs (ON = lit, OFF = dark).
 *
 * Direct writes happen only while underglow is off, when ZMK's tick is
 * stopped and cannot fight us. set_hsb never changes the on/off state, so it
 * cannot cause the toggle edge case.
 */
static void
apply_color(void)
{
	struct rotr_color	c = layer_colors[current_layer()];
	bool			on = false;

	zmk_rgb_underglow_get_state(&on);

	zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){ .h = c.h,
	    .s = c.s, .b = c.b });

	if (on) {
		return;		/* ZMK's tick renders the lit colour */
	}

	if (selector_held()) {
		strip_fill(hsb_to_rgb(c.h, c.s, c.b));
	} else {
		strip_fill((struct led_rgb){ .r = 0, .g = 0, .b = 0 });
	}
}

static void
apply_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	apply_color();
}

static void
boot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/*
	 * Bring up the LED power rail and keep it on. Done after settings have
	 * loaded so this overrides any stale persisted "off" and re-saves the
	 * enabled state. The rail is intentionally decoupled from RGB on/off.
	 */
	if (ext_power != NULL && device_is_ready(ext_power)) {
		int rc = ext_power_enable(ext_power);

		if (rc != 0) {
			(void)rc;	/* best effort; nothing else to do */
		}
	}

	/* Settle the boot colour. */
	apply_color();
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
	k_work_init_delayable(&boot_work, boot_work_handler);

	/* Run once underglow, ext-power and settings are all ready. */
	k_work_schedule(&boot_work, K_MSEC(1500));
	return 0;
}

SYS_INIT(rgb_indicator_init, APPLICATION, 99);
