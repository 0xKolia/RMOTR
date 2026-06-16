/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
 *
 * Layer-aware dual-mode MA730 encoder driver (ROTR "Path C", Stage 2).
 *
 * MOTR's original driver emitted INPUT_REL_WHEEL unconditionally. This
 * extension keeps MOTR's absolute-angle maths (wrapped delta + reversal
 * clearing) untouched and adds a per-layer mode table: on each poll the
 * driver reads the highest active ZMK keymap layer and routes rotation
 * into the correct "world":
 *
 *   layer 0 default    -> inactive (no output)
 *   layer 1 brightness -> keyboard: tap F13 (CW) / F14 (CCW)
 *   layer 2 volume     -> keyboard: tap Vol Up (CW) / Vol Down (CCW)
 *   layer 3 scroll-v   -> pointing: smooth INPUT_REL_WHEEL  (vertical)
 *   layer 4 scroll-h   -> pointing: smooth INPUT_REL_HWHEEL (horizontal)
 *
 * Keyboard-world taps are produced by raising ZMK keycode events directly
 * (raise_zmk_keycode_state_changed_from_encoded). A small poll-driven FIFO
 * spaces each press/release one poll apart so the host's HID polling never
 * coalesces a tap away. Scroll-world events are scaled down by the board's
 * zip_scroll_scaler and metered with track-remainders for smoothness; this
 * driver additionally applies optional velocity-based acceleration.
 *
 * The rotation accumulator is reset on every layer change so a turn that
 * straddles a mode boundary cannot leak a phantom tick into the new world.
 */

#define DT_DRV_COMPAT polarityworks_ma730_input

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <stdlib.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/keymap.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_REGISTER(ma730_input, CONFIG_INPUT_LOG_LEVEL);

#define MA730_COUNTS_PER_REV	65536

/* Initialise after SPI bus (CONFIG_SPI_INIT_PRIORITY = 70). */
#define MA730_INIT_PRIORITY	90

/* Max key taps emitted from a single poll, to bound a fast spin's burst. */
#define MA730_MAX_TAPS_PER_POLL	4

/* Pending press/release events drain one per poll (see ma730_service_keys). */
#define MA730_KEY_QUEUE_LEN	32

/* How the knob behaves on a given layer. */
enum ma730_world {
	MA730_INACTIVE = 0,
	MA730_KEY,
	MA730_SCROLL,
};

/*
 * Per-layer mode table (indexed by ZMK layer index). Layers not listed
 * here, or any index past the table, are treated as inactive.
 *
 * Direction convention: a positive wrapped delta is clockwise (CW).
 *   - KEY layers tap cw_key on CW, ccw_key on CCW.
 *   - SCROLL layers emit rel_code; sign handling lives in ma730_scroll().
 */
struct ma730_layer_mode {
	enum ma730_world	world;
	uint32_t		cw_key;		/* encoded keycode, KEY only */
	uint32_t		ccw_key;	/* encoded keycode, KEY only */
	uint16_t		rel_code;	/* INPUT_REL_*, SCROLL only */
};

static const struct ma730_layer_mode ma730_layer_modes[] = {
	[0] = { .world = MA730_INACTIVE },
	[1] = { .world = MA730_KEY, .cw_key = F13,      .ccw_key = F14 },
	[2] = { .world = MA730_KEY, .cw_key = C_VOL_UP, .ccw_key = C_VOL_DN },
	[3] = { .world = MA730_SCROLL, .rel_code = INPUT_REL_WHEEL },
	[4] = { .world = MA730_SCROLL, .rel_code = INPUT_REL_HWHEEL },
};

struct ma730_config {
	struct spi_dt_spec	 spi;
	int			 reversal_threshold;
	int			 scroll_counts_per_unit;
	int			 key_counts_per_detent;
	int			 scroll_accel_gain;
	int			 scroll_accel_max;
	bool			 invert_scroll;
	bool			 invert_hscroll;
};

struct ma730_key_event {
	uint32_t		keycode;
	bool			press;
};

struct ma730_data {
	const struct device	*dev;
	uint16_t		 last_angle;
	int			 accumulated_counts;
	uint8_t			 last_layer;

	/* Poll-driven press/release FIFO for keyboard-world taps. */
	struct ma730_key_event	 key_queue[MA730_KEY_QUEUE_LEN];
	uint8_t			 key_head;
	uint8_t			 key_count;

