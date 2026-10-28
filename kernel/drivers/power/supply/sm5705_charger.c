// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5705 charger
 *
 * Reports whether USB power is present and what the charger does, and
 * applies the charge voltage, charge current and termination current of
 * the monitored battery. With an extcon device (the SM5705 MUIC) it sets
 * the input current limit for the detected USB charger type, including
 * Samsung AFC chargers at 9 V. Without one
 * the limit stays as the bootloader set it. Either way the charger's AICL
 * lowers the input current when the source sags.
 *
 * The same I2C device also holds the flash LED controller, which is not
 * handled here. Register layout from the Samsung vendor driver
 * (sm5705_charger.c).
 */

#include <linux/devm-helpers.h>
#include <linux/extcon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define SM5705_REG_INT1			0x00
#define SM5705_REG_INTMSK1		0x04
#define SM5705_REG_STATUS1		0x08
#define SM5705_REG_STATUS2		0x09
#define SM5705_REG_STATUS3		0x0a
#define SM5705_REG_CNTL			0x0c
#define SM5705_REG_VBUSCNTL		0x0d
#define SM5705_REG_CHGCNTL2		0x10
#define SM5705_REG_CHGCNTL4		0x12
#define SM5705_REG_CHGCNTL5		0x13
#define SM5705_REG_DEVICE_ID		0x37

/* STATUS1 / INT1 */
#define SM5705_VBUSPOK			BIT(0)
#define SM5705_VBUSUVLO			BIT(1)
#define SM5705_VBUSOVP			BIT(2)
/* STATUS2 / INT2 */
#define SM5705_BATOVP			BIT(1)
#define SM5705_CHGON			BIT(3)
#define SM5705_TOPOFF			BIT(5)
#define SM5705_DONE			BIT(6)
/* STATUS3 / INT3 */
#define SM5705_THEMSHDN			BIT(1)
#define SM5705_PRETMROFF		BIT(4)
#define SM5705_FASTTMROFF		BIT(5)

#define SM5705_CNTL_MODE_MASK		GENMASK(2, 0)
#define SM5705_CNTL_MODE_CHG_OFF	4
#define SM5705_CNTL_MODE_CHG_ON		5

#define SM5705_VBUSLIMIT_MASK		GENMASK(6, 0)
#define SM5705_FASTCHG_MASK		GENMASK(5, 0)
#define SM5705_BATREG_MASK		GENMASK(5, 0)
#define SM5705_TOPOFF_MASK		GENMASK(3, 0)

/* Input current limits per USB charger type */
#define SM5705_SDP_INPUT_LIMIT_UA	500000	/* USB 2.0 */
#define SM5705_CDP_INPUT_LIMIT_UA	1500000	/* BC1.2 */
#define SM5705_DCP_INPUT_LIMIT_UA	1800000	/* Samsung's wall charger setting */
#define SM5705_FAST_INPUT_LIMIT_UA	1650000	/* Samsung's 9 V AFC setting */

/*
 * Battery temperature guard, in tenths of a degree, after Samsung's
 * firmware: no charging outside the battery's alert range (0-50 C unless
 * the battery node says otherwise), 4.10 V and 1.35 A when warm, 0.9 A
 * when cool. Each zone is left only past its recovery point.
 */
#define SM5705_TEMP_ALERT_MIN		0
#define SM5705_TEMP_ALERT_MAX		500
#define SM5705_TEMP_ALERT_HYST		50
#define SM5705_TEMP_WARM		410
#define SM5705_TEMP_WARM_HYST		20
#define SM5705_TEMP_COOL		150
#define SM5705_TEMP_COOL_HYST		50
#define SM5705_WARM_VOLTAGE_UV		4100000
#define SM5705_WARM_CURRENT_UA		1350000
#define SM5705_COOL_CURRENT_UA		900000
#define SM5705_TEMP_POLL		(30 * HZ)

