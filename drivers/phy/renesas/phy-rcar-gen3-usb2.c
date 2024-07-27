#define fin() pr_err("%s(): in\n", __func__)
#define fout(...) pr_err("%s(): out (%d)\n", __func__, ##__VA_ARGS__)
// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R-Car Gen3 for USB2.0 PHY driver
 *
 * Copyright (C) 2015-2017 Renesas Electronics Corporation
 *
 * This is based on the phy-rcar-gen2 driver:
 * Copyright (C) 2014 Renesas Solutions Corp.
 * Copyright (C) 2014 Cogent Embedded, Inc.
 */

#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/usb/of.h>
#include <linux/workqueue.h>

/******* USB2.0 Host registers (original offset is +0x200) *******/
#define USB2_INT_ENABLE		0x000
#define USB2_USBCTR		0x00c
#define USB2_SPD_RSM_TIMSET	0x10c
#define USB2_OC_TIMSET		0x110
#define USB2_COMMCTRL		0x600
#define USB2_OBINTSTA		0x604
#define USB2_OBINTEN		0x608
#define USB2_VBCTRL		0x60c
#define USB2_LINECTRL1		0x610
#define USB2_ADPCTRL		0x630

/* INT_ENABLE */
#define USB2_INT_ENABLE_UCOM_INTEN	BIT(3)
#define USB2_INT_ENABLE_USBH_INTB_EN	BIT(2)	/* For EHCI */
#define USB2_INT_ENABLE_USBH_INTA_EN	BIT(1)	/* For OHCI */

/* USBCTR */
#define USB2_USBCTR_DIRPD	BIT(2)
#define USB2_USBCTR_PLL_RST	BIT(1)

/* SPD_RSM_TIMSET */
#define USB2_SPD_RSM_TIMSET_INIT	0x014e029b

/* OC_TIMSET */
#define USB2_OC_TIMSET_INIT		0x000209ab

/* COMMCTRL */
#define USB2_COMMCTRL_OTG_PERI		BIT(31)	/* 1 = Peripheral mode */

/* OBINTSTA and OBINTEN */
#define USB2_OBINT_SESSVLDCHG		BIT(12)
#define USB2_OBINT_IDDIGCHG		BIT(11)
#define USB2_OBINT_BITS			(USB2_OBINT_SESSVLDCHG | \
					 USB2_OBINT_IDDIGCHG)

/* VBCTRL */
#define USB2_VBCTRL_OCCLREN		BIT(16)
#define USB2_VBCTRL_DRVVBUSSEL		BIT(8)
#define USB2_VBCTRL_VBOUT		BIT(0)

/* LINECTRL1 */
#define USB2_LINECTRL1_DPRPD_EN		BIT(19)
#define USB2_LINECTRL1_DP_RPD		BIT(18)
#define USB2_LINECTRL1_DMRPD_EN		BIT(17)
#define USB2_LINECTRL1_DM_RPD		BIT(16)
#define USB2_LINECTRL1_OPMODE_NODRV	BIT(6)

/* ADPCTRL */
#define USB2_ADPCTRL_OTGSESSVLD		BIT(20)
#define USB2_ADPCTRL_IDDIG		BIT(19)
#define USB2_ADPCTRL_IDPULLUP		BIT(5)	/* 1 = ID sampling is enabled */
#define USB2_ADPCTRL_DRVVBUS		BIT(4)

/*  RZ/G2L specific */
#define USB2_OBINT_IDCHG_EN		BIT(0)
#define USB2_LINECTRL1_USB2_IDMON	BIT(0)

#define NUM_OF_PHYS			4
enum rcar_gen3_phy_index {
	PHY_INDEX_BOTH_HC,
	PHY_INDEX_OHCI,
	PHY_INDEX_EHCI,
	PHY_INDEX_HSUSB
};

static const u32 rcar_gen3_int_enable[NUM_OF_PHYS] = {
	USB2_INT_ENABLE_USBH_INTB_EN | USB2_INT_ENABLE_USBH_INTA_EN,
	USB2_INT_ENABLE_USBH_INTA_EN,
	USB2_INT_ENABLE_USBH_INTB_EN,
	0
};

struct rcar_gen3_phy {
	struct phy *phy;
	struct rcar_gen3_chan *ch;
	u32 int_enable_bits;
	bool initialized;
	bool otg_initialized;
	bool powered;
};

