// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wacom W9019 pen digitizer with Samsung's I2C firmware
 *
 * The protocol follows Samsung's wacom_tab driver in the Galaxy Tab S3
 * (SM-T820) kernel source (Copyright (C) 2012 Samsung Electronics,
 * GPL-2.0): a query command answered with the digitizer's ranges, then
 * a plain read of one coordinate packet per interrupt. It is not the
 * HID-style protocol of the wacom_i2c driver.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#define W9019_CMD_QUERY		0x2a
#define W9019_QUERY_LEN		15
#define W9019_QUERY_HEADER	0x0f
#define W9019_QUERY_RETRIES	3
#define W9019_PACKET_LEN	14

/* Packet byte 0 */
#define W9019_IN_RANGE		BIT(7)
#define W9019_ERASER		BIT(6)
#define W9019_BARREL		BIT(5)
#define W9019_TIP		BIT(4)
/* Packet byte 12: a soft key event instead of a pen sample */
#define W9019_SOFTKEY		BIT(7)

/* Coordinates are in 10 um: 19661 x 14746 over the SM-T820's 197 x 148 mm */
#define W9019_RES_PER_MM	100
#define W9019_TILT_MAX		63

/* Delays of the vendor driver: power-on to query, query to its answer */
#define W9019_POWER_ON_MS	200
#define W9019_QUERY_MS		100

struct w9019 {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vdd;
	struct touchscreen_properties prop;
	unsigned int tool;
	bool in_range;
};

static int w9019_query(struct w9019 *w, u8 *buf)
{
	struct i2c_client *client = w->client;
	u8 cmd = W9019_CMD_QUERY;
	int ret = -EIO;
	int i;

	for (i = 0; i < W9019_QUERY_RETRIES; i++) {
		ret = i2c_master_send(client, &cmd, 1);
		if (ret < 0)
			continue;

		msleep(W9019_QUERY_MS);

		ret = i2c_master_recv(client, buf, W9019_QUERY_LEN);
		if (ret < 0)
			continue;
		if (ret == W9019_QUERY_LEN && buf[0] == W9019_QUERY_HEADER)
			return 0;

		dev_dbg(&client->dev, "bad query answer: %*ph\n", ret, buf);
		ret = -EIO;
	}

	return ret;
}

static void w9019_release(struct w9019 *w)
{
	struct input_dev *input = w->input;

	input_report_abs(input, ABS_PRESSURE, 0);
	input_report_key(input, BTN_TOUCH, 0);
	input_report_key(input, BTN_STYLUS, 0);
	input_report_key(input, w->tool, 0);
	input_sync(input);
	w->in_range = false;
}

static irqreturn_t w9019_irq(int irq, void *dev_id)
{
	struct w9019 *w = dev_id;
	struct input_dev *input = w->input;
	u8 d[W9019_PACKET_LEN];
	unsigned int x, y;
	int tilt_x, tilt_y;
	int ret;

	ret = i2c_master_recv(w->client, d, sizeof(d));
	if (ret != sizeof(d))
		return IRQ_HANDLED;

	if (!(d[0] & W9019_IN_RANGE)) {
		if (w->in_range)
			w9019_release(w);
		return IRQ_HANDLED;
	}

	if (d[12] & W9019_SOFTKEY)
		return IRQ_HANDLED;

	if (!w->in_range) {
		w->tool = d[0] & W9019_ERASER ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
		w->in_range = true;
	}

	x = min(get_unaligned_be16(&d[1]), w->prop.max_x);
	y = min(get_unaligned_be16(&d[3]), w->prop.max_y);

	/* Tilt follows the position axes */
	tilt_x = (s8)d[8];
	tilt_y = (s8)d[9];
	if (w->prop.invert_x)
		tilt_x = -tilt_x;
	if (w->prop.invert_y)
		tilt_y = -tilt_y;
	if (w->prop.swap_x_y)
		swap(tilt_x, tilt_y);

	touchscreen_report_pos(input, &w->prop, x, y, false);
	input_report_abs(input, ABS_PRESSURE, get_unaligned_be16(&d[5]));
	input_report_abs(input, ABS_DISTANCE, d[7]);
	input_report_abs(input, ABS_TILT_X, tilt_x);
	input_report_abs(input, ABS_TILT_Y, tilt_y);
	input_report_key(input, BTN_TOUCH, d[0] & W9019_TIP);
	input_report_key(input, BTN_STYLUS, d[0] & W9019_BARREL);
	input_report_key(input, w->tool, 1);
	input_sync(input);

	return IRQ_HANDLED;
}

