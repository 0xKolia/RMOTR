/*
 * Copyright (c) 2026 Carl Henriksson
 * SPDX-License-Identifier: MIT
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

LOG_MODULE_REGISTER(ma730_input, CONFIG_INPUT_LOG_LEVEL);

#define MA730_COUNTS_PER_REV	65536

/* Initialise after SPI bus (CONFIG_SPI_INIT_PRIORITY = 70). */
#define MA730_INIT_PRIORITY	90

struct ma730_config {
	struct spi_dt_spec	 spi;
	int			 scroll_divisor;
	int			 reversal_threshold;
	bool			 invert_scroll;
};

struct ma730_data {
	const struct device	*dev;
	uint16_t		 last_angle;
	int			 accumulated_counts;
	int			 counts_per_tick;
	struct k_timer		 timer;
#if defined(CONFIG_MA730_INPUT_TRIGGER_GLOBAL_THREAD)
	struct k_work		 work;
#elif defined(CONFIG_MA730_INPUT_TRIGGER_OWN_THREAD)
	K_THREAD_STACK_MEMBER(stack, CONFIG_MA730_INPUT_THREAD_STACK_SIZE);
	struct k_thread		 thread;
	struct k_sem		 sem;
#endif
};

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

static void
ma730_poll(const struct device *dev)
{
	const struct ma730_config	*cfg = dev->config;
	struct ma730_data		*data = dev->data;
	uint16_t			 raw;
	int				 delta;
	int				 ticks;
	int				 err;

	err = ma730_read_angle(dev, &raw);
	if (err != 0) {
		LOG_WRN("spi read failed: %d", err);
		return;
	}

	delta = ma730_compute_delta(data->last_angle, raw);
	data->last_angle = raw;

	if (delta == 0) {
		return;
	}

	/*
	 * Sub-threshold opposing deltas are discarded to suppress detent
	 * noise. A confirmed reversal clears the accumulator.
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

	ticks = data->accumulated_counts / data->counts_per_tick;
	if (ticks == 0) {
		return;
	}

	LOG_DBG("delta=%d acc=%d ticks=%d",
	    delta, data->accumulated_counts, ticks);

	data->accumulated_counts -= ticks * data->counts_per_tick;

	err = input_report_rel(dev, INPUT_REL_WHEEL,
	    cfg->invert_scroll ? -ticks : ticks, true, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("failed to report wheel input: %d", err);
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

	if (cfg->scroll_divisor <= 0) {
		LOG_ERR("scroll-divisor must be >= 1");
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

	data->counts_per_tick =
	    DIV_ROUND_CLOSEST(cfg->scroll_divisor * MA730_COUNTS_PER_REV, 360);
	if (data->counts_per_tick <= 0) {
		LOG_ERR("invalid scroll-divisor");
		return -EINVAL;
	}

	if (cfg->reversal_threshold < data->counts_per_tick / 10) {
		LOG_WRN("reversal-threshold (%d) is less than one-tenth of "
		    "counts_per_tick (%d); detent noise may cause spurious "
		    "accumulator resets",
		    cfg->reversal_threshold, data->counts_per_tick);
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
		.scroll_divisor     = DT_INST_PROP(n, scroll_divisor),	\
		.reversal_threshold = DT_INST_PROP(n, reversal_threshold),\
		.invert_scroll      = DT_INST_PROP(n, invert_scroll),	\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, ma730_init, NULL,			\
	    &ma730_data_##n, &ma730_cfg_##n,				\
	    POST_KERNEL, MA730_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MA730_DEFINE)