struct rcar_gen3_chan {
	void __iomem *base;
	struct device *dev;	/* platform_device's device */
	struct extcon_dev *extcon;
	struct rcar_gen3_phy rphys[NUM_OF_PHYS];
	struct regulator *vbus;
	struct work_struct work;
	struct mutex lock;	/* protects rphys[...].powered */
	enum usb_dr_mode dr_mode;
	int irq;
	u32 obint_enable_bits;
	bool extcon_host;
	bool is_otg_channel;
	bool uses_otg_pins;
	bool soc_no_adp_ctrl;
	enum phy_mode role;
};

struct rcar_gen3_phy_drv_data {
	const struct phy_ops *phy_usb2_ops;
	bool no_adp_ctrl;
};

/*
 * Combination about is_otg_channel and uses_otg_pins:
 *
 * Parameters				|| Behaviors
 * is_otg_channel	| uses_otg_pins	|| irqs		| role sysfs
 * ---------------------+---------------++--------------+------------
 * true			| true		|| enabled	| enabled
 * true                 | false		|| disabled	| enabled
 * false                | any		|| disabled	| disabled
 */

static void rcar_gen3_phy_usb2_work(struct work_struct *work)
{
	struct rcar_gen3_chan *ch = container_of(work, struct rcar_gen3_chan,
						 work);
fin();

	if (ch->extcon_host) {
		extcon_set_state_sync(ch->extcon, EXTCON_USB_HOST, true);
		extcon_set_state_sync(ch->extcon, EXTCON_USB, false);
	} else {
		extcon_set_state_sync(ch->extcon, EXTCON_USB_HOST, false);
		extcon_set_state_sync(ch->extcon, EXTCON_USB, true);
	}
fout(0);
}

static void rcar_gen3_set_host_mode(struct rcar_gen3_chan *ch, int host)
{
	void __iomem *usb2_base = ch->base;
	u32 val = readl(usb2_base + USB2_COMMCTRL);
fin();

	pr_err("%s: %08x, %d\n", __func__, val, host);
//dump_stack();

	if (host)
		val &= ~USB2_COMMCTRL_OTG_PERI;
	else
		val |= USB2_COMMCTRL_OTG_PERI;
	writel(val, usb2_base + USB2_COMMCTRL);
fout(0);
}

static void rcar_gen3_set_linectrl(struct rcar_gen3_chan *ch, int dp, int dm)
{
	void __iomem *usb2_base = ch->base;
	u32 val = readl(usb2_base + USB2_LINECTRL1);
fin();

	dev_vdbg(ch->dev, "%s: %08x, %d, %d\n", __func__, val, dp, dm);
	val &= ~(USB2_LINECTRL1_DP_RPD | USB2_LINECTRL1_DM_RPD);
	if (dp)
		val |= USB2_LINECTRL1_DP_RPD;
	if (dm)
		val |= USB2_LINECTRL1_DM_RPD;
	writel(val, usb2_base + USB2_LINECTRL1);
fout(0);
}

static void rcar_gen3_enable_vbus_ctrl(struct rcar_gen3_chan *ch, int vbus)
{
	void __iomem *usb2_base = ch->base;
	u32 vbus_ctrl_reg = USB2_ADPCTRL;
	u32 vbus_ctrl_val = USB2_ADPCTRL_DRVVBUS;
	u32 val;
fin();

	dev_vdbg(ch->dev, "%s: %08x, %d\n", __func__, val, vbus);
	if (ch->soc_no_adp_ctrl) {
		if (ch->vbus) {
			pr_err("%s(): hardware enable\n", __func__);
			regulator_hardware_enable(ch->vbus, vbus);
		}

		vbus_ctrl_reg = USB2_VBCTRL;
		vbus_ctrl_val = USB2_VBCTRL_VBOUT;
	}

	val = readl(usb2_base + vbus_ctrl_reg);
	if (vbus)
		val |= vbus_ctrl_val;
	else
		val &= ~vbus_ctrl_val;
	writel(val, usb2_base + vbus_ctrl_reg);
fout(0);
}

