// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S2MM005 USB Type-C and Power Delivery controller
 *
 * The S2MM005 ("CCIC" in Samsung's kernels) runs the Type-C state machine
 * and the PD policy engine in its own firmware: as a sink it accepts the
 * source's capabilities and requests the first one, 5 V, by itself. With
 * sink-pdos on the connector, the driver asks it for the capability with
 * the most power at one of those voltages instead. This driver reports
 * what the firmware negotiated: the attach state, the current the source
 * advertises on CC, and the PD contract. A power supply carries the
 * contract to the charger.
 *
 * The firmware also attaches as the source to a sink, and swaps roles when
 * the partner asks, but VBUS comes from elsewhere, and it offers its
 * capabilities only once the driver has started its policy manager. On a
 * dual role connector the driver switches the connector's VBUS supply and
 * the USB role switch to follow the firmware's power and data roles.
 *
 * Registers have 16-bit addresses. Register 0x10 takes commands; the
 * firmware's state words and PD message buffers sit at fixed addresses.
 * Based on the Samsung vendor driver (drivers/ccic/s2mm005*.c).
 *
 * Copyright (C) 2015 Samsung Electronics
 * Copyright (C) 2026 psigod <psigod@example.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/pd.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>

#define S2MM005_REG_CMD			0x0010
#define S2MM005_REG_FUNC_STATE		0x0020
#define S2MM005_REG_INT_STATUS		0x0030
#define S2MM005_REG_LP_STATE		0x0060
#define S2MM005_REG_TX_REQUEST		0x0240
#define S2MM005_REG_RX_SRC_CAPS		0x0260
#define S2MM005_REG_MAX			0x02ff

/* after S2MM005_CMD_FLASH_READ, the low addresses read the firmware version */
#define S2MM005_REG_FW_BOOT		0x0008
#define S2MM005_REG_FW_MAIN		0x0009

#define S2MM005_CMD_INT_CLEAR		0x01
#define S2MM005_CMD_FLASH_READ		0xaa
/* two-byte command: automatic low power mode while detached */
#define S2MM005_CMD_LPM			0x0f
#define S2MM005_LPM_AUTO		0x06
/* options: a sub-command, and an argument for some */
#define S2MM005_CMD_OPTION		0x03
#define S2MM005_OPT_NEXT_STATE		0x02	/* argument: a PD_STATE */
#define S2MM005_OPT_SELECT_PDO		0x03	/* argument: object position */
#define S2MM005_OPT_DPM_START		0x42	/* start the policy manager */

/* FUNC_STATE, little endian */
#define S2MM005_FUNC_PD_STATE		GENMASK(7, 0)
#define S2MM005_FUNC_ATTACHED		BIT(24)
#define S2MM005_FUNC_SOURCE		BIT(25)
#define S2MM005_FUNC_DFP		BIT(26)
#define S2MM005_FUNC_RP_LEVEL		GENMASK(28, 27)
#define S2MM005_FUNC_RESET		BIT(31)

/* LP_STATE, little endian */
#define S2MM005_LP_AUTO_LPM		BIT(0)
#define S2MM005_LP_WATER		BIT(3)

/* PD_STATE values of the firmware's policy engine */
#define S2MM005_PE_SNK_WAIT_CAPS	17
#define S2MM005_PE_SNK_READY		21
/* power role swap: our VBUS off before the partner's, or on after it */
#define S2MM005_PE_PRS_SRC_SNK_OFF	52
#define S2MM005_PE_PRS_SNK_SRC_ON	64

/* RP_LEVEL: the source's pull-up, i.e. the current it allows without PD */
#define S2MM005_RP_DEFAULT		1
#define S2MM005_RP_1_5A			2
#define S2MM005_RP_3_0A			3

/* PD message buffers: a 32-bit header word, then up to 7 data objects */
#define S2MM005_MSG_LEN			32

#define S2MM005_DEFAULT_UA		500000

/*
 * Ask for a better capability only this long after a contract: a dock
 * swaps data roles about 200 ms after its first one and does not try again
 * if a new negotiation is in its way.
 */
#define S2MM005_SELECT_DELAY		msecs_to_jiffies(1000)

struct s2mm005 {
	struct device *dev;
	struct regmap *regmap;

	struct typec_port *port;
	struct typec_partner *partner;
	struct power_supply *psy;
	struct usb_role_switch *role_sw;
	struct device_node *vbus_np;	/* the connector, which names VBUS */
	bool can_source, can_host;	/* the connector's roles */
	u32 snk_pdo[PDO_MAX_OBJECTS];	/* the connector's sink-pdos */
	unsigned int nr_snk_pdo;
	struct delayed_work select_work;

	struct mutex update_lock;	/* the chip, and up to lock */
	struct regulator *vbus;		/* held while VBUS is on */
	enum usb_role role;
	u16 caps_header;		/* of the capabilities last looked at */

	struct mutex lock;		/* the state below, for the supply */
	bool attached;			/* to a source, which powers us */
	bool pd;
	enum typec_pwr_opmode opmode;
	int voltage_uv;
	int current_ua;
};

static const struct regmap_config s2mm005_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = S2MM005_REG_MAX,
};