enum sm5705_temp_zone {
	SM5705_ZONE_NORMAL,
	SM5705_ZONE_COOL,
	SM5705_ZONE_WARM,
	SM5705_ZONE_COLD,
	SM5705_ZONE_HOT,
};

/* BATREG: 0 is 3.8 V, 1 is 4.0 V, then 4.01 V in 10 mV steps */
#define SM5705_BATREG_MIN_UV		4010000
#define SM5705_BATREG_MAX_UV		4620000

struct sm5705_chg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply_battery_info *bat;
	struct extcon_dev *edev;
	struct notifier_block extcon_nb;
	struct work_struct cable_work;
	int usb_type;

	/* battery temperature guard */
	struct mutex lock;
	struct delayed_work temp_work;
	enum sm5705_temp_zone zone;
	int temp_min, temp_max;		/* tenths of a degree */
	int cfg_voltage, cfg_current;	/* limits outside the guard */
};

/* VBUS input current limit: 100 mA + 25 mA steps */
static int sm5705_chg_get_input_limit(struct sm5705_chg *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, SM5705_REG_VBUSCNTL, &val);
	if (ret)
		return ret;

	*ua = 100000 + (val & SM5705_VBUSLIMIT_MASK) * 25000;
	return 0;
}

static int sm5705_chg_set_input_limit(struct sm5705_chg *chg, int ua)
{
	unsigned int val;

	if (ua < 100000)
		return -EINVAL;

	val = min((ua - 100000) / 25000, 0x7f);
	return regmap_update_bits(chg->regmap, SM5705_REG_VBUSCNTL,
				  SM5705_VBUSLIMIT_MASK, val);
}

/* fast charge current: 100 mA + 50 mA steps, at least 200 mA */
static int sm5705_chg_get_charge_current(struct sm5705_chg *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, SM5705_REG_CHGCNTL2, &val);
	if (ret)
		return ret;

	*ua = 100000 + (val & SM5705_FASTCHG_MASK) * 50000;
	return 0;
}

static int sm5705_chg_set_charge_current(struct sm5705_chg *chg, int ua)
{
	unsigned int val;

	if (chg->bat && chg->bat->constant_charge_current_max_ua > 0 &&
	    ua > chg->bat->constant_charge_current_max_ua)
		return -EINVAL;
	if (ua < 200000)
		return -EINVAL;

	val = min((ua - 100000) / 50000, 0x3f);
	return regmap_update_bits(chg->regmap, SM5705_REG_CHGCNTL2,
				  SM5705_FASTCHG_MASK, val);
}

static int sm5705_chg_get_charge_voltage(struct sm5705_chg *chg, int *uv)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, SM5705_REG_CHGCNTL4, &val);
	if (ret)
		return ret;

	val &= SM5705_BATREG_MASK;
	if (val == 0)
		*uv = 3800000;
	else if (val == 1)
		*uv = 4000000;
	else
		*uv = SM5705_BATREG_MIN_UV + (val - 2) * 10000;
	return 0;
}

static int sm5705_chg_set_charge_voltage(struct sm5705_chg *chg, int uv)
{
	unsigned int val;

	if (chg->bat && chg->bat->constant_charge_voltage_max_uv > 0 &&
	    uv > chg->bat->constant_charge_voltage_max_uv)
		return -EINVAL;
	if (uv < SM5705_BATREG_MIN_UV || uv > SM5705_BATREG_MAX_UV)
		return -EINVAL;

	val = (uv - SM5705_BATREG_MIN_UV) / 10000 + 2;
	return regmap_update_bits(chg->regmap, SM5705_REG_CHGCNTL4,
				  SM5705_BATREG_MASK, val);
}

/* top-off (termination) current: 100 mA + 25 mA steps */
static int sm5705_chg_get_term_current(struct sm5705_chg *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, SM5705_REG_CHGCNTL5, &val);
	if (ret)
		return ret;

	*ua = 100000 + (val & SM5705_TOPOFF_MASK) * 25000;
	return 0;
}