static void rcar_gen3_control_otg_irq(struct rcar_gen3_chan *ch, int enable)
{
	void __iomem *usb2_base = ch->base;
	u32 val = readl(usb2_base + USB2_OBINTEN);
fin();

	if (ch->uses_otg_pins && enable)
		val |= ch->obint_enable_bits;
	else
		val &= ~ch->obint_enable_bits;
	writel(val, usb2_base + USB2_OBINTEN);
fout(0);
}

static void rcar_gen3_init_for_host(struct rcar_gen3_chan *ch)
{
fin();
	rcar_gen3_set_linectrl(ch, 1, 1);
	rcar_gen3_set_host_mode(ch, 1);
	rcar_gen3_enable_vbus_ctrl(ch, 1);

	ch->extcon_host = true;
	schedule_work(&ch->work);
fout(0);
}

static void rcar_gen3_init_for_peri(struct rcar_gen3_chan *ch)
{
fin();
	rcar_gen3_set_linectrl(ch, 0, 1);
	rcar_gen3_set_host_mode(ch, 0);
	rcar_gen3_enable_vbus_ctrl(ch, 0);

	ch->extcon_host = false;
	schedule_work(&ch->work);
fout(0);
}

static void rcar_gen3_init_for_b_host(struct rcar_gen3_chan *ch)
{
	void __iomem *usb2_base = ch->base;
	u32 val;
fin();

	val = readl(usb2_base + USB2_LINECTRL1);
	writel(val | USB2_LINECTRL1_OPMODE_NODRV, usb2_base + USB2_LINECTRL1);

	rcar_gen3_set_linectrl(ch, 1, 1);
	rcar_gen3_set_host_mode(ch, 1);
	rcar_gen3_enable_vbus_ctrl(ch, 0);

	val = readl(usb2_base + USB2_LINECTRL1);
	writel(val & ~USB2_LINECTRL1_OPMODE_NODRV, usb2_base + USB2_LINECTRL1);
fout(0);
}

static void rcar_gen3_init_for_a_peri(struct rcar_gen3_chan *ch)
{
fin();
	rcar_gen3_set_linectrl(ch, 0, 1);
	rcar_gen3_set_host_mode(ch, 0);
	rcar_gen3_enable_vbus_ctrl(ch, 1);
fout(0);
}

static void rcar_gen3_init_from_a_peri_to_a_host(struct rcar_gen3_chan *ch)
{
fin();
	rcar_gen3_control_otg_irq(ch, 0);

	rcar_gen3_enable_vbus_ctrl(ch, 1);
	rcar_gen3_init_for_host(ch);

	rcar_gen3_control_otg_irq(ch, 1);
fout(0);
}

static bool rcar_gen3_check_id(struct rcar_gen3_chan *ch)
{
fin();
	if (!ch->uses_otg_pins)
{
fout(0);
		return (ch->dr_mode == USB_DR_MODE_HOST) ? false : true;
}

	if (ch->soc_no_adp_ctrl)
{
fout(1);
		return !!(readl(ch->base + USB2_LINECTRL1) & USB2_LINECTRL1_USB2_IDMON);
}

fout(2);
	return !!(readl(ch->base + USB2_ADPCTRL) & USB2_ADPCTRL_IDDIG);
}

/* Need to protect with spin lock to avoid concurrency w/ IRQ context. */
static void rcar_gen3_device_recognition(struct rcar_gen3_chan *ch)
{
	bool device;

fin();

	pr_err("%s(): dev=%s, LINECTR1=%08x, in IRQ=%d\n", __func__, ch->dev->of_node->full_name,
			readl(ch->base + USB2_LINECTRL1), in_hardirq());

	if (pm_suspend_target_state != PM_SUSPEND_ON && !in_hardirq())
		device = ch->role == PHY_MODE_USB_DEVICE;
	else
		device = rcar_gen3_check_id(ch);

	if (device) {
		rcar_gen3_init_for_peri(ch);
		ch->role = PHY_MODE_USB_DEVICE;
	} else {
		rcar_gen3_init_for_host(ch);
		ch->role = PHY_MODE_USB_HOST;
	}
fout(0);
}

static bool rcar_gen3_is_host(struct rcar_gen3_chan *ch)
{
fin();
	return !(readl(ch->base + USB2_COMMCTRL) & USB2_COMMCTRL_OTG_PERI);
fout(0);
}