static int s2mm005_read32(struct s2mm005 *s, unsigned int reg, u32 *val)
{
	__le32 buf;
	int ret;

	ret = regmap_bulk_read(s->regmap, reg, &buf, sizeof(buf));
	if (ret)
		return ret;

	*val = le32_to_cpu(buf);
	return 0;
}

static int s2mm005_cmd(struct s2mm005 *s, const u8 *cmd, size_t len)
{
	return regmap_raw_write(s->regmap, S2MM005_REG_CMD, cmd, len);
}

/* The firmware sleeps between events; a few reads wake it up for commands */
static void s2mm005_wake(struct s2mm005 *s)
{
	unsigned int val;
	int i;

	for (i = 0; i < 5; i++)
		regmap_read(s->regmap, S2MM005_REG_FW_BOOT, &val);
}

/*
 * As a source, the firmware sends its capabilities only after the AP has
 * started its policy manager. Until then it waits in
 * SRC_Wait_New_Capabilities, and once VBUS comes up it takes that for the
 * partner's and detaches. A firmware restart forgets the request.
 */
static void s2mm005_start_dpm(struct s2mm005 *s)
{
	static const u8 cmd[] = { S2MM005_CMD_OPTION, S2MM005_OPT_DPM_START };

	if (!s->can_source)
		return;

	s2mm005_wake(s);
	s2mm005_cmd(s, cmd, sizeof(cmd));
}

/* The source's capabilities and the request the firmware sent */
struct s2mm005_caps {
	u16 header;
	unsigned int cnt;
	u32 pdo[PDO_MAX_OBJECTS];
	u32 rdo;
};

/*
 * Read the PD contract: the source's capabilities and the request. Only
 * fixed supplies are requested, the firmware's own choice and ours.
 */
static int s2mm005_read_caps(struct s2mm005 *s, struct s2mm005_caps *caps)
{
	__le32 msg[S2MM005_MSG_LEN / 4], req[2];
	unsigned int i, pos;
	int ret;

	ret = regmap_bulk_read(s->regmap, S2MM005_REG_TX_REQUEST, req,
			       sizeof(req));
	if (ret)
		return ret;
	ret = regmap_bulk_read(s->regmap, S2MM005_REG_RX_SRC_CAPS, msg,
			       sizeof(msg));
	if (ret)
		return ret;

	caps->header = le32_to_cpu(msg[0]) & 0xffff;
	caps->cnt = min(pd_header_cnt(caps->header), PDO_MAX_OBJECTS);
	for (i = 0; i < caps->cnt; i++)
		caps->pdo[i] = le32_to_cpu(msg[i + 1]);
	caps->rdo = le32_to_cpu(req[1]);

	pos = rdo_index(caps->rdo);
	if (!pos || pos > caps->cnt ||
	    pdo_type(caps->pdo[pos - 1]) != PDO_TYPE_FIXED)
		return -EINVAL;

	return 0;
}

/*
 * The fixed supply with the most power at one of the sink-pdos voltages,
 * the higher voltage of two with the same power, as TCPM picks; 0 if none
 */
static unsigned int s2mm005_best_pdo(struct s2mm005 *s,
				     const struct s2mm005_caps *caps)
{
	unsigned int i, j, mv, mw, best = 0, best_mv = 0, best_mw = 0;

	for (i = 0; i < caps->cnt; i++) {
		if (pdo_type(caps->pdo[i]) != PDO_TYPE_FIXED)
			continue;

		mv = pdo_fixed_voltage(caps->pdo[i]);
		mw = mv * pdo_max_current(caps->pdo[i]) / 1000;
		for (j = 0; j < s->nr_snk_pdo; j++) {
			if (pdo_type(s->snk_pdo[j]) != PDO_TYPE_FIXED ||
			    pdo_fixed_voltage(s->snk_pdo[j]) != mv)
				continue;
			if (mw > best_mw || (mw == best_mw && mv > best_mv)) {
				best = i + 1;
				best_mv = mv;
				best_mw = mw;
			}
		}
	}

	return best;
}

