/* AMLogic Meson USB2.0 PHY
 *
 * Copyright 2016 Ben Dooks <ben.dooks@codethink.co.uk>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * Note, the PHY block can contain more than one PHY, but at least on the
 * S805 the PHY looks like it has one clock and reset control, therefore we
 * put them both into the same device-node to avoid multiple phy-nodes trying
 * to reset a single peripheral node.
 */

#include <linux/module.h>

#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/reset.h>

#include <linux/usb/phy_companion.h>

#define PHYREG_CONFIG		(0x00)
#define PHYREG_CTRL		(0x04)
#define PHYREG_ADP_BC		(0x0C)

#define CONFIG_CLK_EN		BIT(0)
#define CONFIG_POWEROFF		(0x3 << 12)
#define CONFIG_CLK_SET(__clk)	((__clk) << 1)
#define CONFIG_CLK_DIV(__div)	((__div) << 4)
#define CONFIG_CLK_32KALT	BIT(15)

#define CTRL_CLK_DETECTED	BIT(8)
#define CTRL_POR		BIT(15)
#define CTRL_FSEL(__fs)		((__fs) << 22)
#define CTRL_FSEL_MASK		(7 << 22)

struct amlogic_usbphys;
struct amlogic_usbphy {
	struct amlogic_usbphys	*parent;
	struct device_node	*node;
	struct phy		*phy;
	struct gpio_desc	*reset;
	void __iomem		*regs;
	unsigned int		id;
};	

struct amlogic_usbphys {
	void __iomem		*regs;
	int			nr_phys;
	struct clk		*clk;		/* main/general clock */
	struct device		*dev;
	struct reset_control	*reset;		/* main reset control */
	struct amlogic_usbphy	phys[];
};

static int amlogic_usb_phy_power(struct amlogic_usbphy *phy, bool to)
{
	return 0;
}

static int amlogic_usb_power_off(struct phy *_phy)
{
	struct amlogic_usbphy *phy = phy_get_drvdata(_phy);
	dev_info(phy->parent->dev, "phy%d: power-off\n", phy->id);
	return amlogic_usb_phy_power(phy, false);
}

static int amlogic_usb_power_on(struct phy *_phy)
{
	struct amlogic_usbphy *phy = phy_get_drvdata(_phy);
	dev_info(phy->parent->dev, "phy%d: power-on\n", phy->id);	
	return amlogic_usb_phy_power(phy, true);
}

// notes
// USB0 clock is CBUS1 0x1051  bit 21
// USB1 clock is CBUS1 0x1051  bit 22
// USB0 bridge is CBUS1 0x1052 bit 9
// USB1 bridge is CBUS1 0x1052 bit 8
// USB general is CBUS1 0x1051 bit 26

static void hack_enable_clocks(void)
{
	static void __iomem *reg;

	if (!reg)
		reg = ioremap(0xc1104144, 8);
	if (WARN_ON(!reg))
		return;

	pr_info("%s: regs %08x, %08x (before)\n",
		__func__, readl(reg + 0x0), readl(reg + 0x4));

	writel(readl(reg + 0x0) | (3 << 21) | (1 << 26), reg + 0x0);
	writel(readl(reg + 0x4) | (3 << 8),  reg + 0x4);

	pr_info("%s: regs %08x, %08x (after)\n",
		__func__, readl(reg + 0x0), readl(reg + 0x4));	
}

static int amlogic_usb_init(struct phy *_phy)
{
	struct amlogic_usbphy *phy = phy_get_drvdata(_phy);
	struct amlogic_usbphys *phys = phy->parent;
	u32 reg;

	dev_info(phys->dev, "phy%d: initialising phy\n", phy->id);
	hack_enable_clocks();

	reg = readl(phy->regs + PHYREG_CONFIG);
	reg |= CONFIG_CLK_32KALT;
	reg &= ~CONFIG_POWEROFF;
	writel(reg, phy->regs + PHYREG_CONFIG);
	dev_info(phys->dev, "phy%d: config=%08x\n", phy->id, reg);

	reg = readl(phy->regs + PHYREG_CTRL);
	reg &= ~CTRL_FSEL_MASK;
	reg |= CTRL_FSEL(5);
	reg |= CTRL_POR;

	writel(reg, phy->regs + PHYREG_CTRL);
	msleep(1);
	reg &= ~CTRL_POR;
	writel(reg, phy->regs + PHYREG_CTRL);
	msleep(1);
	dev_info(phys->dev, "phy%d: ctrl=%08x\n", phy->id, reg);

	if (phy->id == 1) {
		reg = readl(phy->regs + PHYREG_ADP_BC);
		reg |= BIT(16);  // ADA_EANBLE;
		writel(reg, phy->regs + PHYREG_ADP_BC);
	}

	if (!(readl(phy->regs + PHYREG_CTRL) & CTRL_CLK_DETECTED))
		dev_warn(phys->dev, "phy%d: no clock detected\n", phy->id);

	return 0;
}