static enum phy_mode rcar_gen3_get_phy_mode(struct rcar_gen3_chan *ch)
{
fin();
	if (rcar_gen3_is_host(ch))
{
fout(0);
		return PHY_MODE_USB_HOST;
}

fout(1);
	return PHY_MODE_USB_DEVICE;
}

static bool rcar_gen3_is_any_rphy_initialized(struct rcar_gen3_chan *ch)
{
	int i;
fin();

	for (i = 0; i < NUM_OF_PHYS; i++) {
		if (ch->rphys[i].initialized)
{
fout(0);
			return true;
}
	}

fout(1);
	return false;
}

static bool rcar_gen3_needs_init_otg(struct rcar_gen3_chan *ch)
{
	int i;
fin();

	for (i = 0; i < NUM_OF_PHYS; i++) {
		if (ch->rphys[i].otg_initialized)
{
fout(0);
			return false;
}
	}

fout(1);
	return true;
}

static bool rcar_gen3_are_all_rphys_power_off(struct rcar_gen3_chan *ch)
{
	int i;
fin();

	for (i = 0; i < NUM_OF_PHYS; i++) {
		if (ch->rphys[i].powered)
{
fout(0);
			return false;
}
	}

fout(1);
	return true;
}

static void rcar_gen3_config_default(struct device *dev)
{
	struct rcar_gen3_chan *ch = dev_get_drvdata(dev);
	bool is_b_device;
	enum phy_mode cur_mode, new_mode;

	/* is_b_device: true is B-Device. false is A-Device. */
	is_b_device = rcar_gen3_check_id(ch);
	cur_mode = rcar_gen3_get_phy_mode(ch);

	/* If current and new mode is the same, this returns the error */
	if (cur_mode == PHY_MODE_USB_DEVICE)
{
fout(2);
		return;
}

	mutex_lock(&ch->lock);
	if (!is_b_device)	/* A-Host */
		rcar_gen3_init_for_a_peri(ch);
	else			/* B-Host */
		rcar_gen3_init_for_peri(ch);

	ch->role = PHY_MODE_USB_DEVICE;
	mutex_unlock(&ch->lock);

}
static ssize_t role_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct rcar_gen3_chan *ch = dev_get_drvdata(dev);
	bool is_b_device;
	enum phy_mode cur_mode, new_mode;
fin();

	if (!ch->is_otg_channel || !rcar_gen3_is_any_rphy_initialized(ch))
{
fout(0);
		return -EIO;
}

	if (sysfs_streq(buf, "host"))
		new_mode = PHY_MODE_USB_HOST;
	else if (sysfs_streq(buf, "peripheral"))
		new_mode = PHY_MODE_USB_DEVICE;
	else
{
fout(1);
		return -EINVAL;
}

	/* is_b_device: true is B-Device. false is A-Device. */
	is_b_device = rcar_gen3_check_id(ch);
	cur_mode = rcar_gen3_get_phy_mode(ch);

	/* If current and new mode is the same, this returns the error */
	if (cur_mode == new_mode)
{
fout(2);
		return -EINVAL;
}

	mutex_lock(&ch->lock);
	if (new_mode == PHY_MODE_USB_HOST) { /* And is_host must be false */
		if (!is_b_device)	/* A-Peripheral */
			rcar_gen3_init_from_a_peri_to_a_host(ch);
		else			/* B-Peripheral */
			rcar_gen3_init_for_b_host(ch);
	} else {			/* And is_host must be true */
		if (!is_b_device)	/* A-Host */
			rcar_gen3_init_for_a_peri(ch);
		else			/* B-Host */
			rcar_gen3_init_for_peri(ch);
	}

	ch->role = new_mode;
	mutex_unlock(&ch->lock);

fout(3);
	return count;
}

static ssize_t role_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct rcar_gen3_chan *ch = dev_get_drvdata(dev);
fin();

	if (!ch->is_otg_channel || !rcar_gen3_is_any_rphy_initialized(ch))
{
fout(0);
		return -EIO;
}

	return sprintf(buf, "%s\n", rcar_gen3_is_host(ch) ? "host" :
							    "peripheral");
}
static DEVICE_ATTR_RW(role);