/*
 * Have the firmware request the best capability, with update_lock held.
 * It picks the first one after every detach by itself.
 */
static void s2mm005_select_pdo(struct s2mm005 *s,
			       const struct s2mm005_caps *caps)
{
	u8 cmd[3] = { S2MM005_CMD_OPTION };
	unsigned int best;

	best = s2mm005_best_pdo(s, caps);
	if (!best || best == rdo_index(caps->rdo))
		return;

	dev_dbg(s->dev, "requesting capability %u\n", best);
	cmd[1] = S2MM005_OPT_SELECT_PDO;
	cmd[2] = best;
	s2mm005_wake(s);
	s2mm005_cmd(s, cmd, sizeof(cmd));
	/* and negotiate again */
	cmd[1] = S2MM005_OPT_NEXT_STATE;
	cmd[2] = S2MM005_PE_SNK_WAIT_CAPS;
	s2mm005_wake(s);
	s2mm005_cmd(s, cmd, sizeof(cmd));
}

/*
 * The charger that provides VBUS takes this port's supply as its input
 * (power-supplies), so it can only probe after us and is unbound before us.
 * Look VBUS up when it is switched on and let it go when it is off again.
 * Unbinding the charger while we source VBUS still pulls the supply away.
 */
static void s2mm005_set_vbus(struct s2mm005 *s, bool on)
{
	struct regulator *vbus;
	int ret;

	if (on == !!s->vbus)
		return;

	if (!on) {
		ret = regulator_disable(s->vbus);
		if (ret) {
			dev_err(s->dev, "failed to switch VBUS off: %d\n", ret);
			return;
		}
		regulator_put(s->vbus);
		s->vbus = NULL;
		dev_info(s->dev, "VBUS off\n");
		return;
	}

	vbus = of_regulator_get_optional(s->dev, s->vbus_np, "vbus");
	if (IS_ERR(vbus)) {
		dev_err(s->dev, "no VBUS supply: %pe\n", vbus);
		return;
	}

	ret = regulator_enable(vbus);
	if (ret) {
		dev_err(s->dev, "failed to switch VBUS on: %d\n", ret);
		regulator_put(vbus);
		return;
	}

	s->vbus = vbus;
	dev_info(s->dev, "VBUS on\n");
}

static void s2mm005_set_role(struct s2mm005 *s, enum usb_role role)
{
	int ret;

	if (role == s->role)
		return;

	ret = usb_role_switch_set_role(s->role_sw, role);
	if (ret) {
		dev_err(s->dev, "failed to set USB role %s: %d\n",
			usb_role_string(role), ret);
		return;
	}

	s->role = role;
}