	struct k_timer		 timer;
#if defined(CONFIG_MA730_INPUT_TRIGGER_GLOBAL_THREAD)
	struct k_work		 work;
#elif defined(CONFIG_MA730_INPUT_TRIGGER_OWN_THREAD)
	K_THREAD_STACK_MEMBER(stack, CONFIG_MA730_INPUT_THREAD_STACK_SIZE);
	struct k_thread		 thread;
	struct k_sem		 sem;
#endif
};

static const struct ma730_layer_mode *
ma730_mode_for_layer(uint8_t layer)
{
	static const struct ma730_layer_mode inactive = {
		.world = MA730_INACTIVE,
	};

	if (layer >= ARRAY_SIZE(ma730_layer_modes)) {
		return &inactive;
	}
	return &ma730_layer_modes[layer];
}

/*
 * The MA730 returns the current angle on any SPI transaction; two zero
 * bytes is a NOP command. Angle is MSB-first, SPI mode 3 (CPOL=1, CPHA=1).
 */
static int
ma730_read_angle(const struct device *dev, uint16_t *out)
{
	const struct ma730_config	*cfg = dev->config;
	uint8_t				 tx[2] = {0};
	uint8_t				 rx[2] = {0};
	struct spi_buf			 tx_buf = {.buf = tx, .len = sizeof(tx)};
	struct spi_buf_set		 tx_set = {.buffers = &tx_buf, .count = 1};
	struct spi_buf			 rx_buf = {.buf = rx, .len = sizeof(rx)};
	struct spi_buf_set		 rx_set = {.buffers = &rx_buf, .count = 1};
	int				 err;

	err = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);
	if (err != 0) {
		return err;
	}

	*out = sys_get_be16(rx);
	return 0;
}

/* Shortest signed delta across the 16-bit wrap boundary. */
static int
ma730_compute_delta(uint16_t last_angle, uint16_t raw)
{
	int	delta;

	delta = (int)raw - (int)last_angle;
	if (delta > MA730_COUNTS_PER_REV / 2) {
		delta -= MA730_COUNTS_PER_REV;
	}
	if (delta < -(MA730_COUNTS_PER_REV / 2)) {
		delta += MA730_COUNTS_PER_REV;
	}

	return delta;
}

/* Enqueue one press/release. Dropped silently if the FIFO is full. */
static void
ma730_key_enqueue(struct ma730_data *data, uint32_t keycode, bool press)
{
	uint8_t	idx;

	if (data->key_count >= MA730_KEY_QUEUE_LEN) {
		LOG_WRN("key queue full, dropping event");
		return;
	}

	idx = (data->key_head + data->key_count) % MA730_KEY_QUEUE_LEN;
	data->key_queue[idx].keycode = keycode;
	data->key_queue[idx].press = press;
	data->key_count++;
}

/* Queue a full tap (press now, release on the following poll). */
static void
ma730_key_tap(struct ma730_data *data, uint32_t keycode)
{
	ma730_key_enqueue(data, keycode, true);
	ma730_key_enqueue(data, keycode, false);
}

/*
 * Drain at most one queued press/release per poll. One-per-poll spacing
 * (~8 ms) guarantees the host sees each transition on a separate HID
 * report, so taps are never coalesced into nothing.
 */
static void
ma730_service_keys(struct ma730_data *data)
{
	struct ma730_key_event	ev;

	if (data->key_count == 0) {
		return;
	}

	ev = data->key_queue[data->key_head];
	data->key_head = (data->key_head + 1) % MA730_KEY_QUEUE_LEN;
	data->key_count--;

	raise_zmk_keycode_state_changed_from_encoded(ev.keycode, ev.press,
	    k_uptime_get());
}

/* Keyboard world: convert accumulated counts into discrete key taps. */
static void
ma730_keys(const struct device *dev, const struct ma730_layer_mode *mode)
{
	const struct ma730_config	*cfg = dev->config;
	struct ma730_data		*data = dev->data;
	uint32_t			 keycode;
	int				 detents;
	int				 n;

	detents = data->accumulated_counts / cfg->key_counts_per_detent;
	if (detents == 0) {
		return;
	}

	data->accumulated_counts -= detents * cfg->key_counts_per_detent;

	keycode = (detents > 0) ? mode->cw_key : mode->ccw_key;
	n = MIN(abs(detents), MA730_MAX_TAPS_PER_POLL);

	LOG_DBG("key world: detents=%d taps=%d", detents, n);

	for (int i = 0; i < n; i++) {
		ma730_key_tap(data, keycode);
	}
}