static void rcar_gen3_init_otg(struct rcar_gen3_chan *ch)
{
	void __iomem *usb2_base = ch->base;
	u32 val;
fin();

	pr_err("%s(): dev=%s\n", __func__, ch->dev->of_node->full_name);
	/* Should not use functions of read-modify-write a register */
	val = readl(usb2_base + USB2_LINECTRL1);
	val = (val & ~USB2_LINECTRL1_DP_RPD) | USB2_LINECTRL1_DPRPD_EN |
	      USB2_LINECTRL1_DMRPD_EN | USB2_LINECTRL1_DM_RPD;
	writel(val, usb2_base + USB2_LINECTRL1);

	if (!ch->soc_no_adp_ctrl) {
		val = readl(usb2_base + USB2_VBCTRL);
		val &= ~USB2_VBCTRL_OCCLREN;
		writel(val | USB2_VBCTRL_DRVVBUSSEL, usb2_base + USB2_VBCTRL);
		val = readl(usb2_base + USB2_ADPCTRL);
		writel(val | USB2_ADPCTRL_IDPULLUP, usb2_base + USB2_ADPCTRL);
	}
//	wmb();
	pr_err("%s(): before sleep: dev=%s\n", __func__, ch->dev->of_node->full_name);
	msleep(20);

	pr_err("%s(): after sleep: dev=%s\n", __func__, ch->dev->of_node->full_name);

	val = readl(usb2_base + USB2_OBINTSTA);
	val |= BIT(17) | BIT(16) | BIT(6) | BIT(5) | BIT(4) | BIT(1) | BIT(0);
	writel(val, usb2_base + USB2_OBINTSTA);
	writel(ch->obint_enable_bits, usb2_base + USB2_OBINTEN);

	rcar_gen3_device_recognition(ch);
fout(0);
}

static irqreturn_t rcar_gen3_phy_usb2_irq(int irq, void *_ch)
{
	struct rcar_gen3_chan *ch = _ch;
	void __iomem *usb2_base = ch->base;
	u32 status = readl(usb2_base + USB2_OBINTSTA);
	irqreturn_t ret = IRQ_NONE;
fin();

	if (status & ch->obint_enable_bits) {
		dev_vdbg(ch->dev, "%s: %08x\n", __func__, status);
		writel(ch->obint_enable_bits, usb2_base + USB2_OBINTSTA);
		rcar_gen3_device_recognition(ch);
		ret = IRQ_HANDLED;
	}

fout(0);
	return ret;
}

static int rcar_gen3_phy_usb2_init(struct phy *p)
{
	struct rcar_gen3_phy *rphy = phy_get_drvdata(p);
	struct rcar_gen3_chan *channel = rphy->ch;
	void __iomem *usb2_base = channel->base;
	u32 val;
	int ret;
fin();


	pr_err("%s(): dev=%s, phy=%s\n", __func__, channel->dev->of_node->full_name,
			rphy->phy->dev.of_node->full_name);
//dump_stack();

	mutex_lock(&channel->lock);

	if (!rcar_gen3_is_any_rphy_initialized(channel) && channel->irq >= 0) {
		INIT_WORK(&channel->work, rcar_gen3_phy_usb2_work);
		ret = request_irq(channel->irq, rcar_gen3_phy_usb2_irq,
				  IRQF_SHARED, dev_name(channel->dev), channel);
		if (ret < 0) {
			dev_err(channel->dev, "No irq handler (%d)\n", channel->irq);
fout(0);
			goto unlock;
		}
	}

	/* Initialize USB2 part */
	val = readl(usb2_base + USB2_INT_ENABLE);
	val |= USB2_INT_ENABLE_UCOM_INTEN | rphy->int_enable_bits;
	writel(val, usb2_base + USB2_INT_ENABLE);

	if (rcar_gen3_is_any_rphy_initialized(channel)) {
		if (channel->is_otg_channel) {
			rphy->otg_initialized = true;
			goto set_init;
		}
	}

	writel(USB2_SPD_RSM_TIMSET_INIT, usb2_base + USB2_SPD_RSM_TIMSET);
	writel(USB2_OC_TIMSET_INIT, usb2_base + USB2_OC_TIMSET);

	/* Initialize otg part */
	if (channel->is_otg_channel) {
		if (rcar_gen3_needs_init_otg(channel))
			rcar_gen3_init_otg(channel);
		rphy->otg_initialized = true;
	}

set_init:
	rphy->initialized = true;

unlock:
	mutex_unlock(&channel->lock);
fout(1);
	return 0;
}

