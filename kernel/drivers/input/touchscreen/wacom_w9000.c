// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Wacom W9000-series penabled I2C touchscreen driver
 *
 * Copyright (c) 2026 Hendrik Noack <hendrik-noack@gmx.de>
 *
 * Partially based on vendor driver:
 *	Copyright (C) 2012, Samsung Electronics Co. Ltd.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/lockdep.h>
#include <linux/regulator/consumer.h>

#include "wacom_w9000.h"

/* Message length */
#define CMD_QUERY_NUM_MAX	15
#define MSG_COORD_NUM_MAX	14

struct wacom_w9000_variant {
	const struct wacom_w9000_query_policy query;
	const u8 msg_coord_num;
	const u8 special_packet_byte;
	const bool has_distance;
	const bool requires_flash_mode;
	const u16 power_off_delay_us;
	const char *name;
};

struct wacom_w9000_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	const struct wacom_w9000_variant *variant;
	u16 fw_version;

	struct touchscreen_properties prop;
	u16 max_pressure;

	struct regulator *regulator;

	struct gpio_desc *flash_mode_gpio;
	struct gpio_desc *reset_gpio;

	unsigned int irq;

	struct wacom_w9000_input_state input_state;
	struct wacom_w9000_power_state power_state;
};

static int wacom_w9000_query_transfer(void *context, u8 command, u8 *data,
				      size_t len)
{
	struct wacom_w9000_data *wacom_data = context;
	struct i2c_client *client = wacom_data->client;
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.buf = &command,
			.len = sizeof(command),
		}, {
			.addr = client->addr,
			.flags = I2C_M_RD,
			.buf = data,
			.len = len,
		}
	};

	return i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
}

static int wacom_w9000_query_send(void *context, u8 command)
{
	struct wacom_w9000_data *wacom_data = context;

	return i2c_master_send(wacom_data->client, &command, sizeof(command));
}

static int wacom_w9000_query_receive(void *context, u8 *data, size_t len)
{
	struct wacom_w9000_data *wacom_data = context;

	return i2c_master_recv(wacom_data->client, data, len);
}

static void wacom_w9000_query_delay(void *context, unsigned int delay_ms)
{
	msleep(delay_ms);
}

static const struct wacom_w9000_query_ops wacom_w9000_query_ops = {
	.transfer = wacom_w9000_query_transfer,
	.send = wacom_w9000_query_send,
	.receive = wacom_w9000_query_receive,
	.delay = wacom_w9000_query_delay,
};

static int wacom_w9000_query(struct wacom_w9000_data *wacom_data)
{
	const struct wacom_w9000_query_policy *policy =
		&wacom_data->variant->query;
	const struct wacom_w9000_query_ops *ops = &wacom_w9000_query_ops;
	struct device *dev = &wacom_data->client->dev;
	struct wacom_w9000_features features;
	u8 data[CMD_QUERY_NUM_MAX];
	int error;

	error = wacom_w9000_run_query(policy, ops, wacom_data, data, &features);
	if (error)
		return error;

	dev_dbg(dev, "query: %*ph\n",
		wacom_data->variant->query.response_len, data);

	wacom_data->prop.max_x = features.max_x;
	wacom_data->prop.max_y = features.max_y;
	wacom_data->max_pressure = features.max_pressure;
	wacom_data->fw_version = features.fw_version;

	dev_dbg(dev, "max_x:%d, max_y:%d, max_pressure:%d, fw:%#x\n",
		wacom_data->prop.max_x, wacom_data->prop.max_y,
		wacom_data->max_pressure, wacom_data->fw_version);

	return 0;
}

static void wacom_w9000_input_report_key(void *context, unsigned int code,
					 int value)
{
	struct wacom_w9000_data *wacom_data = context;

	input_report_key(wacom_data->input_dev, code, value);
}

static void wacom_w9000_input_report_abs(void *context, unsigned int code,
					 int value)
{
	struct wacom_w9000_data *wacom_data = context;

	input_report_abs(wacom_data->input_dev, code, value);
}