static int sm5705_chg_set_term_current(struct sm5705_chg *chg, int ua)
{
	unsigned int val;

	if (ua < 100000)
		return -EINVAL;

	val = min((ua - 100000) / 25000, 0xf);
	return regmap_update_bits(chg->regmap, SM5705_REG_CHGCNTL5,
				  SM5705_TOPOFF_MASK, val);
}

static int sm5705_chg_read_status(struct sm5705_chg *chg, u8 status[3])
{
	return regmap_bulk_read(chg->regmap, SM5705_REG_STATUS1, status, 3);
}

static int sm5705_chg_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sm5705_chg *chg = power_supply_get_drvdata(psy);
	u8 st[3];
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return sm5705_chg_get_input_limit(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return sm5705_chg_get_charge_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return sm5705_chg_get_charge_voltage(chg, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		return sm5705_chg_get_term_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = chg->usb_type;
		return 0;
	default:
		break;
	}

	ret = sm5705_chg_read_status(chg, st);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!(st[0] & SM5705_VBUSPOK);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!(st[0] & SM5705_VBUSPOK))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (chg->zone == SM5705_ZONE_HOT ||
			 chg->zone == SM5705_ZONE_COLD)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (st[1] & (SM5705_TOPOFF | SM5705_DONE))
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (st[1] & SM5705_CHGON)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if ((st[0] & SM5705_VBUSOVP) || (st[1] & SM5705_BATOVP))
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		else if (st[2] & SM5705_THEMSHDN)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (st[2] & (SM5705_PRETMROFF | SM5705_FASTTMROFF))
			val->intval = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
		else if (chg->zone == SM5705_ZONE_HOT)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (chg->zone == SM5705_ZONE_COLD)
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		else if (chg->zone == SM5705_ZONE_WARM)
			val->intval = POWER_SUPPLY_HEALTH_WARM;
		else if (chg->zone == SM5705_ZONE_COOL)
			val->intval = POWER_SUPPLY_HEALTH_COOL;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void sm5705_chg_apply_zone(struct sm5705_chg *chg);

static int sm5705_chg_set_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct sm5705_chg *chg = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return sm5705_chg_set_input_limit(chg, val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		return sm5705_chg_set_term_current(chg, val->intval);
	default:
		break;
	}

	/* charge voltage and current: the guard may hold them lower */
	mutex_lock(&chg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = sm5705_chg_set_charge_current(chg, val->intval);
		if (!ret)
			chg->cfg_current = val->intval;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = sm5705_chg_set_charge_voltage(chg, val->intval);
		if (!ret)
			chg->cfg_voltage = val->intval;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		sm5705_chg_apply_zone(chg);
	mutex_unlock(&chg->lock);

	return ret;
}

static int sm5705_chg_property_is_writeable(struct power_supply *psy,
					    enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		return 1;
	default:
		return 0;
	}
}

static const enum power_supply_property sm5705_chg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
};

static const struct power_supply_desc sm5705_chg_desc = {
	.name = "sm5705-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP),
	.properties = sm5705_chg_props,
	.num_properties = ARRAY_SIZE(sm5705_chg_props),
	.get_property = sm5705_chg_get_property,
	.set_property = sm5705_chg_set_property,
	.property_is_writeable = sm5705_chg_property_is_writeable,
};

static irqreturn_t sm5705_chg_irq(int irq, void *data)
{
	struct sm5705_chg *chg = data;
	u8 ints[4];

	/* reading the interrupt registers clears them */
	if (regmap_bulk_read(chg->regmap, SM5705_REG_INT1, ints, sizeof(ints)))
		return IRQ_NONE;

	dev_dbg(chg->dev, "interrupt %*ph\n", (int)sizeof(ints), ints);
	power_supply_changed(chg->psy);

	return IRQ_HANDLED;
}