static int rcar_gen3_phy_usb2_exit(struct phy *p)
{
	struct rcar_gen3_phy *rphy = phy_get_drvdata(p);
	struct rcar_gen3_chan *channel = rphy->ch;
	void __iomem *usb2_base = channel->base;
	u32 val;
fin();

	pr_err("%s(): dev=%s\n", __func__, channel->dev->of_node->full_name);

	mutex_lock(&channel->lock);

	rphy->initialized = false;
	if (channel->is_otg_channel)
		rphy->otg_initialized = false;

	val = readl(usb2_base + USB2_INT_ENABLE);
	val &= ~rphy->int_enable_bits;
	if (rcar_gen3_is_any_rphy_initialized(channel)) {
		writel(val, usb2_base + USB2_INT_ENABLE);
		goto unlock;
	}

	val &= ~USB2_INT_ENABLE_UCOM_INTEN;
	writel(val, usb2_base + USB2_INT_ENABLE);
	if (channel->irq >= 0)
		free_irq(channel->irq, channel);

unlock:
	mutex_unlock(&channel->lock);

fout(0);
	return 0;
}

static int rcar_gen3_phy_usb2_power_on(struct phy *p)
{
	struct rcar_gen3_phy *rphy = phy_get_drvdata(p);
	struct rcar_gen3_chan *channel = rphy->ch;
	void __iomem *usb2_base = channel->base;
	u32 val;
	int ret = 0;
	static int cnt = 0;
fin();

	pr_err("%s(): dev=%s\n", __func__, channel->dev->of_node->full_name);
//dump_stack();

	mutex_lock(&channel->lock);
	if (!rcar_gen3_are_all_rphys_power_off(channel))
		goto out;

	if (channel->vbus) {
		pr_err("%s(): regulator ena %d\n", __func__, cnt++);
		ret = regulator_enable(channel->vbus);
		if (ret)
			goto out;
	}

	if (0 && !rcar_gen3_are_all_rphys_power_off(channel))
		goto out;

	val = readl(usb2_base + USB2_USBCTR);
	val |= USB2_USBCTR_PLL_RST;
	writel(val, usb2_base + USB2_USBCTR);
	val &= ~USB2_USBCTR_PLL_RST;
	writel(val, usb2_base + USB2_USBCTR);

out:
	/* The powered flag should be set for any other phys anyway */
	rphy->powered = true;
unlock:
	mutex_unlock(&channel->lock);

fout(0);
	return 0;
}

static int rcar_gen3_phy_usb2_power_off(struct phy *p)
{
	struct rcar_gen3_phy *rphy = phy_get_drvdata(p);
	struct rcar_gen3_chan *channel = rphy->ch;
	int ret = 0;
	static int cnt = 0;
fin();

	pr_err("%s(): dev=%s\n", __func__, channel->dev->of_node->full_name);
	mutex_lock(&channel->lock);
	rphy->powered = false;

	if (!rcar_gen3_are_all_rphys_power_off(channel))
		goto out;

	if (channel->vbus) {
		pr_err("%s(): regulator dis %d\n", __func__, cnt++);

		ret = regulator_disable(channel->vbus);
	}
out:
	mutex_unlock(&channel->lock);

fout(0);
	return ret;
}

static const struct phy_ops rcar_gen3_phy_usb2_ops = {
	.init		= rcar_gen3_phy_usb2_init,
	.exit		= rcar_gen3_phy_usb2_exit,
	.power_on	= rcar_gen3_phy_usb2_power_on,
	.power_off	= rcar_gen3_phy_usb2_power_off,
	.owner		= THIS_MODULE,
};

static const struct phy_ops rz_g1c_phy_usb2_ops = {
	.init		= rcar_gen3_phy_usb2_init,
	.exit		= rcar_gen3_phy_usb2_exit,
	.owner		= THIS_MODULE,
};

static const struct rcar_gen3_phy_drv_data rcar_gen3_phy_usb2_data = {
	.phy_usb2_ops = &rcar_gen3_phy_usb2_ops,
	.no_adp_ctrl = false,
};

