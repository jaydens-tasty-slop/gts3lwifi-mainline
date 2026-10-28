/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Silicon Mitus SM5705 MUIC: interface for the SM5705 charger driver
 */

#ifndef __LINUX_EXTCON_SM5705_MUIC_H
#define __LINUX_EXTCON_SM5705_MUIC_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device_node;

#if IS_REACHABLE(CONFIG_EXTCON_SM5705)
int sm5705_muic_hold_5v(struct device_node *np, bool hold);
#else
static inline int sm5705_muic_hold_5v(struct device_node *np, bool hold)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_EXTCON_SM5705_MUIC_H */