static void sm5705_chg_cable_work(struct work_struct *work)
{
	struct sm5705_chg *chg = container_of(work, struct sm5705_chg,
					      cable_work);
	int type, limit = 0;

	if (extcon_get_state(chg->edev, EXTCON_CHG_USB_FAST) > 0) {
		type = POWER_SUPPLY_USB_TYPE_DCP;
		limit = SM5705_FAST_INPUT_LIMIT_UA;
	} else if (extcon_get_state(chg->edev, EXTCON_CHG_USB_DCP) > 0) {
		type = POWER_SUPPLY_USB_TYPE_DCP;
		limit = SM5705_DCP_INPUT_LIMIT_UA;
	} else if (extcon_get_state(chg->edev, EXTCON_CHG_USB_CDP) > 0) {
		type = POWER_SUPPLY_USB_TYPE_CDP;
		limit = SM5705_CDP_INPUT_LIMIT_UA;
	} else if (extcon_get_state(chg->edev, EXTCON_CHG_USB_SDP) > 0) {
		type = POWER_SUPPLY_USB_TYPE_SDP;
		limit = SM5705_SDP_INPUT_LIMIT_UA;
	} else {
		type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	}

	/* nothing attached: keep the limit for the next cable */
	if (limit && sm5705_chg_set_input_limit(chg, limit))
		dev_err(chg->dev, "failed to set input limit %d uA\n", limit);

	dev_dbg(chg->dev, "usb type %d, input limit %d uA\n", type, limit);
	chg->usb_type = type;
	power_supply_changed(chg->psy);
}

static int sm5705_chg_extcon_notifier(struct notifier_block *nb,
				      unsigned long event, void *ptr)
{
	struct sm5705_chg *chg = container_of(nb, struct sm5705_chg,
					      extcon_nb);

	schedule_work(&chg->cable_work);
	return NOTIFY_OK;
}

static const char * const sm5705_zone_names[] = {
	[SM5705_ZONE_NORMAL] = "normal",
	[SM5705_ZONE_COOL] = "cool, 0.9 A",
	[SM5705_ZONE_WARM] = "warm, 4.10 V and 1.35 A",
	[SM5705_ZONE_COLD] = "too cold, not charging",
	[SM5705_ZONE_HOT] = "too hot, not charging",
};

/* Write the limits for the current zone; called with chg->lock held */
static void sm5705_chg_apply_zone(struct sm5705_chg *chg)
{
	int voltage = chg->cfg_voltage, current_ua = chg->cfg_current;
	unsigned int mode, cntl;
	bool charge = true;

	switch (chg->zone) {
	case SM5705_ZONE_HOT:
	case SM5705_ZONE_COLD:
		charge = false;
		break;
	case SM5705_ZONE_WARM:
		voltage = min(voltage, SM5705_WARM_VOLTAGE_UV);
		current_ua = min(current_ua, SM5705_WARM_CURRENT_UA);
		break;
	case SM5705_ZONE_COOL:
		current_ua = min(current_ua, SM5705_COOL_CURRENT_UA);
		break;
	default:
		break;
	}

	if (voltage > 0)
		sm5705_chg_set_charge_voltage(chg, voltage);
	if (current_ua > 0)
		sm5705_chg_set_charge_current(chg, current_ua);

	/* only switch between charging on and off, leave other modes alone */
	if (regmap_read(chg->regmap, SM5705_REG_CNTL, &cntl))
		return;
	mode = cntl & SM5705_CNTL_MODE_MASK;
	if (mode != SM5705_CNTL_MODE_CHG_ON && mode != SM5705_CNTL_MODE_CHG_OFF)
		return;
	regmap_update_bits(chg->regmap, SM5705_REG_CNTL, SM5705_CNTL_MODE_MASK,
			   charge ? SM5705_CNTL_MODE_CHG_ON :
				    SM5705_CNTL_MODE_CHG_OFF);
}

