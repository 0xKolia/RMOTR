/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * ROTR connection-aware power policy.
 *
 * Rules:
 * - USB HID ready always takes output priority over BLE.
 * - USB HID idle powers off after ROTR_POWER_POLICY_USB_TIMEOUT_MS.
 * - BLE or disconnected idle powers off after ROTR_POWER_POLICY_BLE_TIMEOUT_MS.
 * - Losing USB while not BLE-connected powers off immediately.
 * - Losing BLE while USB HID is not ready powers off immediately.
 *
 * Button presses can wake from power-off through the kscan wakeup-source.
 * The MA730 knob is SPI-polled, so knob movement alone is not a wake source.
 */

#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/pm.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct k_work_delayable policy_work;
static struct k_work_delayable poweroff_work;
static int64_t last_activity_ms;
static bool was_usb_powered;
static bool was_usb_hid_ready;
static bool was_ble_connected;
static bool powering_off;

static bool usb_powered(void)
{
	return zmk_usb_get_conn_state() != ZMK_USB_CONN_NONE;
}

static bool usb_hid_ready(void)
{
	return zmk_usb_is_hid_ready();
}

static bool ble_connected(void)
{
	return zmk_ble_active_profile_is_connected();
}

static int32_t active_timeout_ms(void)
{
	if (usb_hid_ready()) {
		return CONFIG_ROTR_POWER_POLICY_USB_TIMEOUT_MS;
	}

	return CONFIG_ROTR_POWER_POLICY_BLE_TIMEOUT_MS;
}

static void prefer_usb_when_ready(void)
{
	if (usb_hid_ready() &&
	    zmk_endpoint_get_preferred_transport() != ZMK_TRANSPORT_USB) {
		zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
	}
}

static void schedule_policy(void)
{
	if (powering_off) {
		return;
	}

	int32_t timeout = active_timeout_ms();
	int64_t elapsed = k_uptime_get() - last_activity_ms;
	int32_t remaining = timeout - (int32_t)elapsed;

	k_work_reschedule(&policy_work,
	    K_MSEC(MAX(remaining, CONFIG_ROTR_POWER_POLICY_BLE_TIMEOUT_MS / 60)));
}

static void poweroff_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	zmk_endpoint_clear_reports();
	k_sleep(K_MSEC(100));

	if (zmk_pm_suspend_devices() < 0) {
		zmk_pm_resume_devices();
		powering_off = false;
		schedule_policy();
		return;
	}

	sys_poweroff();
}

static void request_poweroff(void)
{
	if (powering_off) {
		return;
	}

	powering_off = true;
	k_work_cancel_delayable(&policy_work);
	k_work_reschedule(&poweroff_work, K_NO_WAIT);
}

static void policy_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	prefer_usb_when_ready();

	if (k_uptime_get() - last_activity_ms >= active_timeout_ms()) {
		request_poweroff();
		return;
	}

	schedule_policy();
}

static void note_activity(void)
{
	last_activity_ms = k_uptime_get();
	schedule_policy();
}

static void handle_connection_change(void)
{
	bool now_usb_powered = usb_powered();
	bool now_usb_hid_ready = usb_hid_ready();
	bool now_ble_connected = ble_connected();

	prefer_usb_when_ready();

	if (was_usb_powered && !now_usb_powered && !now_ble_connected) {
		request_poweroff();
		return;
	}

	if (was_ble_connected && !now_ble_connected && !now_usb_hid_ready) {
		request_poweroff();
		return;
	}

	was_usb_powered = now_usb_powered;
	was_usb_hid_ready = now_usb_hid_ready;
	was_ble_connected = now_ble_connected;
	schedule_policy();
}

static int power_policy_listener(const zmk_event_t *eh)
{
	if (as_zmk_usb_conn_state_changed(eh) ||
	    as_zmk_ble_active_profile_changed(eh)) {
		handle_connection_change();
		return ZMK_EV_EVENT_BUBBLE;
	}

	if (as_zmk_position_state_changed(eh) ||
	    as_zmk_keycode_state_changed(eh) ||
	    as_zmk_layer_state_changed(eh)) {
		note_activity();
		return ZMK_EV_EVENT_BUBBLE;
	}

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rotr_power_policy, power_policy_listener);
ZMK_SUBSCRIPTION(rotr_power_policy, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(rotr_power_policy, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(rotr_power_policy, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rotr_power_policy, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(rotr_power_policy, zmk_layer_state_changed);

static void input_activity_listener(struct input_event *ev, void *user_data)
{
	ARG_UNUSED(ev);
	ARG_UNUSED(user_data);

	note_activity();
}

INPUT_CALLBACK_DEFINE(NULL, input_activity_listener, NULL);

static int power_policy_init(void)
{
	last_activity_ms = k_uptime_get();
	was_usb_powered = usb_powered();
	was_usb_hid_ready = usb_hid_ready();
	was_ble_connected = ble_connected();

	k_work_init_delayable(&policy_work, policy_work_handler);
	k_work_init_delayable(&poweroff_work, poweroff_work_handler);

	prefer_usb_when_ready();
	schedule_policy();

	return 0;
}

SYS_INIT(power_policy_init, APPLICATION, 99);
