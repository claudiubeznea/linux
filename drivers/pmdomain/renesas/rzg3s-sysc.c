// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G3S SYSC PM domain driver
 *
 * Copyright (C) 2024 Renesas Electronics Corporation
 */

#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

#include <linux/soc/renesas/rzg3s-sysc-pmdomain.h>
#include <dt-bindings/power/r9a08g045,sysc-pmdomain.h>

#define RZG3S_SYSC_USB_PWRRDY		0xd70
#define RZG3S_SYSC_PCIE_RST_RSM_B	0xd74
#define RZG3S_SYSC_SET_MASK		0x1

/**
 * struct rzg3s_sysc_pmdomain_conf - SYSC PM domain configuration
 * @offset: offset to configure the reset
 * @off_val: value to write to register on power on
 * @on_val: value to write to register on power off
 * @id: PM domain ID
 */
struct rzg3s_sysc_pmdomain_conf {
	u16 offset;
	u8 off_val;
	u8 on_val;
	u8 id;
};

/**
 * struct rzg3s_sysc_pmdomain_init_data - SYSC PM domain init data
 * @name: PM domain name
 * @conf: PM domain configuration
 */
struct rzg3s_sysc_pmdomain_init_data {
	const char *name;
	const struct rzg3s_sysc_pmdomain_conf conf;
};

/**
 * struct rzg3s_sysc_pmdomain_info - SYSC PM domain info
 * @init_data: PM domain init data
 * @num_domains: number of domains
 */
struct rzg3s_sysc_pmdomain_info {
	const struct rzg3s_sysc_pmdomain_init_data *init_data;
	u8 num_domains;
};

/**
 * struct rzg3s_sysc_pmdomains - SYSC PM domains
 * @onecell_data: cell data
 * @domains: generic PM domains
 */
struct rzg3s_sysc_pmdomains {
	struct genpd_onecell_data onecell_data;
	struct generic_pm_domain *domains[];
};

/**
 * struct rzg3s_sysc_pd - SYSC Power domain
 * @priv: SYSC private data
 * @genpd: generic PM domain
 * @conf: SYSC PM domain configuration
 */
struct rzg3s_sysc_pd {
	struct rzg3s_sysc_priv *priv;
	struct generic_pm_domain genpd;
	struct rzg3s_sysc_pmdomain_conf conf;
};

/**
 * struct rzg3s_sysc_priv - SYSC private data structure
 * @info: SYSC PM domain info
 * @adev: auxiliary device
 */
struct rzg3s_sysc_priv {
	const struct rzg3s_sysc_pmdomain_info *info;
	struct auxiliary_device *adev;
};

static int rzg3s_sysc_pmdomain_power_set(struct generic_pm_domain *domain, bool on)
{
	struct rzg3s_sysc_pd *pd = container_of(domain, struct rzg3s_sysc_pd, genpd);
	struct rzg3s_sysc_pmdomain_adev *pmd_adev = to_rzg3s_sysc_pmdomain_adev(pd->priv->adev);
	struct rzg3s_sysc_pmdomain_conf conf = pd->conf;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(pmd_adev->lock, flags);
	val = readl(pmd_adev->base + RZG3S_SYSC_USB_PWRRDY);
	val &= ~RZG3S_SYSC_SET_MASK;
	val |= on ? conf.on_val : conf.off_val;
	writel(val, pmd_adev->base + RZG3S_SYSC_USB_PWRRDY);
	spin_unlock_irqrestore(pmd_adev->lock, flags);

	return 0;
}

static int rzg3s_sysc_pmdomain_power_on(struct generic_pm_domain *domain)
{
	return rzg3s_sysc_pmdomain_power_set(domain, true);
}

static int rzg3s_sysc_pmdomain_power_off(struct generic_pm_domain *domain)
{
	return rzg3s_sysc_pmdomain_power_set(domain, false);
}

static struct generic_pm_domain *
rzg3s_sysc_pmdomain_xlate(const struct of_phandle_args *spec, void *data)
{
	struct generic_pm_domain *domain = ERR_PTR(-ENOENT);
	struct genpd_onecell_data *genpd = data;

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	for (unsigned int i = 0; i < genpd->num_domains; i++) {
		struct rzg3s_sysc_pd *pd = container_of(genpd->domains[i], struct rzg3s_sysc_pd,
							genpd);

		if (pd->conf.id == spec->args[0]) {
			domain = &pd->genpd;
			break;
		}
	}

	return domain;
}

