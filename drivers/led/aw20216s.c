/*
 * Copyright (c) 2023 Kelly Helmut Lord
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT awinic_aw20216s

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/led/aw20216s.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aw20216s, CONFIG_LED_LOG_LEVEL);

#define AW20216S_MIN_BRIGHTNESS 0
#define AW20216S_MAX_BRIGHTNESS 255

#define AW20216S_SPI_SPEC_CONF (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER)

struct aw20216s_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec enable;
	struct gpio_dt_spec sync;
	const uint8_t *channel_map;
	uint8_t channel_map_len;
	uint8_t current_limit;
	uint8_t sl_current_limit;
};

static int aw20216s_write_register(const struct device *dev, uint8_t reg, uint8_t page,
				   uint8_t val)
{
	if (page > AW20216S_PAGE_4) {
		return -EINVAL;
	}

	const struct aw20216s_config *config = dev->config;

	uint8_t cmd[] = {AW20216S_CHIP_ID | (page << 1) | AW20216S_WRITE, reg, val};

	const struct spi_buf tx[] = {{
		.buf = &cmd,
		.len = sizeof(cmd),
	}};

	struct spi_buf_set tx_buf_set = {
		.buffers = tx,
		.count = 1,
	};

	return spi_write_dt(&config->spi, &tx_buf_set);
}

static int aw20216s_led_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	const struct aw20216s_config *config = dev->config;

	if (led >= config->channel_map_len || value > AW20216S_MAX_BRIGHTNESS) {
		return -EINVAL;
	}

	uint8_t val = (value * 255U) / AW20216S_MAX_BRIGHTNESS;

	int err = aw20216s_write_register(dev,
					  AW20216S_PWM_CONFIGURATION_REGISTER_BASE +
						  config->channel_map[led],
					  AW20216S_PAGE_1, val);

	if (err) {
		LOG_ERR("Failed to set PWM configuration register %d", led);
		return err;
	}

	return 0;
}

static int aw20216s_led_on(const struct device *dev, uint32_t led)
{
	return aw20216s_led_set_brightness(dev, led, AW20216S_MAX_BRIGHTNESS);
}

static int aw20216s_led_off(const struct device *dev, uint32_t led)
{
	return aw20216s_led_set_brightness(dev, led, 0);
}

static int aw20216s_led_init(const struct device *dev)
{
	const struct aw20216s_config *config = dev->config;

	int err;

	if (config->enable.port != NULL) {
		if (!device_is_ready(config->enable.port)) {
			LOG_ERR("Enable GPIO port not ready");
			return -ENODEV;
		}
		gpio_pin_configure_dt(&config->enable, GPIO_OUTPUT);
		gpio_pin_set_dt(&config->enable, 1);
		k_msleep(2);
	}

	if (!device_is_ready(config->spi.bus)) {
		LOG_ERR("SPI bus is not ready");
		return -ENODEV;
	}

	err = aw20216s_write_register(dev, AW20216S_RESET_REGISTER, AW20216S_PAGE_0,
				      AW20216S_DEFAULT_RESET_REGISTER_VALUE);
	if (err) {
		LOG_ERR("Failed to reset AW20216S");
		return err;
	}
	k_msleep(2);

	err = aw20216s_write_register(dev, AW20216S_GLOBAL_CONTROL_REGISTER, AW20216S_PAGE_0,
				      (AW20216S_GLOBAL_CONTROL_REGISTER_VALUE_ALL_SW |
				       AW20216S_GLOBAL_CONTROL_REGISTER_CHIP_ENABLE));
	if (err) {
		LOG_ERR("Failed to enable LED driver");
		return err;
	}

	err = aw20216s_write_register(dev, AW20216S_GLOBAL_CURRENT_CONTROL_REGISTER,
				      AW20216S_PAGE_0, config->current_limit);
	if (err) {
		LOG_ERR("Failed to set global current limit");
		return err;
	}

	for (int i = 0; i < AW20216S_NUM_SOURCE_LEVEL_CONFIG_REGISTERS; i++) {
		err = aw20216s_write_register(dev,
					      AW20216S_SOURCE_LEVEL_CONFIGURATION_REGISTER_BASE + i,
					      AW20216S_PAGE_2, config->sl_current_limit);
		if (err) {
			LOG_ERR("Failed to set source level configuration register %d", i);
			return err;
		}
	}

	return 0;
}

static const struct led_driver_api aw20216s_led_api = {
	.set_brightness = aw20216s_led_set_brightness,
	.on = aw20216s_led_on,
	.off = aw20216s_led_off,
};

#define AW20216S_CHANNEL_INDEX(child) DT_PROP(child, index),

#define AW20216S_DEVICE(id)                                                                        \
	static const uint8_t aw20216s_##id##_channel_map[] = {                                 \
		DT_INST_FOREACH_CHILD_STATUS_OKAY(id, AW20216S_CHANNEL_INDEX)};                  \
	static const struct aw20216s_config aw20216s_##id##_cfg = {                              \
		.spi = SPI_DT_SPEC_INST_GET(id, AW20216S_SPI_SPEC_CONF, 0),                      \
		.enable = GPIO_DT_SPEC_INST_GET_OR(id, en_gpios, {0}),                           \
		.sync = GPIO_DT_SPEC_INST_GET_OR(id, sync_gpios, {0}),                           \
		.channel_map = aw20216s_##id##_channel_map,                                      \
		.channel_map_len = ARRAY_SIZE(aw20216s_##id##_channel_map),                      \
		.current_limit = DT_INST_PROP(id, current_limit),                                \
		.sl_current_limit = DT_INST_PROP(id, sl_current_limit)};                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(id, &aw20216s_led_init, NULL, NULL, &aw20216s_##id##_cfg,          \
			      POST_KERNEL, CONFIG_LED_INIT_PRIORITY, &aw20216s_led_api);

DT_INST_FOREACH_STATUS_OKAY(AW20216S_DEVICE)
