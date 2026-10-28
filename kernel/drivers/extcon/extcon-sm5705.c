// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5705 MUIC (USB charger type detection)
 *
 * The MUIC of the SM5705 charger and fuel gauge IC runs BC1.2 detection on
 * the USB data lines and reports the result in its CHGTYPE register. This
 * driver turns that into extcon USB and charger cable states. The register
 * layout differs from the SM5502 family: a third interrupt register at 0x05
 * moves the masks, ADC and device type registers.
 *
 * Accessory (ID resistor) detection, the manual switch and Samsung's AFC
 * high voltage charging are not supported. Register layout from the
 * Samsung vendor driver (muic_regmap_sm5705.c).
 */

#include <linux/extcon-provider.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define SM5705_MUIC_REG_DEVICE_ID	0x01
#define SM5705_MUIC_REG_CONTROL		0x02
#define SM5705_MUIC_REG_INT1		0x03
#define SM5705_MUIC_REG_INTMASK1	0x06
#define SM5705_MUIC_REG_RSVD_ID1	0x15
#define SM5705_MUIC_REG_CHG_TYPE	0x17

#define SM5705_MUIC_CONTROL_MASK_INT	BIT(0)
#define SM5705_MUIC_RSVD_ID1_VBUS_VALID	BIT(0)
#define SM5705_MUIC_CHG_TYPE_MASK	GENMASK(4, 0)

/* CHG_TYPE values */
#define SM5705_MUIC_CHG_TYPE_DCP	0x01
#define SM5705_MUIC_CHG_TYPE_CDP	0x02
#define SM5705_MUIC_CHG_TYPE_SDP	0x04
#define SM5705_MUIC_CHG_TYPE_DCD_OUT	0x08	/* data contact timeout */
#define SM5705_MUIC_CHG_TYPE_U200	0x10
#define SM5705_MUIC_CHG_TYPE_AFC	0x11
#define SM5705_MUIC_CHG_TYPE_QC20	0x13

static const unsigned int sm5705_muic_cables[] = {
	EXTCON_USB,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_CDP,
	EXTCON_NONE,
};

struct sm5705_muic {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *edev;
	struct mutex lock;
};

static int sm5705_muic_detect(struct sm5705_muic *muic)
{
	bool usb = false, sdp = false, dcp = false, cdp = false;
	unsigned int vbus, type;
	int ret;

	ret = regmap_read(muic->regmap, SM5705_MUIC_REG_RSVD_ID1, &vbus);
	if (ret)
		return ret;
	ret = regmap_read(muic->regmap, SM5705_MUIC_REG_CHG_TYPE, &type);
	if (ret)
		return ret;
	type &= SM5705_MUIC_CHG_TYPE_MASK;

	if (vbus & SM5705_MUIC_RSVD_ID1_VBUS_VALID) {
		switch (type) {
		case SM5705_MUIC_CHG_TYPE_SDP:
		case SM5705_MUIC_CHG_TYPE_DCD_OUT:
			usb = sdp = true;
			break;
		case SM5705_MUIC_CHG_TYPE_CDP:
			usb = cdp = true;
			break;
		case SM5705_MUIC_CHG_TYPE_DCP:
		case SM5705_MUIC_CHG_TYPE_U200:
		case SM5705_MUIC_CHG_TYPE_AFC:
		case SM5705_MUIC_CHG_TYPE_QC20:
			dcp = true;
			break;
		default:
			/* detection still running, the attach interrupt follows */
			break;
		}
	}

	dev_dbg(muic->dev, "vbus %#04x chg type %#04x\n", vbus, type);

	/* charger type first, so consumers of EXTCON_USB see it */
	extcon_set_state_sync(muic->edev, EXTCON_CHG_USB_SDP, sdp);
	extcon_set_state_sync(muic->edev, EXTCON_CHG_USB_DCP, dcp);
	extcon_set_state_sync(muic->edev, EXTCON_CHG_USB_CDP, cdp);
	extcon_set_state_sync(muic->edev, EXTCON_USB, usb);

	return 0;
}

static irqreturn_t sm5705_muic_irq(int irq, void *data)
{
	struct sm5705_muic *muic = data;
	u8 ints[3];
	int ret;

	mutex_lock(&muic->lock);

	/* reading the interrupt registers clears them */
	ret = regmap_bulk_read(muic->regmap, SM5705_MUIC_REG_INT1, ints,
			       sizeof(ints));
	if (!ret) {
		dev_dbg(muic->dev, "interrupt %*ph\n", (int)sizeof(ints), ints);
		sm5705_muic_detect(muic);
	}

	mutex_unlock(&muic->lock);

	return ret ? IRQ_NONE : IRQ_HANDLED;
}

static const struct regmap_config sm5705_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x3b,
};

static int sm5705_muic_probe(struct i2c_client *client)
{
	/* attach/detach; VBUS on, ADC change, VBUS off; no AFC events */
	static const u8 int_mask[3] = { 0xfc, 0x7a, 0xff };
	struct device *dev = &client->dev;
	struct sm5705_muic *muic;
	unsigned int id;
	u8 ints[3];
	int ret;

	muic = devm_kzalloc(dev, sizeof(*muic), GFP_KERNEL);
	if (!muic)
		return -ENOMEM;
	muic->dev = dev;

	ret = devm_mutex_init(dev, &muic->lock);
	if (ret)
		return ret;

	muic->regmap = devm_regmap_init_i2c(client, &sm5705_muic_regmap_config);
	if (IS_ERR(muic->regmap))
		return dev_err_probe(dev, PTR_ERR(muic->regmap),
				     "failed to init regmap\n");

	ret = regmap_read(muic->regmap, SM5705_MUIC_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device id\n");

	muic->edev = devm_extcon_dev_allocate(dev, sm5705_muic_cables);
	if (IS_ERR(muic->edev))
		return dev_err_probe(dev, PTR_ERR(muic->edev),
				     "failed to allocate extcon device\n");

	ret = devm_extcon_dev_register(dev, muic->edev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register extcon device\n");

	/* clear what the bootloader left, then unmask our events */
	ret = regmap_bulk_read(muic->regmap, SM5705_MUIC_REG_INT1, ints,
			       sizeof(ints));
	if (!ret)
		ret = regmap_bulk_write(muic->regmap, SM5705_MUIC_REG_INTMASK1,
					int_mask, sizeof(int_mask));
	if (!ret)
		ret = regmap_clear_bits(muic->regmap, SM5705_MUIC_REG_CONTROL,
					SM5705_MUIC_CONTROL_MASK_INT);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up interrupts\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					sm5705_muic_irq, IRQF_ONESHOT,
					"sm5705-muic", muic);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request interrupt\n");

	mutex_lock(&muic->lock);
	ret = sm5705_muic_detect(muic);
	mutex_unlock(&muic->lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to detect cable\n");

	dev_info(dev, "SM5705 MUIC, id %#04x\n", id);
	return 0;
}

static const struct of_device_id sm5705_muic_of_match[] = {
	{ .compatible = "siliconmitus,sm5705-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5705_muic_of_match);

static struct i2c_driver sm5705_muic_driver = {
	.driver = {
		.name = "sm5705-muic",
		.of_match_table = sm5705_muic_of_match,
	},
	.probe = sm5705_muic_probe,
};
module_i2c_driver(sm5705_muic_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5705 MUIC extcon driver");
MODULE_LICENSE("GPL");