static int rzg3s_sysc_pd_setup(struct rzg3s_sysc_pd *pd, bool always_on)
{
	struct dev_power_governor *governor;

	pd->genpd.flags = GENPD_FLAG_ACTIVE_WAKEUP;
	if (always_on) {
		pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		governor = &pm_domain_always_on_gov;
	} else {
		pd->genpd.power_on = rzg3s_sysc_pmdomain_power_on;
		pd->genpd.power_off = rzg3s_sysc_pmdomain_power_off;
		governor = &simple_qos_governor;
	}

	return pm_genpd_init(&pd->genpd, governor, !always_on);
}

static void rzg3s_sysc_genpd_remove(void *data)
{
	struct genpd_onecell_data *celldata = data;

	for (unsigned int i = 0; i < celldata->num_domains; i++)
		pm_genpd_remove(celldata->domains[i]);
}

static int rzg3s_sysc_pmdomain_add(struct rzg3s_sysc_priv *priv)
{
	const struct rzg3s_sysc_pmdomain_info *info = priv->info;
	struct rzg3s_sysc_pmdomains *domains;
	struct device *dev = &priv->adev->dev;
	struct device_node *np = dev->parent->of_node;
	int ret;

	domains = devm_kzalloc(dev, struct_size(domains, domains, info->num_domains),
			       GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	domains->onecell_data.domains = domains->domains;
	domains->onecell_data.num_domains = info->num_domains;
	domains->onecell_data.xlate = rzg3s_sysc_pmdomain_xlate;

	ret = devm_add_action_or_reset(dev, rzg3s_sysc_genpd_remove, &domains->onecell_data);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < info->num_domains; i++) {
		struct generic_pm_domain *parent;
		struct rzg3s_sysc_pd *pd;

		pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
		if (!pd)
			return -ENOMEM;

		pd->genpd.name = info->init_data[i].name;
		pd->conf = info->init_data[i].conf;
		pd->priv = priv;

		/*
		 * Parent should be on the very first entry of info->init_data[].
		 * We keep it always on.
		 */
		ret = rzg3s_sysc_pd_setup(pd, !i);
		if (ret)
			return ret;

		domains->domains[i] = &pd->genpd;
		if (!i) {
			parent = &pd->genpd;
			continue;
		}

		ret = pm_genpd_add_subdomain(parent, &pd->genpd);
		if (ret)
			return ret;
	}

	return of_genpd_add_provider_onecell(np, &domains->onecell_data);
}

static int rzg3s_sysc_pmdomain_probe(struct auxiliary_device *adev,
				     const struct auxiliary_device_id *id)
{
	const struct rzg3s_sysc_pmdomain_info *info =
					(struct rzg3s_sysc_pmdomain_info *)id->driver_data;
	struct rzg3s_sysc_pmdomain_adev *pmd_adev = to_rzg3s_sysc_pmdomain_adev(adev);
	struct device *dev = &adev->dev;
	struct rzg3s_sysc_priv *priv;

	if (!info || !pmd_adev || !pmd_adev->base || !pmd_adev->lock)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->adev = adev;
	priv->info = info;

	return rzg3s_sysc_pmdomain_add(priv);
}

#define RZG3S_SYSC_DEF_PD(_name, _id, _offset, _on, _off) \
	{ \
		.name = _name, \
		.conf = { \
			.id = _id, \
			.offset = _offset, \
			.on_val = _on, \
			.off_val = _off, \
		} \
	}

static const struct rzg3s_sysc_pmdomain_init_data rzg3s_sysc_pmdomain_init_data[] = {
	/*
	 * sysc is the parent domain. Id, offset, on, off don't count as sysc domain
	 * is fake to have a common parent for the rest of domains.
	 */
	RZG3S_SYSC_DEF_PD("sysc", 0, 0, 0, 0),
	RZG3S_SYSC_DEF_PD("usb", R9A08G045_SYSC_PD_USB, RZG3S_SYSC_USB_PWRRDY, 0, 1),
	RZG3S_SYSC_DEF_PD("pci", R9A08G045_SYSC_PD_PCI, RZG3S_SYSC_PCIE_RST_RSM_B, 1, 0)
};

static const struct rzg3s_sysc_pmdomain_info rzg3s_sysc_pmdomain_info = {
	.init_data = rzg3s_sysc_pmdomain_init_data,
	.num_domains = ARRAY_SIZE(rzg3s_sysc_pmdomain_init_data)
};

static const struct auxiliary_device_id rzg3s_sysc_pmdomain_ids[] = {
	{
		.name = "rzg3s_sysc.pmdomain",
		.driver_data = (kernel_ulong_t)&rzg3s_sysc_pmdomain_info
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(auxiliary, rzg2l_pm_domain_ids);

static struct auxiliary_driver rzg2l_pm_domain_driver = {
	.probe		= rzg3s_sysc_pmdomain_probe,
	.id_table	= rzg3s_sysc_pmdomain_ids,
};
module_auxiliary_driver(rzg2l_pm_domain_driver);