/* Pointing world: meter accumulated counts into smooth wheel events. */
static void
ma730_scroll(const struct device *dev, const struct ma730_layer_mode *mode,
    int delta)
{
	const struct ma730_config	*cfg = dev->config;
	struct ma730_data		*data = dev->data;
	int				 units;
	int				 factor;
	int				 out;
	int				 err;

	units = data->accumulated_counts / cfg->scroll_counts_per_unit;
	if (units == 0) {
		return;
	}

	data->accumulated_counts -= units * cfg->scroll_counts_per_unit;

	/*
	 * Velocity-based acceleration: scale the emitted magnitude (not the
	 * event rate) by the per-poll speed. This keeps the HID report rate
	 * bounded by the poll interval while letting fast turns travel
	 * further. factor is a 256-based fixed-point multiplier.
	 */
	factor = 256;
	if (cfg->scroll_accel_gain > 0) {
		factor += abs(delta) * cfg->scroll_accel_gain;
		factor = MIN(factor, cfg->scroll_accel_max);
	}

	out = (units * factor) / 256;
	if (out == 0) {
		out = (units > 0) ? 1 : -1;
	}

	/*
	 * Direction. Positive delta = clockwise. Defaults: CW scrolls DOWN
	 * (vertical) and pans RIGHT (horizontal); invert-* flips each.
	 */
	if (mode->rel_code == INPUT_REL_WHEEL) {
		out = cfg->invert_scroll ? out : -out;
	} else {
		out = cfg->invert_hscroll ? -out : out;
	}

	LOG_DBG("scroll world: code=%u units=%d factor=%d out=%d",
	    mode->rel_code, units, factor, out);

	err = input_report_rel(dev, mode->rel_code, out, true, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("failed to report wheel input: %d", err);
	}
}

static void
ma730_poll(const struct device *dev)
{
	const struct ma730_config	*cfg = dev->config;
	struct ma730_data		*data = dev->data;
	const struct ma730_layer_mode	*mode;
	uint16_t			 raw;
	uint8_t				 layer;
	int				 delta;
	int				 err;

	/* Drain queued taps every poll, regardless of rotation. */
	ma730_service_keys(data);

	err = ma730_read_angle(dev, &raw);
	if (err != 0) {
		LOG_WRN("spi read failed: %d", err);
		return;
	}

	layer = zmk_keymap_highest_layer_active();
	mode = ma730_mode_for_layer(layer);

	/*
	 * Critical edge case: on any layer (hence mode) change, discard the
	 * delta that straddled the boundary and reset the accumulator so a
	 * single turn cannot leak a phantom tick into the new world.
	 */
	if (layer != data->last_layer) {
		LOG_DBG("layer change %u -> %u, resetting accumulator",
		    data->last_layer, layer);
		data->last_layer = layer;
		data->accumulated_counts = 0;
		data->last_angle = raw;
		return;
	}

	delta = ma730_compute_delta(data->last_angle, raw);
	data->last_angle = raw;

	if (delta == 0) {
		return;
	}

	/*
	 * Sub-threshold opposing deltas are discarded to suppress detent
	 * noise. A confirmed reversal clears the accumulator (MOTR logic).
	 */
	if ((delta > 0) != (data->accumulated_counts > 0) &&
	    data->accumulated_counts != 0) {
		if (delta > -cfg->reversal_threshold &&
		    delta < cfg->reversal_threshold) {
			return;
		}
		data->accumulated_counts = 0;
	}

	data->accumulated_counts += delta;

	switch (mode->world) {
	case MA730_KEY:
		ma730_keys(dev, mode);
		break;
	case MA730_SCROLL:
		ma730_scroll(dev, mode, delta);
		break;
	case MA730_INACTIVE:
	default:
		/* Knob does nothing on this layer; never build up counts. */
		data->accumulated_counts = 0;
		break;
	}
}

#if defined(CONFIG_MA730_INPUT_TRIGGER_GLOBAL_THREAD)