static enum sm5705_temp_zone sm5705_chg_temp_zone(struct sm5705_chg *chg,
						  int temp)
{
	enum sm5705_temp_zone zone = chg->zone;

	if (temp >= chg->temp_max ||
	    (zone == SM5705_ZONE_HOT &&
	     temp >= chg->temp_max - SM5705_TEMP_ALERT_HYST))
		return SM5705_ZONE_HOT;
	if (temp < chg->temp_min ||
	    (zone == SM5705_ZONE_COLD &&
	     temp <= chg->temp_min + SM5705_TEMP_ALERT_HYST))
		return SM5705_ZONE_COLD;
	if (temp >= SM5705_TEMP_WARM ||
	    ((zone == SM5705_ZONE_WARM || zone == SM5705_ZONE_HOT) &&
	     temp >= SM5705_TEMP_WARM - SM5705_TEMP_WARM_HYST))
		return SM5705_ZONE_WARM;
	if (temp < SM5705_TEMP_COOL ||
	    ((zone == SM5705_ZONE_COOL || zone == SM5705_ZONE_COLD) &&
	     temp <= SM5705_TEMP_COOL + SM5705_TEMP_COOL_HYST))
		return SM5705_ZONE_COOL;

	return SM5705_ZONE_NORMAL;
}

struct sm5705_bat_temp {
	struct sm5705_chg *chg;
	int temp;
};

/* Battery temperature from the battery this charger supplies */
static int sm5705_chg_read_bat_temp(struct power_supply *psy, void *data)
{
	struct sm5705_bat_temp *bt = data;
	union power_supply_propval val;
	size_t i;

	if (psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
		return 0;

	for (i = 0; i < psy->num_supplies; i++) {
		if (strcmp(psy->supplied_from[i], bt->chg->psy->desc->name))
			continue;
		if (power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val))
			return 0;
		bt->temp = val.intval;
		return 1;
	}

	return 0;
}

static void sm5705_chg_temp_work(struct work_struct *work)
{
	struct sm5705_chg *chg = container_of(work, struct sm5705_chg,
					      temp_work.work);
	struct sm5705_bat_temp bt = { .chg = chg };
	enum sm5705_temp_zone zone;
	bool found;

	found = power_supply_for_each_psy(&bt, sm5705_chg_read_bat_temp) > 0;
	if (found) {
		mutex_lock(&chg->lock);
		zone = sm5705_chg_temp_zone(chg, bt.temp);
		if (zone != chg->zone) {
			dev_info(chg->dev, "battery at %d.%d C: %s\n",
				 bt.temp / 10, abs(bt.temp % 10),
				 sm5705_zone_names[zone]);
			chg->zone = zone;
			sm5705_chg_apply_zone(chg);
			mutex_unlock(&chg->lock);
			power_supply_changed(chg->psy);
		} else {
			mutex_unlock(&chg->lock);
		}
	}

	/* the battery may register after the charger */
	schedule_delayed_work(&chg->temp_work, found ? SM5705_TEMP_POLL : 5 * HZ);
}