static void wacom_w9000_input_report_pos(void *context, u16 x, u16 y)
{
	struct wacom_w9000_data *wacom_data = context;

	touchscreen_report_pos(wacom_data->input_dev, &wacom_data->prop,
			       x, y, false);
}

static void wacom_w9000_input_sync(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	input_sync(wacom_data->input_dev);
}

static const struct wacom_w9000_input_ops wacom_w9000_input_ops = {
	.report_key = wacom_w9000_input_report_key,
	.report_abs = wacom_w9000_input_report_abs,
	.report_pos = wacom_w9000_input_report_pos,
	.sync = wacom_w9000_input_sync,
};

static void wacom_w9000_release_pen(struct wacom_w9000_data *wacom_data)
{
	wacom_w9000_release_input(&wacom_data->input_state,
				  wacom_data->variant->has_distance,
				  &wacom_w9000_input_ops, wacom_data);
}

static int wacom_w9000_supply_enable(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	return regulator_enable(wacom_data->regulator);
}

static int wacom_w9000_supply_disable(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	return regulator_disable(wacom_data->regulator);
}

static void wacom_w9000_reset(void *context, bool asserted)
{
	struct wacom_w9000_data *wacom_data = context;

	gpiod_set_value_cansleep(wacom_data->reset_gpio, asserted);
}

static void wacom_w9000_irq_enable(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	enable_irq(wacom_data->irq);
}

static void wacom_w9000_irq_disable(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	disable_irq(wacom_data->irq);
}

static void wacom_w9000_release_input_callback(void *context)
{
	wacom_w9000_release_pen(context);
}

static void wacom_w9000_startup_delay(void *context)
{
	msleep(200);
}

static void wacom_w9000_shutdown_delay(void *context)
{
	struct wacom_w9000_data *wacom_data = context;

	if (wacom_data->variant->power_off_delay_us)
		usleep_range(wacom_data->variant->power_off_delay_us,
			     wacom_data->variant->power_off_delay_us + 1000);
}

static const struct wacom_w9000_power_ops wacom_w9000_power_ops = {
	.supply_enable = wacom_w9000_supply_enable,
	.supply_disable = wacom_w9000_supply_disable,
	.reset = wacom_w9000_reset,
	.irq_enable = wacom_w9000_irq_enable,
	.irq_disable = wacom_w9000_irq_disable,
	.release_input = wacom_w9000_release_input_callback,
	.startup_delay = wacom_w9000_startup_delay,
	.shutdown_delay = wacom_w9000_shutdown_delay,
};

static int wacom_w9000_power_on(struct wacom_w9000_data *wacom_data)
{
	struct wacom_w9000_power_state *state = &wacom_data->power_state;
	const struct wacom_w9000_power_ops *ops = &wacom_w9000_power_ops;
	int error;

	lockdep_assert_held(&wacom_data->input_dev->mutex);

	error = wacom_w9000_power_on_transition(state, ops, wacom_data);
	if (error)
		dev_err(&wacom_data->client->dev,
			"Failed to enable regulators: %d\n", error);

	return error;
}

static int wacom_w9000_power_off(struct wacom_w9000_data *wacom_data,
				 bool restore_on_failure)
{
	lockdep_assert_held(&wacom_data->input_dev->mutex);

	return wacom_w9000_power_off_transition(&wacom_data->power_state,
						&wacom_w9000_power_ops,
						wacom_data,
						restore_on_failure);
}

static void wacom_w9000_disable_regulator(void *data)
{
	struct wacom_w9000_data *wacom_data = data;
	int error;

	error = wacom_w9000_supply_off(&wacom_data->power_state,
				       &wacom_w9000_power_ops, wacom_data);
	if (error)
		dev_err(&wacom_data->client->dev,
			"Failed to disable regulators during cleanup: %d\n", error);
}