static void s2mm005_update(struct s2mm005 *s)
{
	enum typec_pwr_opmode opmode = TYPEC_PWR_MODE_USB;
	struct typec_partner_desc desc = { };
	int voltage_uv = 5000000, current_ua = S2MM005_DEFAULT_UA;
	bool connected, source, attached, vbus, pd = false, changed;
	enum usb_role role = USB_ROLE_NONE;
	struct s2mm005_caps caps;
	unsigned int pd_state, pos;
	u16 pd_rev = 0;
	u32 func, lp;
	u8 cmd[2];
	int ret;

	ret = s2mm005_read32(s, S2MM005_REG_FUNC_STATE, &func);
	if (ret) {
		dev_err(s->dev, "failed to read state: %d\n", ret);
		return;
	}
	dev_dbg(s->dev, "function state %#010x\n", func);

	/* the firmware restarted: set it up again */
	if (func & S2MM005_FUNC_RESET) {
		s2mm005_start_dpm(s);
		/* and let it save power while detached */
		if (!s2mm005_read32(s, S2MM005_REG_LP_STATE, &lp) &&
		    !(lp & S2MM005_LP_AUTO_LPM)) {
			dev_dbg(s->dev, "firmware reset, enabling low power mode\n");
			cmd[0] = S2MM005_CMD_LPM;
			cmd[1] = S2MM005_LPM_AUTO;
			s2mm005_wake(s);
			s2mm005_cmd(s, cmd, sizeof(cmd));
		}
	}

	pd_state = FIELD_GET(S2MM005_FUNC_PD_STATE, func);
	connected = func & S2MM005_FUNC_ATTACHED;
	source = connected && (func & S2MM005_FUNC_SOURCE);
	/* without the source role, a sink on the other end gets nothing */
	if (source && !s->can_source) {
		connected = false;
		source = false;
	}
	attached = connected && !source;

	/* across a power role swap too: ours off first, or the partner's */
	vbus = s->can_source &&
	       ((source && pd_state != S2MM005_PE_PRS_SRC_SNK_OFF) ||
		pd_state == S2MM005_PE_PRS_SNK_SRC_ON);

	if (connected)
		role = (s->can_host && (func & S2MM005_FUNC_DFP)) ?
		       USB_ROLE_HOST : USB_ROLE_DEVICE;

	/* VBUS before the host starts, and the host stopped before VBUS */
	if (vbus)
		s2mm005_set_vbus(s, true);
	s2mm005_set_role(s, role);
	if (!vbus)
		s2mm005_set_vbus(s, false);

	if (!attached) {
		s->caps_header = 0;
		cancel_delayed_work(&s->select_work);
	}

	if (attached && pd_state == S2MM005_PE_SNK_READY &&
	    !s2mm005_read_caps(s, &caps)) {
		pd = true;
		opmode = TYPEC_PWR_MODE_PD;
		pos = rdo_index(caps.rdo);
		voltage_uv = pdo_fixed_voltage(caps.pdo[pos - 1]) * 1000;
		current_ua = rdo_op_current(caps.rdo) * 1000;
		/* specification revision: 0 is 1.0, 1 is 2.0, 2 is 3.0 */
		pd_rev = (pd_header_rev(caps.header) + 1) << 8;
		/*
		 * look at each Source_Capabilities message once (the header
		 * holds its message ID): a request the source turned down is
		 * not repeated
		 */
		if (s->nr_snk_pdo && caps.header != s->caps_header) {
			s->caps_header = caps.header;
			schedule_delayed_work(&s->select_work,
					      S2MM005_SELECT_DELAY);
		}
	} else if (attached) {
		switch (FIELD_GET(S2MM005_FUNC_RP_LEVEL, func)) {
		case S2MM005_RP_3_0A:
			opmode = TYPEC_PWR_MODE_3_0A;
			current_ua = 3000000;
			break;
		case S2MM005_RP_1_5A:
			opmode = TYPEC_PWR_MODE_1_5A;
			current_ua = 1500000;
			break;
		default:
			break;
		}
	}

	mutex_lock(&s->lock);

	changed = attached != s->attached || pd != s->pd ||
		  opmode != s->opmode || voltage_uv != s->voltage_uv ||
		  current_ua != s->current_ua;

	/* the PD contract usually follows the attach: register the partner again */
	if (s->partner && (!connected || pd != s->pd)) {
		typec_unregister_partner(s->partner);
		s->partner = NULL;
	}

	if (connected && !s->partner) {
		desc.usb_pd = pd;
		desc.pd_revision = pd_rev;
		s->partner = typec_register_partner(s->port, &desc);
		if (IS_ERR(s->partner)) {
			dev_err(s->dev, "failed to register partner: %ld\n",
				PTR_ERR(s->partner));
			s->partner = NULL;
		}
	}

	typec_set_pwr_role(s->port, source ? TYPEC_SOURCE : TYPEC_SINK);
	typec_set_data_role(s->port, role == USB_ROLE_HOST ? TYPEC_HOST :
							     TYPEC_DEVICE);
	typec_set_pwr_opmode(s->port, opmode);

	s->attached = attached;
	s->pd = pd;
	s->opmode = opmode;
	s->voltage_uv = voltage_uv;
	s->current_ua = current_ua;

	mutex_unlock(&s->lock);

	if (changed) {
		if (attached)
			dev_info(s->dev, "%s: %d mV, %d mA\n",
				 pd ? "PD contract" : "Type-C source",
				 voltage_uv / 1000, current_ua / 1000);
		else
			dev_dbg(s->dev, "detached\n");
		power_supply_changed(s->psy);
	}
}