static const struct phy_ops phy_ops = {
	.init		= amlogic_usb_init,
	.power_on	= amlogic_usb_power_on,
	.power_off	= amlogic_usb_power_off,
	.owner		= THIS_MODULE,
};

static struct phy *amlogic_phy_xlate(struct device *dev,
				     struct of_phandle_args *args)
{
	struct amlogic_usbphys *phys = dev_get_drvdata(dev);  
	struct device_node *np = args->np;
	int i;

	for (i = 0; i < phys->nr_phys; i++) {
		dev_info(dev, "match %d: %p, %p\n", i, phys->phys[i].node, np);
		if (phys->phys[i].node == np)
			return phys->phys[i].phy;
	}
	
	return ERR_PTR(-ENODEV);
}

static int amlogic_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;	
	struct amlogic_usbphys *phys;
	struct amlogic_usbphy *phy;
	struct device_node *np;
	struct resource *res;
	int nr_phy;
	size_t sz;
	
	nr_phy = of_get_child_count(pdev->dev.of_node);
	if (nr_phy <= 0) {
		dev_err(dev, "no phys specified (%d)\n", nr_phy);
		return -EINVAL;
	}
	
	dev_info(dev, "%d phys\n", nr_phy);
	
	sz = sizeof(*phys) + sizeof(struct amlogic_usbphy) * nr_phy;
	phys = devm_kzalloc(&pdev->dev, sz, GFP_KERNEL);
	if (!phys)
		return -ENOMEM;

	phys->dev = dev;
	phys->nr_phys = nr_phy;
	platform_set_drvdata(pdev, phys);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	phys->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(phys->regs)) {
		dev_err(dev, "no registers specified\n");
		return PTR_ERR(phys->regs);
	}

	phys->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(phys->reset)) {
		dev_warn(dev, "no reset controller\n");
		return PTR_ERR(phys->reset);
	}

	phys->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(phys->clk))
		dev_warn(dev, "no main clock supplied\n");
  
	reset_control_reset(phys->reset);

	for_each_child_of_node(dev->of_node, np) {
		int err;
		u32 ch;

		err = of_property_read_u32(np, "reg", &ch);
		if (err) {
			dev_err(dev, "cannot read reg property\n");
			continue;
		}

		dev_info(dev, "attaching phy %d\n", ch);
		
		phy = &phys->phys[ch];
		phy->phy = devm_phy_create(dev, NULL, &phy_ops);
		if (IS_ERR(phy->phy)) {
			dev_err(dev, "Failed to create PHY\n");
			continue;
		}

		// todo - do we need to get the usb general clock
		// here?
		// of_clk_get_by_name(np, "main")
		// of_clk_get_by_name(np, "ddr")

		if (ch == 1) {
			phy->reset = devm_gpiod_get_optional(dev, "reset1",
							     GPIOD_OUT_LOW);
		}

		phy->id = ch;
		phy->parent = phys;
		phy->regs = phys->regs + (ch * 0x20);
		phy->node = np;
		phy_set_drvdata(phy->phy, phy);
	}

	provider = devm_of_phy_provider_register(dev, amlogic_phy_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "Failed to register PHY provider\n");
		return PTR_ERR(provider);
	}

	dev_info(dev, "added phys\n");
	return 0;
}

static const struct of_device_id amlogic_usb_id_table[] = {
	{ .compatible = "amlogic,meson-usb-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, amlogic_usb_id_table);

static struct platform_driver amlogic_usb_driver = {
	.probe		= amlogic_usb_probe,
	.driver		= {
		.name	= "amlogic-usbphy",
		.of_match_table = amlogic_usb_id_table,
	},
};

module_platform_driver(amlogic_usb_driver);

MODULE_DESCRIPTION("Amlogic Meson USB-PHY driver");
MODULE_AUTHOR("Ben Dooks <ben.dooks@codethink.co.uk>");
MODULE_LICENSE("GPL");