static const struct rcar_gen3_phy_drv_data rz_g1c_phy_usb2_data = {
	.phy_usb2_ops = &rz_g1c_phy_usb2_ops,
	.no_adp_ctrl = false,
};

static const struct rcar_gen3_phy_drv_data rz_g2l_phy_usb2_data = {
	.phy_usb2_ops = &rcar_gen3_phy_usb2_ops,
	.no_adp_ctrl = true,
};

static const struct of_device_id rcar_gen3_phy_usb2_match_table[] = {
	{
		.compatible = "renesas,usb2-phy-r8a77470",
		.data = &rz_g1c_phy_usb2_data,
	},
	{
		.compatible = "renesas,usb2-phy-r8a7795",
		.data = &rcar_gen3_phy_usb2_data,
	},
	{
		.compatible = "renesas,usb2-phy-r8a7796",
		.data = &rcar_gen3_phy_usb2_data,
	},
	{
		.compatible = "renesas,usb2-phy-r8a77965",
		.data = &rcar_gen3_phy_usb2_data,
	},
	{
		.compatible = "renesas,rzg2l-usb2-phy",
		.data = &rz_g2l_phy_usb2_data,
	},
	{
		.compatible = "renesas,rcar-gen3-usb2-phy",
		.data = &rcar_gen3_phy_usb2_data,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rcar_gen3_phy_usb2_match_table);

static const unsigned int rcar_gen3_phy_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static struct phy *rcar_gen3_phy_usb2_xlate(struct device *dev,
					    const struct of_phandle_args *args)
{
	struct rcar_gen3_chan *ch = dev_get_drvdata(dev);
fin();

	if (args->args_count == 0)	/* For old version dts */
{
fout(0);
		return ch->rphys[PHY_INDEX_BOTH_HC].phy;
}
	else if (args->args_count > 1)	/* Prevent invalid args count */
{
fout(1);
		return ERR_PTR(-ENODEV);
}

	if (args->args[0] >= NUM_OF_PHYS)
{
fout(2);
		return ERR_PTR(-ENODEV);
}

fout(3);
	return ch->rphys[args->args[0]].phy;
}

static enum usb_dr_mode rcar_gen3_get_dr_mode(struct device_node *np)
{
	enum usb_dr_mode candidate = USB_DR_MODE_UNKNOWN;
	int i;
fin();

	/*
	 * If one of device nodes has other dr_mode except UNKNOWN,
	 * this function returns UNKNOWN. To achieve backward compatibility,
	 * this loop starts the index as 0.
	 */
	for (i = 0; i < NUM_OF_PHYS; i++) {
		enum usb_dr_mode mode = of_usb_get_dr_mode_by_phy(np, i);

		if (mode != USB_DR_MODE_UNKNOWN) {
			if (candidate == USB_DR_MODE_UNKNOWN)
				candidate = mode;
			else if (candidate != mode)
{
fout(0);
				return USB_DR_MODE_UNKNOWN;
}
		}
	}

fout(1);
	return candidate;
}

static int rcar_gen3_phy_usb2_probe(struct platform_device *pdev)
{
	const struct rcar_gen3_phy_drv_data *phy_data;
	struct device *dev = &pdev->dev;
	struct rcar_gen3_chan *channel;
	struct phy_provider *provider;
	int ret = 0, i;
fin();

	if (!dev->of_node) {
		dev_err(dev, "This driver needs device tree\n");
fout(0);
		return -EINVAL;
	}

	channel = devm_kzalloc(dev, sizeof(*channel), GFP_KERNEL);
	if (!channel)
{
fout(1);
		return -ENOMEM;
}

	channel->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(channel->base))
{
fout(2);
		return PTR_ERR(channel->base);
}

	channel->obint_enable_bits = USB2_OBINT_BITS;
	/* get irq number here and request_irq for OTG in phy_init */
	channel->irq = platform_get_irq_optional(pdev, 0);
	channel->dr_mode = rcar_gen3_get_dr_mode(dev->of_node);
	if (channel->dr_mode != USB_DR_MODE_UNKNOWN) {
		channel->is_otg_channel = true;
		channel->uses_otg_pins = !of_property_read_bool(dev->of_node,
							"renesas,no-otg-pins");
		channel->extcon = devm_extcon_dev_allocate(dev,
							rcar_gen3_phy_cable);
		if (IS_ERR(channel->extcon))
{
fout(3);
			return PTR_ERR(channel->extcon);
}

		ret = devm_extcon_dev_register(dev, channel->extcon);
		if (ret < 0) {
			dev_err(dev, "Failed to register extcon\n");
fout(4);
			return ret;
		}
	}

	/*
	 * devm_phy_create() will call pm_runtime_enable(&phy->dev);
	 * And then, phy-core will manage runtime pm for this device.
	 */
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	/* But ^^^...*/
	phy_data = of_device_get_match_data(dev);
	if (!phy_data) {
		ret = -EINVAL;
		goto error;
	}

	channel->soc_no_adp_ctrl = phy_data->no_adp_ctrl;
	if (phy_data->no_adp_ctrl)
		channel->obint_enable_bits = USB2_OBINT_IDCHG_EN;

	mutex_init(&channel->lock);
	for (i = 0; i < NUM_OF_PHYS; i++) {
		channel->rphys[i].phy = devm_phy_create(dev, NULL,
							phy_data->phy_usb2_ops);
		if (IS_ERR(channel->rphys[i].phy)) {
			dev_err(dev, "Failed to create USB2 PHY\n");
			ret = PTR_ERR(channel->rphys[i].phy);
			goto error;
		}
		channel->rphys[i].ch = channel;
		channel->rphys[i].int_enable_bits = rcar_gen3_int_enable[i];
		phy_set_drvdata(channel->rphys[i].phy, &channel->rphys[i]);
#if 0
	/* Consider using struct phy::pwr populated though phy-supply. */
	if (channel->soc_no_adp_ctrl && channel->is_otg_channel)
		channel->vbus = devm_regulator_get_exclusive(&channel->rphys[i].phy->dev, "vbus");
	else
		channel->vbus = devm_regulator_get_optional(&channel->rphys[i].phy->dev, "vbus");
	if (IS_ERR(channel->vbus)) {
		if (PTR_ERR(channel->vbus) == -EPROBE_DEFER) {
			ret = PTR_ERR(channel->vbus);
			goto error;
		}
		channel->vbus = NULL;
	}
#endif
	}

	/* Consider using struct phy::pwr populated though phy-supply. 
	 * stack traces on regulator when unbinding devices from this
	 * driver. First unbind attempts to unregister a used regulator.
	 */
	if (channel->soc_no_adp_ctrl && channel->is_otg_channel)
		channel->vbus = devm_regulator_get_exclusive(dev, "vbus");
	else
		channel->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(channel->vbus)) {
		if (PTR_ERR(channel->vbus) == -EPROBE_DEFER) {
			ret = PTR_ERR(channel->vbus);
			goto error;
		}
		channel->vbus = NULL;
	}

	platform_set_drvdata(pdev, channel);
	channel->dev = dev;

	provider = devm_of_phy_provider_register(dev, rcar_gen3_phy_usb2_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "Failed to register PHY provider\n");
		ret = PTR_ERR(provider);
		goto error;
	} else if (channel->is_otg_channel) {
		ret = device_create_file(dev, &dev_attr_role);
		if (ret < 0)
			goto error;
	}

fout(5);
	return 0;

error:
	pm_runtime_disable(dev);

fout(6);
	return ret;
}

static void rcar_gen3_phy_usb2_remove(struct platform_device *pdev)
{
	struct rcar_gen3_chan *channel = platform_get_drvdata(pdev);
fin();

	if (channel->is_otg_channel)
		device_remove_file(&pdev->dev, &dev_attr_role);

//	rcar_gen3_config_default(&pdev->dev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
};

static struct platform_driver rcar_gen3_phy_usb2_driver = {
	.driver = {
		.name		= "phy_rcar_gen3_usb2",
		.of_match_table	= rcar_gen3_phy_usb2_match_table,
	},
	.probe	= rcar_gen3_phy_usb2_probe,
	.remove_new = rcar_gen3_phy_usb2_remove,
};
module_platform_driver(rcar_gen3_phy_usb2_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Renesas R-Car Gen3 USB 2.0 PHY");
MODULE_AUTHOR("Yoshihiro Shimoda <yoshihiro.shimoda.uh@renesas.com>");