static void wacom_w9000_coord(struct wacom_w9000_data *wacom_data)
{
	struct i2c_client *client = wacom_data->client;
	struct device *dev = &wacom_data->client->dev;
	const struct wacom_w9000_report_format format = {
		.length = wacom_data->variant->msg_coord_num,
		.special_packet_byte = wacom_data->variant->special_packet_byte,
		.has_distance = wacom_data->variant->has_distance,
	};
	const struct wacom_w9000_features features = {
		.max_x = wacom_data->prop.max_x,
		.max_y = wacom_data->prop.max_y,
		.max_pressure = wacom_data->max_pressure,
		.fw_version = wacom_data->fw_version,
	};
	struct wacom_w9000_report report;
	u8 data[MSG_COORD_NUM_MAX];
	int error;

	error = i2c_master_recv(client, data, wacom_data->variant->msg_coord_num);
	error = wacom_w9000_xfer_result(error,
					wacom_data->variant->msg_coord_num);
	if (error) {
		dev_err_ratelimited(dev, "%s: i2c receive failed (%d)\n", __func__, error);
		return;
	}

	dev_dbg(dev, "data: %*ph\n", wacom_data->variant->msg_coord_num, data);

	error = wacom_w9000_parse_report(data,
					 wacom_data->variant->msg_coord_num,
					 &format, &features, &report);
	if (error) {
		dev_warn_ratelimited(dev, "Invalid coordinate report: %d\n", error);
		return;
	}

	wacom_w9000_report_input(&wacom_data->input_state, &report,
				 wacom_data->variant->has_distance,
				 &wacom_w9000_input_ops, wacom_data);
}

static irqreturn_t wacom_w9000_interrupt(int irq, void *dev_id)
{
	struct wacom_w9000_data *wacom_data = dev_id;

	wacom_w9000_coord(wacom_data);

	return IRQ_HANDLED;
}

static int wacom_w9000_open(struct input_dev *dev)
{
	struct wacom_w9000_data *wacom_data = input_get_drvdata(dev);

	return wacom_w9000_power_on(wacom_data);
}

static void wacom_w9000_close(struct input_dev *dev)
{
	struct wacom_w9000_data *wacom_data = input_get_drvdata(dev);
	int error;

	error = wacom_w9000_power_off(wacom_data, false);
	if (error)
		dev_err(&wacom_data->client->dev,
			"Failed to disable regulators: %d\n", error);
}