/* Battery limits from the monitored-battery node, as stock sets them */
static int sm5705_chg_apply_battery_info(struct sm5705_chg *chg)
{
	struct power_supply_battery_info *bat = chg->bat;
	int ret;

	if (bat->constant_charge_voltage_max_uv > 0) {
		ret = sm5705_chg_set_charge_voltage(chg,
				bat->constant_charge_voltage_max_uv);
		if (ret)
			return ret;
	}

	if (bat->constant_charge_current_max_ua > 0) {
		ret = sm5705_chg_set_charge_current(chg,
				bat->constant_charge_current_max_ua);
		if (ret)
			return ret;
	}

	if (bat->charge_term_current_ua > 0) {
		ret = sm5705_chg_set_term_current(chg,
				bat->charge_term_current_ua);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct regmap_config sm5705_chg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x3e,
};

static int sm5705_chg_probe(struct i2c_client *client)
{
	/* VBUS good/UVLO/OVP; battery OVP, charging, top-off, done; thermal, timers */
	static const u8 int_mask[4] = { 0xf8, 0x95, 0xcd, 0xff };
	struct power_supply_config cfg = {};
	struct device *dev = &client->dev;
	struct sm5705_chg *chg;
	unsigned int id;
	u8 ints[4];
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->dev = dev;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	chg->temp_min = SM5705_TEMP_ALERT_MIN;
	chg->temp_max = SM5705_TEMP_ALERT_MAX;

	ret = devm_mutex_init(dev, &chg->lock);
	if (ret)
		return ret;

	if (of_property_present(dev->of_node, "extcon")) {
		chg->edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(chg->edev))
			return dev_err_probe(dev, PTR_ERR(chg->edev),
					     "failed to get extcon device\n");
	}

	chg->regmap = devm_regmap_init_i2c(client, &sm5705_chg_regmap_config);
	if (IS_ERR(chg->regmap))
		return dev_err_probe(dev, PTR_ERR(chg->regmap),
				     "failed to init regmap\n");

	ret = regmap_read(chg->regmap, SM5705_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device id\n");

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(dev);
	chg->psy = devm_power_supply_register(dev, &sm5705_chg_desc, &cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, PTR_ERR(chg->psy),
				     "failed to register power supply\n");

	ret = power_supply_get_battery_info(chg->psy, &chg->bat);
	if (ret) {
		dev_warn(dev, "no battery info, keeping the charger settings: %d\n",
			 ret);
		chg->bat = NULL;
	} else {
		ret = sm5705_chg_apply_battery_info(chg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to apply battery limits\n");
		/* alert-celsius in the battery node, in degrees */
		if (chg->bat->temp_alert_min > INT_MIN)
			chg->temp_min = chg->bat->temp_alert_min * 10;
		if (chg->bat->temp_alert_max < INT_MAX)
			chg->temp_max = chg->bat->temp_alert_max * 10;
	}

	ret = sm5705_chg_get_charge_voltage(chg, &chg->cfg_voltage);
	if (!ret)
		ret = sm5705_chg_get_charge_current(chg, &chg->cfg_current);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read charge limits\n");

	/* start from the normal zone, whatever an earlier run left */
	mutex_lock(&chg->lock);
	sm5705_chg_apply_zone(chg);
	mutex_unlock(&chg->lock);

	ret = devm_delayed_work_autocancel(dev, &chg->temp_work,
					   sm5705_chg_temp_work);
	if (ret)
		return ret;
	schedule_delayed_work(&chg->temp_work, 0);

	if (client->irq) {
		/* clear what the bootloader left, then unmask our events */
		ret = regmap_bulk_read(chg->regmap, SM5705_REG_INT1, ints,
				       sizeof(ints));
		if (!ret)
			ret = regmap_bulk_write(chg->regmap, SM5705_REG_INTMSK1,
						int_mask, sizeof(int_mask));
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set up interrupts\n");

		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						sm5705_chg_irq, IRQF_ONESHOT,
						"sm5705-charger", chg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request interrupt\n");
	}

	if (chg->edev) {
		ret = devm_work_autocancel(dev, &chg->cable_work,
					   sm5705_chg_cable_work);
		if (ret)
			return ret;

		chg->extcon_nb.notifier_call = sm5705_chg_extcon_notifier;
		ret = devm_extcon_register_notifier_all(dev, chg->edev,
							&chg->extcon_nb);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register extcon notifier\n");

		schedule_work(&chg->cable_work);
	}

	dev_info(dev, "SM5705 charger, id %#04x\n", id);
	return 0;
}

static const struct of_device_id sm5705_chg_of_match[] = {
	{ .compatible = "siliconmitus,sm5705-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5705_chg_of_match);

static struct i2c_driver sm5705_chg_driver = {
	.driver = {
		.name = "sm5705-charger",
		.of_match_table = sm5705_chg_of_match,
	},
	.probe = sm5705_chg_probe,
};
module_i2c_driver(sm5705_chg_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5705 charger driver");
MODULE_LICENSE("GPL");