static void w9019_power_off(void *data)
{
	struct w9019 *w = data;

	regulator_disable(w->vdd);
}

static int w9019_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *fwe_gpio;
	struct input_dev *input;
	u8 q[W9019_QUERY_LEN];
	struct w9019 *w;
	int ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->client = client;

	w->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(w->vdd))
		return dev_err_probe(dev, PTR_ERR(w->vdd), "failed to get vdd\n");

	/* Firmware write enable: high starts the boot loader instead */
	fwe_gpio = devm_gpiod_get_optional(dev, "fwe", GPIOD_OUT_LOW);
	if (IS_ERR(fwe_gpio))
		return dev_err_probe(dev, PTR_ERR(fwe_gpio), "failed to get fwe gpio\n");

	ret = regulator_enable(w->vdd);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, w9019_power_off, w);
	if (ret)
		return ret;

	msleep(W9019_POWER_ON_MS);

	ret = w9019_query(w, q);
	if (ret)
		return dev_err_probe(dev, ret, "no answer to the query command\n");

	dev_info(dev, "firmware %#06x, %u x %u, pressure %u\n",
		 get_unaligned_be16(&q[7]), get_unaligned_be16(&q[1]),
		 get_unaligned_be16(&q[3]), get_unaligned_be16(&q[5]));

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	w->input = input;

	input->name = "Wacom W9019 pen";
	input->id.bustype = BUS_I2C;

	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_capability(input, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(input, EV_KEY, BTN_TOOL_RUBBER);
	input_set_capability(input, EV_KEY, BTN_STYLUS);

	input_set_abs_params(input, ABS_X, 0, get_unaligned_be16(&q[1]), 4, 0);
	input_set_abs_params(input, ABS_Y, 0, get_unaligned_be16(&q[3]), 4, 0);
	input_abs_set_res(input, ABS_X, W9019_RES_PER_MM);
	input_abs_set_res(input, ABS_Y, W9019_RES_PER_MM);
	input_set_abs_params(input, ABS_PRESSURE, 0, get_unaligned_be16(&q[5]), 0, 0);
	input_set_abs_params(input, ABS_DISTANCE, 0, U8_MAX, 0, 0);
	input_set_abs_params(input, ABS_TILT_X, -W9019_TILT_MAX, W9019_TILT_MAX, 0, 0);
	input_set_abs_params(input, ABS_TILT_Y, -W9019_TILT_MAX, W9019_TILT_MAX, 0, 0);
	touchscreen_parse_properties(input, false, &w->prop);

	ret = devm_request_threaded_irq(dev, client->irq, NULL, w9019_irq,
					IRQF_ONESHOT, client->name, w);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	i2c_set_clientdata(client, w);

	return input_register_device(input);
}

static int w9019_suspend(struct device *dev)
{
	struct w9019 *w = dev_get_drvdata(dev);

	disable_irq(w->client->irq);
	if (w->in_range)
		w9019_release(w);

	return regulator_disable(w->vdd);
}

static int w9019_resume(struct device *dev)
{
	struct w9019 *w = dev_get_drvdata(dev);
	int ret;

	ret = regulator_enable(w->vdd);
	if (ret)
		return ret;

	msleep(W9019_POWER_ON_MS);
	enable_irq(w->client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(w9019_pm_ops, w9019_suspend, w9019_resume);

static const struct of_device_id w9019_of_match[] = {
	{ .compatible = "wacom,w9019" },
	{ }
};
MODULE_DEVICE_TABLE(of, w9019_of_match);

static const struct i2c_device_id w9019_id[] = {
	{ "w9019" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, w9019_id);

static struct i2c_driver w9019_driver = {
	.driver = {
		.name = "wacom_w9019",
		.of_match_table = w9019_of_match,
		.pm = pm_sleep_ptr(&w9019_pm_ops),
	},
	.probe = w9019_probe,
	.id_table = w9019_id,
};
module_i2c_driver(w9019_driver);

MODULE_DESCRIPTION("Wacom W9019 pen digitizer (Samsung I2C firmware)");
MODULE_LICENSE("GPL");