static int wacom_w9000_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct wacom_w9000_data *wacom_data;
	struct input_dev *input_dev;
	enum gpiod_flags flash_mode_flags;
	int disable_error;
	int error;
	u32 val;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	wacom_data = devm_kzalloc(dev, sizeof(*wacom_data), GFP_KERNEL);
	if (!wacom_data)
		return -ENOMEM;

	wacom_data->variant = i2c_get_match_data(client);
	if (!wacom_data->variant) {
		dev_err(dev, "No i2c match_data available\n");
		return -EINVAL;
	}

	if (wacom_data->variant->query.response_len <
					WACOM_W9000_QUERY_MIN_LEN ||
	    wacom_data->variant->query.response_len > CMD_QUERY_NUM_MAX ||
	    wacom_data->variant->msg_coord_num > MSG_COORD_NUM_MAX) {
		dev_err(dev, "Invalid message length for %s\n",
			wacom_data->variant->name);
		return -EINVAL;
	}

	if (wacom_data->variant->msg_coord_num < WACOM_W9000_REPORT_MIN_LEN) {
		dev_err(dev, "Length of coordinates message for %s too short\n",
			wacom_data->variant->name);
		return -EINVAL;
	}
	if (wacom_data->variant->has_distance &&
	    wacom_data->variant->msg_coord_num <= WACOM_W9000_REPORT_MIN_LEN) {
		dev_err(dev, "Distance report for %s has no data byte\n",
			wacom_data->variant->name);
		return -EINVAL;
	}

	if (wacom_data->variant->special_packet_byte >=
	    wacom_data->variant->msg_coord_num) {
		dev_err(dev, "Special packet byte for %s is out of range\n",
			wacom_data->variant->name);
		return -EINVAL;
	}

	wacom_data->client = client;
	wacom_data->irq = client->irq;
	i2c_set_clientdata(client, wacom_data);

	wacom_data->regulator = devm_regulator_get(dev, "vdd");
	if (IS_ERR(wacom_data->regulator))
		return dev_err_probe(dev, PTR_ERR(wacom_data->regulator),
				     "Failed to get regulators\n");
	error = devm_add_action_or_reset(dev, wacom_w9000_disable_regulator,
					 wacom_data);
	if (error)
		return error;

	flash_mode_flags = wacom_data->variant->requires_flash_mode ?
					GPIOD_ASIS : GPIOD_OUT_LOW;
	wacom_data->flash_mode_gpio =
		devm_gpiod_get_optional(dev, "flash-mode", flash_mode_flags);
	if (IS_ERR(wacom_data->flash_mode_gpio))
		return dev_err_probe(dev, PTR_ERR(wacom_data->flash_mode_gpio),
				     "Failed to get flash-mode gpio\n");
	if (wacom_data->variant->requires_flash_mode &&
	    !wacom_data->flash_mode_gpio)
		return dev_err_probe(dev, -EINVAL,
				     "active-high flash-mode gpio is required for %s\n",
				     wacom_data->variant->name);
	if (wacom_data->variant->requires_flash_mode &&
	    gpiod_is_active_low(wacom_data->flash_mode_gpio))
		return dev_err_probe(dev, -EINVAL,
				     "flash-mode gpio for %s must be active-high\n",
				     wacom_data->variant->name);
	if (wacom_data->variant->requires_flash_mode) {
		error = gpiod_direction_output_raw(wacom_data->flash_mode_gpio, 0);
		if (error)
			return dev_err_probe(dev, error,
					     "Failed to hold flash-mode gpio low\n");
	}

	wacom_data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(wacom_data->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(wacom_data->reset_gpio),
				     "Failed to get reset gpio\n");

	error = regulator_enable(wacom_data->regulator);
	if (error)
		return dev_err_probe(dev, error, "Failed to enable regulators\n");
	wacom_data->power_state.supply_enabled = true;

	msleep(200);

	gpiod_set_value_cansleep(wacom_data->reset_gpio, 0);

	error = wacom_w9000_query(wacom_data);

	gpiod_set_value_cansleep(wacom_data->reset_gpio, 1);
	disable_error = wacom_w9000_supply_off(&wacom_data->power_state,
					       &wacom_w9000_power_ops,
					       wacom_data);

	if (error)
		return dev_err_probe(dev, error, "Failed to query\n");
	if (disable_error)
		return dev_err_probe(dev, disable_error,
				     "Failed to disable regulators\n");

	error = devm_request_threaded_irq(dev, wacom_data->irq, NULL, wacom_w9000_interrupt,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN, client->name, wacom_data);
	if (error)
		return dev_err_probe(dev, error, "Failed to register interrupt\n");

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev)
		return -ENOMEM;

	wacom_data->input_dev = input_dev;
	input_set_drvdata(input_dev, wacom_data);

	input_dev->name = wacom_data->variant->name;
	input_dev->id.bustype = BUS_I2C;
	input_dev->id.vendor = 0x56a;
	input_dev->id.version = wacom_data->fw_version;
	input_dev->open = wacom_w9000_open;
	input_dev->close = wacom_w9000_close;

	input_set_capability(input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(input_dev, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(input_dev, EV_KEY, BTN_TOOL_RUBBER);
	input_set_capability(input_dev, EV_KEY, BTN_STYLUS);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	input_set_abs_params(input_dev, ABS_X, 0, wacom_data->prop.max_x, 4, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, wacom_data->prop.max_y, 4, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, wacom_data->max_pressure, 0, 0);

	if (wacom_data->variant->has_distance)
		input_set_abs_params(input_dev, ABS_DISTANCE, 0, 255, 0, 0);

	touchscreen_parse_properties(input_dev, false, &wacom_data->prop);

	dev_info(dev, "%s size X%uY%u\n", wacom_data->variant->name,
		 wacom_data->prop.max_x, wacom_data->prop.max_y);

	error = device_property_read_u32(dev, "touchscreen-x-mm", &val);
	if (!error && val)
		input_abs_set_res(input_dev, wacom_data->prop.swap_x_y ? ABS_Y : ABS_X,
				  wacom_data->prop.max_x / val);
	error = device_property_read_u32(dev, "touchscreen-y-mm", &val);
	if (!error && val)
		input_abs_set_res(input_dev, wacom_data->prop.swap_x_y ? ABS_X : ABS_Y,
				  wacom_data->prop.max_y / val);

	error = input_register_device(wacom_data->input_dev);
	if (error)
		return dev_err_probe(dev, error, "Failed to register input device\n");

	return 0;
}

static int wacom_w9000_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_w9000_data *wacom_data = i2c_get_clientdata(client);

	guard(mutex)(&wacom_data->input_dev->mutex);

	if (input_device_enabled(wacom_data->input_dev)) {
		return wacom_w9000_power_off(wacom_data, true);
	}
	if (wacom_data->power_state.supply_enabled)
		return wacom_w9000_power_off(wacom_data, false);

	return 0;
}