static void
ma730_work_handler(struct k_work *work)
{
	struct ma730_data	*data;

	data = CONTAINER_OF(work, struct ma730_data, work);
	ma730_poll(data->dev);
}

static void
ma730_timer_expiry(struct k_timer *timer)
{
	struct ma730_data	*data;

	data = CONTAINER_OF(timer, struct ma730_data, timer);
	k_work_submit(&data->work);
}

#elif defined(CONFIG_MA730_INPUT_TRIGGER_OWN_THREAD)

static void
ma730_timer_expiry(struct k_timer *timer)
{
	struct ma730_data	*data;

	data = CONTAINER_OF(timer, struct ma730_data, timer);
	k_sem_give(&data->sem);
}

static void
ma730_thread(void *p1, void *p2, void *p3)
{
	const struct device	*dev = p1;
	struct ma730_data	*data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&data->sem, K_FOREVER);
		ma730_poll(dev);
	}
}

#endif

static int
ma730_init(const struct device *dev)
{
	const struct ma730_config	*cfg = dev->config;
	struct ma730_data		*data = dev->data;
	uint16_t			 initial;
	int				 err;

	data->dev = dev;
	data->last_layer = 0;
	data->accumulated_counts = 0;
	data->key_head = 0;
	data->key_count = 0;

	if (cfg->scroll_counts_per_unit <= 0) {
		LOG_ERR("scroll-counts-per-unit must be >= 1");
		return -EINVAL;
	}
	if (cfg->key_counts_per_detent <= 0) {
		LOG_ERR("key-counts-per-detent must be >= 1");
		return -EINVAL;
	}
	if (cfg->reversal_threshold <= 0) {
		LOG_ERR("reversal-threshold must be >= 1");
		return -EINVAL;
	}
	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("spi bus not ready");
		return -ENODEV;
	}

	/* Avoid a spurious first event. */
	err = ma730_read_angle(dev, &initial);
	if (err != 0) {
		LOG_ERR("failed to read initial angle: %d", err);
		return err;
	}

	data->last_angle = initial;

#if defined(CONFIG_MA730_INPUT_TRIGGER_GLOBAL_THREAD)
	k_work_init(&data->work, ma730_work_handler);
#elif defined(CONFIG_MA730_INPUT_TRIGGER_OWN_THREAD)
	k_sem_init(&data->sem, 0, UINT_MAX);

	k_thread_create(&data->thread, data->stack,
	    CONFIG_MA730_INPUT_THREAD_STACK_SIZE,
	    ma730_thread, (void *)dev, NULL, NULL,
	    K_PRIO_COOP(CONFIG_MA730_INPUT_THREAD_PRIORITY),
	    0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "ma730_input");
#endif

	k_timer_init(&data->timer, ma730_timer_expiry, NULL);
	k_timer_start(&data->timer,
	    K_MSEC(CONFIG_MA730_INPUT_POLL_INTERVAL_MS),
	    K_MSEC(CONFIG_MA730_INPUT_POLL_INTERVAL_MS));

	return 0;
}

#define MA730_DEFINE(n)							\
	static struct ma730_data ma730_data_##n;			\
	static const struct ma730_config ma730_cfg_##n = {		\
		.spi		    = SPI_DT_SPEC_INST_GET(n,		\
				        SPI_WORD_SET(8) |		\
				        SPI_OP_MODE_MASTER |		\
				        SPI_MODE_CPOL |			\
				        SPI_MODE_CPHA, 0),		\
		.reversal_threshold = DT_INST_PROP(n, reversal_threshold),\
		.scroll_counts_per_unit =				\
		    DT_INST_PROP(n, scroll_counts_per_unit),		\
		.key_counts_per_detent =				\
		    DT_INST_PROP(n, key_counts_per_detent),		\
		.scroll_accel_gain  = DT_INST_PROP(n, scroll_accel_gain),\
		.scroll_accel_max   = DT_INST_PROP(n, scroll_accel_max),\
		.invert_scroll      = DT_INST_PROP(n, invert_scroll),	\
		.invert_hscroll     = DT_INST_PROP(n, invert_hscroll),	\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, ma730_init, NULL,			\
	    &ma730_data_##n, &ma730_cfg_##n,				\
	    POST_KERNEL, MA730_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MA730_DEFINE)
