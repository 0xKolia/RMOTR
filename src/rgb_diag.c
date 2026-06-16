/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * THROWAWAY low-level LED-strip diagnostic (gated by CONFIG_ROTR_RGB_DIAG).
 *
 * Bypasses all ZMK underglow code and drives the WS2812 strip directly via
 * the Zephyr LED strip API, logging device_is_ready() and every
 * led_strip_update_rgb() return code over USB CDC (CONFIG_ZMK_USB_LOGGING).
 * It also explicitly enables the ext-power rail so power is not a variable.
 *
 * If the strip cycles red/green/blue -> the strip/SPI/pinctrl path works in
 * our build and the fault is in ZMK's underglow layer above it.
 * If it stays dark -> the log tells us whether the device was not ready or
 * whether update returned an error (SPI/DMA), isolating strip/SPI init.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

LOG_MODULE_REGISTER(rotr_rgb_diag, LOG_LEVEL_INF);

#define STRIP_NODE	DT_CHOSEN(zmk_underglow)
#define STRIP_NUM	DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM];

static void
rotr_rgb_diag_thread(void *a, void *b, void *c)
{
	int	phase = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("=== ROTR RGB DIAG start: strip='%s' chain-length=%d ===",
	    strip->name, STRIP_NUM);

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
	{
		const struct device *ext =
		    DEVICE_DT_GET_ANY(zmk_ext_power_generic);

		if (ext != NULL && device_is_ready(ext)) {
			int rc = ext_power_enable(ext);

			LOG_INF("ext_power '%s' enable rc=%d", ext->name, rc);
		} else {
			LOG_WRN("ext_power device missing or not ready");
		}
	}
#else
	LOG_INF("CONFIG_ZMK_EXT_POWER is disabled in this build");
#endif

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip '%s' device_is_ready() = FALSE -- SPI/strip "
		    "init failed", strip->name);
	} else {
		LOG_INF("LED strip '%s' device_is_ready() = TRUE", strip->name);
	}

	for (;;) {
		struct led_rgb col;

		switch (phase % 3) {
		case 0:
			col = (struct led_rgb){ .r = 50, .g = 0, .b = 0 };
			break;
		case 1:
			col = (struct led_rgb){ .r = 0, .g = 50, .b = 0 };
			break;
		default:
			col = (struct led_rgb){ .r = 0, .g = 0, .b = 50 };
			break;
		}

		for (int i = 0; i < STRIP_NUM; i++) {
			pixels[i] = col;
		}

		int rc = led_strip_update_rgb(strip, pixels, STRIP_NUM);

		LOG_INF("led_strip_update_rgb rc=%d (phase %d: %s)", rc,
		    phase % 3,
		    (phase % 3 == 0) ? "red" : (phase % 3 == 1) ? "green"
								: "blue");
		phase++;
		k_msleep(1000);
	}
}

K_THREAD_DEFINE(rotr_rgb_diag_tid, 1024, rotr_rgb_diag_thread,
    NULL, NULL, NULL, 7, 0, 2500);