static int wacom_w9000_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_w9000_data *wacom_data = i2c_get_clientdata(client);

	guard(mutex)(&wacom_data->input_dev->mutex);

	if (input_device_enabled(wacom_data->input_dev))
		return wacom_w9000_power_on(wacom_data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(wacom_w9000_pm, wacom_w9000_suspend, wacom_w9000_resume);

static const struct wacom_w9000_variant w9002 = {
	.query.response_len = 9,
	.msg_coord_num  = 7,
	.name = "Wacom W9002 Digitizer",
};

static const struct wacom_w9000_variant w9007a_lt03 = {
	.query.response_len = 9,
	.msg_coord_num	= 8,
	.has_distance	= true,
	.name = "Wacom W9007A LT03 Digitizer",
};

static const struct wacom_w9000_variant w9007a_v1 = {
	.query.response_len = 9,
	.msg_coord_num	= 12,
	.has_distance	= true,
	.name = "Wacom W9007A V1 Digitizer",
};

static const struct wacom_w9000_variant w9019 = {
	.query = {
		.response_len = 15,
		.retries = 3,
		.response_delay_ms = 100,
		.retry_delay_ms = 100,
		.split_transfer = true,
		.require_nonzero_ranges = true,
	},
	.msg_coord_num	= 14,
	.special_packet_byte = 12,
	.requires_flash_mode = true,
	.power_off_delay_us = 10000,
	.name = "Wacom W9019 Digitizer",
};

static const struct of_device_id wacom_w9000_of_match[] = {
	{ .compatible = "wacom,w9002", .data = &w9002 },
	{ .compatible = "wacom,w9007a-lt03", .data = &w9007a_lt03, },
	{ .compatible = "wacom,w9007a-v1", .data = &w9007a_v1, },
	{ .compatible = "wacom,w9019", .data = &w9019, },
	{ }
};
MODULE_DEVICE_TABLE(of, wacom_w9000_of_match);

static const struct i2c_device_id wacom_w9000_id[] = {
	{ .name = "w9002", .driver_data = (kernel_ulong_t)&w9002 },
	{ .name = "w9007a-lt03", .driver_data = (kernel_ulong_t)&w9007a_lt03 },
	{ .name = "w9007a-v1", .driver_data = (kernel_ulong_t)&w9007a_v1 },
	{ .name = "w9019", .driver_data = (kernel_ulong_t)&w9019 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wacom_w9000_id);

static struct i2c_driver wacom_w9000_driver = {
	.driver = {
		.name	= "wacom_w9000",
		.of_match_table = wacom_w9000_of_match,
		.pm	= pm_sleep_ptr(&wacom_w9000_pm),
	},
	.probe		= wacom_w9000_probe,
	.id_table	= wacom_w9000_id,
};
module_i2c_driver(wacom_w9000_driver);

MODULE_AUTHOR("Hendrik Noack <hendrik-noack@gmx.de>");
MODULE_DESCRIPTION("Wacom W9000-series penabled touchscreen driver");
MODULE_LICENSE("GPL");