static irqreturn_t s2mm005_irq(int irq, void *data)
{
	struct s2mm005 *s = data;
	u8 status[20], cmd = S2MM005_CMD_INT_CLEAR;
	int ret;

	/* sync, control, data and extended message flags, then the summary */
	ret = regmap_bulk_read(s->regmap, S2MM005_REG_INT_STATUS, status,
			       sizeof(status));
	if (ret)
		return IRQ_NONE;

	dev_dbg(s->dev, "interrupt %*ph\n", (int)sizeof(status), status);
	mutex_lock(&s->update_lock);
	s2mm005_cmd(s, &cmd, 1);
	s2mm005_update(s);
	mutex_unlock(&s->update_lock);

	return IRQ_HANDLED;
}

static void s2mm005_select_work(struct work_struct *work)
{
	struct s2mm005 *s = container_of(to_delayed_work(work), struct s2mm005,
					 select_work);
	struct s2mm005_caps caps;
	u32 func;

	guard(mutex)(&s->update_lock);

	/* still the contract that scheduled us */
	if (s2mm005_read32(s, S2MM005_REG_FUNC_STATE, &func) ||
	    !(func & S2MM005_FUNC_ATTACHED) || (func & S2MM005_FUNC_SOURCE) ||
	    FIELD_GET(S2MM005_FUNC_PD_STATE, func) != S2MM005_PE_SNK_READY ||
	    s2mm005_read_caps(s, &caps) || caps.header != s->caps_header)
		return;

	s2mm005_select_pdo(s, &caps);
}

static void s2mm005_cancel_select(void *data)
{
	struct s2mm005 *s = data;

	cancel_delayed_work_sync(&s->select_work);
}

