// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5705 fuel gauge
 *
 * Read-only: reports the state of charge, voltage, current and temperature
 * that the gauge computes itself. The gauge runs from the battery and keeps
 * its battery model across reboots, so once the vendor firmware has set it
 * up its readings stay valid. Loading the model is not implemented; the
 * driver warns when the gauge reports that it lost its setup.
 *
 * Design capacity, voltages and chemistry come from the monitored-battery
 * node, which the power supply core exposes by itself.
 *
 * Register layout from the Samsung vendor driver (sm5705_fuelgauge.c).
 */

#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define SM5705_REG_DEVICE_ID		0x00
#define SM5705_REG_SOC			0x05
#define SM5705_REG_OCV			0x06
#define SM5705_REG_VOLTAGE		0x07
#define SM5705_REG_CURRENT		0x08
#define SM5705_REG_TEMPERATURE		0x09
#define SM5705_REG_FG_OP_STATUS		0x10

#define SM5705_FG_OP_INIT_DONE		BIT(4)
#define SM5705_SIGN			BIT(15)

/* |current| below this counts as idle when guessing the status */
#define SM5705_IDLE_CURRENT_UA		20000
#define SM5705_POLL_INTERVAL		(30 * HZ)

struct sm5705_fg {
	struct i2c_client *client;
	struct power_supply *psy;
	struct delayed_work work;
	int last_capacity;
	int last_status;
};

static int sm5705_fg_read(struct sm5705_fg *fg, u8 reg)
{
	return i2c_smbus_read_word_data(fg->client, reg);
}

/* SOC: integer percent in bits 15:8, 1/256 percent in bits 7:0 */
static int sm5705_fg_capacity(struct sm5705_fg *fg, int *val)
{
	int ret = sm5705_fg_read(fg, SM5705_REG_SOC);

	if (ret < 0)
		return ret;

	*val = clamp((ret + 128) >> 8, 0, 100);
	return 0;
}

/* volts in bits 13:11, 1/2048 V in bits 10:0 */
static int sm5705_fg_voltage(struct sm5705_fg *fg, u8 reg, int *val)
{
	int ret = sm5705_fg_read(fg, reg);

	if (ret < 0)
		return ret;

	*val = ((ret >> 11) & 0x7) * 1000000 + (ret & 0x7ff) * 1000000 / 2048;
	return 0;
}

/* sign in bit 15 (set: discharging), amps in bits 12:11, 1/2048 A in 10:0 */
static int sm5705_fg_current(struct sm5705_fg *fg, int *val)
{
	int ret = sm5705_fg_read(fg, SM5705_REG_CURRENT);

	if (ret < 0)
		return ret;

	*val = ((ret >> 11) & 0x3) * 1000000 + (ret & 0x7ff) * 1000000 / 2048;
	if (ret & SM5705_SIGN)
		*val = -*val;
	return 0;
}

/* sign in bit 15, degrees in bits 14:8, 1/16 degree in bits 7:4; tenths */
static int sm5705_fg_temp(struct sm5705_fg *fg, int *val)
{
	int ret = sm5705_fg_read(fg, SM5705_REG_TEMPERATURE);

	if (ret < 0)
		return ret;

	*val = ((ret >> 8) & 0x7f) * 10 + ((ret >> 4) & 0xf) * 10 / 16;
	if (ret & SM5705_SIGN)
		*val = -*val;
	return 0;
}

/* Ask the charger if there is one, else guess from the gauge */
static int sm5705_fg_status(struct sm5705_fg *fg, int *val)
{
	union power_supply_propval sup;
	int ret, curr, cap;

	if (!power_supply_get_property_from_supplier(fg->psy,
						     POWER_SUPPLY_PROP_STATUS,
						     &sup)) {
		*val = sup.intval;
		return 0;
	}

	ret = sm5705_fg_current(fg, &curr);
	if (ret)
		return ret;
	ret = sm5705_fg_capacity(fg, &cap);
	if (ret)
		return ret;

	if (curr > SM5705_IDLE_CURRENT_UA)
		*val = POWER_SUPPLY_STATUS_CHARGING;
	else if (curr < -SM5705_IDLE_CURRENT_UA)
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (cap >= 100)
		*val = POWER_SUPPLY_STATUS_FULL;
	else
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
	return 0;
}

static int sm5705_fg_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct sm5705_fg *fg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return sm5705_fg_status(fg, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		return sm5705_fg_capacity(fg, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return sm5705_fg_voltage(fg, SM5705_REG_VOLTAGE, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		return sm5705_fg_voltage(fg, SM5705_REG_OCV, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return sm5705_fg_current(fg, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		return sm5705_fg_temp(fg, &val->intval);
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property sm5705_fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_desc sm5705_fg_desc = {
	.name = "sm5705-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5705_fg_props,
	.num_properties = ARRAY_SIZE(sm5705_fg_props),
	.get_property = sm5705_fg_get_property,
	.external_power_changed = power_supply_changed,
};

/* The alert interrupt is not used: poll and notify user space */
static void sm5705_fg_work(struct work_struct *work)
{
	struct sm5705_fg *fg = container_of(work, struct sm5705_fg, work.work);
	int cap, status;

	if (!sm5705_fg_capacity(fg, &cap) && !sm5705_fg_status(fg, &status) &&
	    (cap != fg->last_capacity || status != fg->last_status)) {
		fg->last_capacity = cap;
		fg->last_status = status;
		power_supply_changed(fg->psy);
	}

	schedule_delayed_work(&fg->work, SM5705_POLL_INTERVAL);
}

static void sm5705_fg_cancel_work(void *data)
{
	struct sm5705_fg *fg = data;

	cancel_delayed_work_sync(&fg->work);
}

static int sm5705_fg_probe(struct i2c_client *client)
{
	struct power_supply_config cfg = {};
	struct device *dev = &client->dev;
	struct sm5705_fg *fg;
	int id, op, ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->client = client;
	fg->last_capacity = -1;
	fg->last_status = -1;

	id = sm5705_fg_read(fg, SM5705_REG_DEVICE_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "failed to read device id\n");

	op = sm5705_fg_read(fg, SM5705_REG_FG_OP_STATUS);
	if (op < 0)
		return dev_err_probe(dev, op, "failed to read status\n");
	if (!(op & SM5705_FG_OP_INIT_DONE))
		dev_warn(dev, "gauge lost its battery model (op status %#06x), readings may be wrong\n",
			 op);

	cfg.drv_data = fg;
	cfg.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &sm5705_fg_desc, &cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy),
				     "failed to register power supply\n");

	INIT_DELAYED_WORK(&fg->work, sm5705_fg_work);
	ret = devm_add_action_or_reset(dev, sm5705_fg_cancel_work, fg);
	if (ret)
		return ret;
	schedule_delayed_work(&fg->work, 0);

	dev_info(dev, "SM5705 fuel gauge, id %#06x\n", id);
	return 0;
}

static const struct of_device_id sm5705_fg_of_match[] = {
	{ .compatible = "siliconmitus,sm5705-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5705_fg_of_match);

static struct i2c_driver sm5705_fg_driver = {
	.driver = {
		.name = "sm5705-fuel-gauge",
		.of_match_table = sm5705_fg_of_match,
	},
	.probe = sm5705_fg_probe,
};
module_i2c_driver(sm5705_fg_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5705 fuel gauge driver");
MODULE_LICENSE("GPL");