static const enum power_supply_property s2mm005_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int s2mm005_psy_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct s2mm005 *s = power_supply_get_drvdata(psy);

	guard(mutex)(&s->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = s->attached;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = s->pd ? POWER_SUPPLY_USB_TYPE_PD :
				      POWER_SUPPLY_USB_TYPE_C;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = s->attached ? s->voltage_uv : 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = s->attached ? s->current_ua : 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mm005_psy_register(struct s2mm005 *s)
{
	struct power_supply_config cfg = {
		.drv_data = s,
		.fwnode = dev_fwnode(s->dev),
	};
	struct power_supply_desc *desc;

	desc = devm_kzalloc(s->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->name = devm_kasprintf(s->dev, GFP_KERNEL, "s2mm005-source-%s",
				    dev_name(s->dev));
	if (!desc->name)
		return -ENOMEM;

	desc->type = POWER_SUPPLY_TYPE_USB;
	desc->usb_types = BIT(POWER_SUPPLY_USB_TYPE_C) |
			  BIT(POWER_SUPPLY_USB_TYPE_PD);
	desc->properties = s2mm005_psy_props;
	desc->num_properties = ARRAY_SIZE(s2mm005_psy_props);
	desc->get_property = s2mm005_psy_get_property;

	s->psy = devm_power_supply_register(s->dev, desc, &cfg);

	return PTR_ERR_OR_ZERO(s->psy);
}

static void s2mm005_unregister_port(void *data)
{
	struct s2mm005 *s = data;

	s2mm005_set_role(s, USB_ROLE_NONE);
	s2mm005_set_vbus(s, false);
	typec_unregister_partner(s->partner);
	typec_unregister_port(s->port);
}

static void s2mm005_put_role_sw(void *data)
{
	usb_role_switch_put(data);
}

static void s2mm005_put_vbus_np(void *data)
{
	of_node_put(data);
}

/*
 * The roles the connector allows: sink and device unless it says more. The
 * source role needs a VBUS supply, the host role a USB role switch.
 */
static int s2mm005_get_connector(struct s2mm005 *s,
				 struct fwnode_handle *connector,
				 struct typec_capability *cap)
{
	const char *str;
	int ret;

	cap->type = TYPEC_PORT_SNK;
	cap->data = TYPEC_PORT_UFP;
	if (!connector)
		return 0;

	if (!fwnode_property_read_string(connector, "power-role", &str)) {
		ret = typec_find_port_power_role(str);
		if (ret < 0)
			return dev_err_probe(s->dev, ret, "bad power-role %s\n",
					     str);
		cap->type = ret;
	}

	if (!fwnode_property_read_string(connector, "data-role", &str)) {
		ret = typec_find_port_data_role(str);
		if (ret < 0)
			return dev_err_probe(s->dev, ret, "bad data-role %s\n",
					     str);
		cap->data = ret;
	}

	if (cap->type != TYPEC_PORT_SNK &&
	    fwnode_property_present(connector, "vbus-supply")) {
		s->vbus_np = of_node_get(to_of_node(connector));
		ret = devm_add_action_or_reset(s->dev, s2mm005_put_vbus_np,
					       s->vbus_np);
		if (ret)
			return ret;
		s->can_source = true;
	}

	/* without sink-pdos the firmware's choice stands */
	ret = fwnode_property_count_u32(connector, "sink-pdos");
	if (ret > 0) {
		s->nr_snk_pdo = min(ret, PDO_MAX_OBJECTS);
		ret = fwnode_property_read_u32_array(connector, "sink-pdos",
						     s->snk_pdo,
						     s->nr_snk_pdo);
		if (ret)
			return dev_err_probe(s->dev, ret,
					     "bad sink-pdos\n");
	}

	if (cap->data != TYPEC_PORT_UFP) {
		s->role_sw = fwnode_usb_role_switch_get(connector);
		if (IS_ERR(s->role_sw))
			return dev_err_probe(s->dev, PTR_ERR(s->role_sw),
					     "failed to get role switch\n");
		ret = devm_add_action_or_reset(s->dev, s2mm005_put_role_sw,
					       s->role_sw);
		if (ret)
			return ret;
		s->can_host = !!s->role_sw;
	}

	return 0;
}

static int s2mm005_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_capability cap = { };
	struct fwnode_handle *connector;
	unsigned int boot;
	u8 main[3], cmd;
	struct s2mm005 *s;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = dev;
	s->opmode = TYPEC_PWR_MODE_USB;
	ret = devm_mutex_init(dev, &s->lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &s->update_lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&s->select_work, s2mm005_select_work);

	s->regmap = devm_regmap_init_i2c(client, &s2mm005_regmap_config);
	if (IS_ERR(s->regmap))
		return dev_err_probe(dev, PTR_ERR(s->regmap),
				     "failed to init regmap\n");

	s2mm005_wake(s);
	cmd = S2MM005_CMD_FLASH_READ;
	ret = s2mm005_cmd(s, &cmd, 1);
	if (!ret)
		ret = regmap_read(s->regmap, S2MM005_REG_FW_BOOT, &boot);
	if (!ret)
		ret = s2mm005_cmd(s, &cmd, 1);
	if (!ret)
		ret = regmap_bulk_read(s->regmap, S2MM005_REG_FW_MAIN, main,
				       sizeof(main));
	if (ret)
		return dev_err_probe(dev, ret, "no answer\n");

	ret = s2mm005_psy_register(s);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register supply\n");

	connector = device_get_named_child_node(dev, "connector");
	ret = s2mm005_get_connector(s, connector, &cap);
	if (ret) {
		fwnode_handle_put(connector);
		return ret;
	}

	cap.revision = USB_TYPEC_REV_1_2;
	cap.pd_revision = 0x300;
	cap.fwnode = connector;

	s->port = typec_register_port(dev, &cap);
	fwnode_handle_put(connector);
	if (IS_ERR(s->port))
		return dev_err_probe(dev, PTR_ERR(s->port),
				     "failed to register port\n");

	ret = devm_add_action_or_reset(dev, s2mm005_unregister_port, s);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, s2mm005_cancel_select, s);
	if (ret)
		return ret;

	mutex_lock(&s->update_lock);
	cmd = S2MM005_CMD_INT_CLEAR;
	s2mm005_cmd(s, &cmd, 1);
	s2mm005_start_dpm(s);
	s2mm005_update(s);
	mutex_unlock(&s->update_lock);

	ret = devm_request_threaded_irq(dev, client->irq, NULL, s2mm005_irq,
					IRQF_ONESHOT, "s2mm005", s);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	dev_info(dev, "S2MM005, firmware %02x boot %d\n", main[0], boot);

	return 0;
}

static const struct of_device_id s2mm005_of_match[] = {
	{ .compatible = "samsung,s2mm005" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mm005_of_match);

static struct i2c_driver s2mm005_driver = {
	.driver = {
		.name = "s2mm005",
		.of_match_table = s2mm005_of_match,
	},
	.probe = s2mm005_probe,
};
module_i2c_driver(s2mm005_driver);

MODULE_DESCRIPTION("Samsung S2MM005 USB Type-C and PD controller driver");
MODULE_LICENSE("GPL");
