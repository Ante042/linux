// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple SoC pinctrl+GPIO+external IRQ driver
 *
 * Copyright (C) The Asahi Linux Contributors
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on: pinctrl-pistachio.c
 * Copyright (C) 2014 Imagination Technologies Ltd.
 * Copyright (C) 2014 Google, Inc.
 */

#include <dt-bindings/pinctrl/apple.h>

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/suspend.h>

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-utils.h"
#include "core.h"
#include "pinmux.h"
#include "pinctrl-apple-irq.h"

struct apple_gpio_pinctrl;

struct apple_gpio_irq_group {
	struct apple_gpio_pinctrl *pctl;
	u8 index;
};

struct apple_gpio_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctldev;

	void __iomem *base;
	struct regmap *map;

	struct pinctrl_desc pinctrl_desc;
	struct gpio_chip gpio_chip;
	/* The parent IRQ of the first group, which every pin's IRQ goes to */
	int grp0_irq;

	const struct apple_gpio_hw *hw;
	/* Pin configuration saved for suspend-to-RAM, NULL if the block keeps it */
	u32 *s2r_cfg;
	bool s2r_saved;
	u32 s2r_input_enable;

	u8 irq_group_base;
	struct apple_gpio_irq_group irqgrps[];
};

#define REG_GPIO(x)          (4 * (x))
#define REG_GPIOx_DATA       BIT(0)
#define REG_GPIOx_MODE       GENMASK(3, 1)
#define REG_GPIOx_OUT        1
#define REG_GPIOx_IN_IRQ_HI  2
#define REG_GPIOx_IN_IRQ_LO  3
#define REG_GPIOx_IN_IRQ_UP  4
#define REG_GPIOx_IN_IRQ_DN  5
#define REG_GPIOx_IN_IRQ_ANY 6
#define REG_GPIOx_IN_IRQ_OFF 7
#define REG_GPIOx_PERIPH     GENMASK(6, 5)
#define REG_GPIOx_PULL       GENMASK(8, 7)
#define REG_GPIOx_PULL_OFF   0
#define REG_GPIOx_PULL_DOWN  1
#define REG_GPIOx_PULL_UP_STRONG 2
#define REG_GPIOx_PULL_UP    3
#define REG_GPIOx_INPUT_ENABLE BIT(9)
#define REG_GPIOx_DRIVE_STRENGTH0 GENMASK(11, 10)
#define REG_GPIOx_SCHMITT    BIT(15)
#define REG_GPIOx_GRP        GENMASK(18, 16)
#define REG_GPIOx_LOCK       BIT(21)
#define REG_GPIOx_DRIVE_STRENGTH1 GENMASK(23, 22)
#define REG_IRQ(g, x)        (0x800 + 0x40 * (g) + 4 * ((x) >> 5))
/*
 * Input receivers of the whole block. The boot firmware sets this, and it reads
 * 0 once the block has lost power. While it is clear every input reads low, so
 * a level-low IRQ fires forever although nothing drives the line.
 */
#define REG_INPUT_ENABLE     0xc48

static const struct regmap_config regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.cache_type = REGCACHE_FLAT,
	.max_register = 512 * sizeof(u32),
	.num_reg_defaults_raw = 512,
	.use_relaxed_mmio = true,
	.use_raw_spinlock = true,
};

/* No locking needed to mask/unmask IRQs as the interrupt mode is per pin-register. */
static void apple_gpio_set_reg(struct apple_gpio_pinctrl *pctl,
			       unsigned int pin, u32 mask, u32 value)
{
	regmap_update_bits(pctl->map, REG_GPIO(pin), mask, value);
}

static u32 apple_gpio_get_reg(struct apple_gpio_pinctrl *pctl,
			      unsigned int pin)
{
	int ret;
	u32 val;

	ret = regmap_read(pctl->map, REG_GPIO(pin), &val);
	if (ret)
		return 0;

	return val;
}

/* Pin controller functions */

static int apple_gpio_dt_node_to_map(struct pinctrl_dev *pctldev,
				     struct device_node *node,
				     struct pinctrl_map **map,
				     unsigned int *num_maps)
{
	unsigned int reserved_maps;
	struct apple_gpio_pinctrl *pctl;
	u32 pinfunc, pin, func;
	int num_pins, i, ret;
	const char *group_name;
	const char *function_name;

	*map = NULL;
	*num_maps = 0;
	reserved_maps = 0;

	pctl = pinctrl_dev_get_drvdata(pctldev);

	ret = of_property_count_u32_elems(node, "pinmux");
	if (ret <= 0) {
		dev_err(pctl->dev,
			"missing or empty pinmux property in node %pOFn.\n",
			node);
		return ret ? ret : -EINVAL;
	}

	num_pins = ret;

	ret = pinctrl_utils_reserve_map(pctldev, map, &reserved_maps, num_maps, num_pins);
	if (ret)
		return ret;

	for (i = 0; i < num_pins; i++) {
		ret = of_property_read_u32_index(node, "pinmux", i, &pinfunc);
		if (ret)
			goto free_map;

		pin = APPLE_PIN(pinfunc);
		func = APPLE_FUNC(pinfunc);

		if (func >= pinmux_generic_get_function_count(pctldev)) {
			ret = -EINVAL;
			goto free_map;
		}

		group_name = pinctrl_generic_get_group_name(pctldev, pin);
		function_name = pinmux_generic_get_function_name(pctl->pctldev, func);
		ret = pinctrl_utils_add_map_mux(pctl->pctldev, map,
						&reserved_maps, num_maps,
						group_name, function_name);
		if (ret)
			goto free_map;
	}

free_map:
	if (ret < 0)
		pinctrl_utils_free_map(pctldev, *map, *num_maps);

	return ret;
}

static const struct pinctrl_ops apple_gpio_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = apple_gpio_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

/* Pin multiplexer functions */

static bool apple_gpio_pinmux_func_is_gpio(struct pinctrl_dev *pctldev,
					   unsigned int selector)
{
	/* Function selector 0 is always the GPIO mode */
	return (selector == 0);
}

static int apple_gpio_pinmux_set(struct pinctrl_dev *pctldev, unsigned int func,
				 unsigned int group)
{
	struct apple_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	apple_gpio_set_reg(
		pctl, group, REG_GPIOx_PERIPH | REG_GPIOx_INPUT_ENABLE,
		FIELD_PREP(REG_GPIOx_PERIPH, func) | REG_GPIOx_INPUT_ENABLE);

	return 0;
}

static const struct pinmux_ops apple_gpio_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.function_is_gpio = apple_gpio_pinmux_func_is_gpio,
	.set_mux = apple_gpio_pinmux_set,
	.strict = true,
};

/* GPIO chip functions */

static int apple_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);
	unsigned int reg = apple_gpio_get_reg(pctl, offset);

	if (FIELD_GET(REG_GPIOx_MODE, reg) == REG_GPIOx_OUT)
		return GPIO_LINE_DIRECTION_OUT;
	return GPIO_LINE_DIRECTION_IN;
}

static int apple_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);
	unsigned int reg = apple_gpio_get_reg(pctl, offset);

	/*
	 * If this is an input GPIO, read the actual value (not the
	 * cached regmap value)
	 */
	if (FIELD_GET(REG_GPIOx_MODE, reg) != REG_GPIOx_OUT)
		reg = readl_relaxed(pctl->base + REG_GPIO(offset));

	return !!(reg & REG_GPIOx_DATA);
}

static int apple_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	apple_gpio_set_reg(pctl, offset, REG_GPIOx_DATA, value ? REG_GPIOx_DATA : 0);

	return 0;
}

static int apple_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	apple_gpio_set_reg(pctl, offset,
			   REG_GPIOx_PERIPH | REG_GPIOx_MODE | REG_GPIOx_DATA |
				   REG_GPIOx_INPUT_ENABLE,
			   FIELD_PREP(REG_GPIOx_MODE, REG_GPIOx_IN_IRQ_OFF) |
				   REG_GPIOx_INPUT_ENABLE);
	return 0;
}

static int apple_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	apple_gpio_set_reg(pctl, offset,
			   REG_GPIOx_PERIPH | REG_GPIOx_MODE | REG_GPIOx_DATA,
			   FIELD_PREP(REG_GPIOx_MODE, REG_GPIOx_OUT) |
				   (value ? REG_GPIOx_DATA : 0));
	return 0;
}

/* IRQ chip functions */

static void apple_gpio_irq_ack(struct irq_data *data)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));
	unsigned int irqgrp = FIELD_GET(REG_GPIOx_GRP, apple_gpio_get_reg(pctl, data->hwirq));

	writel(BIT(data->hwirq % 32), pctl->base + REG_IRQ(irqgrp, data->hwirq));
}

static unsigned int apple_gpio_irq_type(unsigned int type)
{
	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		return REG_GPIOx_IN_IRQ_UP;
	case IRQ_TYPE_EDGE_FALLING:
		return REG_GPIOx_IN_IRQ_DN;
	case IRQ_TYPE_EDGE_BOTH:
		return REG_GPIOx_IN_IRQ_ANY;
	case IRQ_TYPE_LEVEL_HIGH:
		return REG_GPIOx_IN_IRQ_HI;
	case IRQ_TYPE_LEVEL_LOW:
		return REG_GPIOx_IN_IRQ_LO;
	default:
		return REG_GPIOx_IN_IRQ_OFF;
	}
}

static void apple_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(gc);

	apple_gpio_set_reg(pctl, data->hwirq, REG_GPIOx_MODE,
			   FIELD_PREP(REG_GPIOx_MODE, REG_GPIOx_IN_IRQ_OFF));
	gpiochip_disable_irq(gc, data->hwirq);
}

static void apple_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(gc);
	unsigned int irqtype = apple_gpio_irq_type(irqd_get_trigger_type(data));

	gpiochip_enable_irq(gc, data->hwirq);
	apple_gpio_set_reg(pctl, data->hwirq, REG_GPIOx_MODE,
			   FIELD_PREP(REG_GPIOx_MODE, irqtype));
}

static unsigned int apple_gpio_irq_startup(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	apple_gpio_set_reg(pctl, data->hwirq, REG_GPIOx_GRP,
			   FIELD_PREP(REG_GPIOx_GRP, pctl->irq_group_base));

	apple_gpio_direction_input(chip, data->hwirq);
	apple_gpio_irq_unmask(data);

	return 0;
}

static int apple_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));
	unsigned int irqtype = apple_gpio_irq_type(type);

	if (irqtype == REG_GPIOx_IN_IRQ_OFF)
		return -EINVAL;

	apple_gpio_set_reg(pctl, data->hwirq, REG_GPIOx_MODE,
			   FIELD_PREP(REG_GPIOx_MODE, irqtype));

	if (type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(data, handle_level_irq);
	else
		irq_set_handler_locked(data, handle_edge_irq);
	return 0;
}

/*
 * Pass wake requests on to the AIC, which only keeps wake-armed IRQs
 * unmasked during system sleep.
 */
static int apple_gpio_irq_set_wake(struct irq_data *data, unsigned int on)
{
	struct apple_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));

	return irq_set_irq_wake(pctl->grp0_irq, on);
}

static void apple_gpio_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct apple_gpio_irq_group *group = irq_desc_get_handler_data(desc);
	struct apple_gpio_pinctrl *pctl = group->pctl;
	unsigned int pinh, pinl;
	unsigned long pending;
	struct gpio_chip *gc;

	gc = &pctl->gpio_chip;

	chained_irq_enter(chip, desc);
	for (pinh = 0; pinh < gc->ngpio; pinh += 32) {
		pending = readl_relaxed(pctl->base + REG_IRQ(group->index, pinh));
		for_each_set_bit(pinl, &pending, min_t(unsigned int, 32, gc->ngpio - pinh))
			generic_handle_domain_irq(gc->irq.domain, pinh + pinl);
	}
	chained_irq_exit(chip, desc);
}

static const struct irq_chip apple_gpio_irqchip = {
	.name			= "Apple-GPIO",
	.irq_startup		= apple_gpio_irq_startup,
	.irq_ack		= apple_gpio_irq_ack,
	.irq_mask		= apple_gpio_irq_mask,
	.irq_unmask		= apple_gpio_irq_unmask,
	.irq_set_type		= apple_gpio_irq_set_type,
	/*
	 * The group interrupts are chained handlers on the AIC, which
	 * the PM core never disables across system sleep, so a pin
	 * interrupt only has to be marked as a wakeup source here.
	 */
	.flags			= IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

/* Wake arming that reaches the AIC, so far only on the machine with suspend-to-RAM */
static const struct irq_chip apple_gpio_irqchip_wake = {
	.name			= "Apple-GPIO",
	.irq_startup		= apple_gpio_irq_startup,
	.irq_ack		= apple_gpio_irq_ack,
	.irq_mask		= apple_gpio_irq_mask,
	.irq_unmask		= apple_gpio_irq_unmask,
	.irq_set_type		= apple_gpio_irq_set_type,
	.irq_set_wake		= apple_gpio_irq_set_wake,
	/*
	 * Every pin shares the first group's IRQ, so pins that are not wake sources
	 * have to be masked here while it stays open. Drivers such as
	 * spi-hid-apple disable their IRQ before arming it for wake.
	 */
	.flags			= IRQCHIP_IMMUTABLE | IRQCHIP_MASK_ON_SUSPEND |
				  IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

/* Probe & register */

struct apple_gpio_hw {
	/*
	 * The register block contains slots that fault when read, so the regmap
	 * cannot be cache-initialised by reading it back; access it uncached and
	 * only within the valid GPIO registers minus gpio-reserved-ranges.
	 */
	bool no_regcache;
	/*
	 * The instance in a power domain that can go down loses its pin
	 * configuration in suspend-to-RAM. Save it and check it on resume,
	 * except for the pins the ADT lists as no-restore-gpio-pins, which
	 * macOS leaves alone too. Only SoCs whose list is known do this.
	 */
	bool s2r_save;
	const u16 *s2r_skip_pins;
	unsigned int num_s2r_skip_pins;
};

/* T8140 (A18 Pro) and T8132 (M4) both have faulting slots in this block. */
static const struct apple_gpio_hw apple_gpio_uncached_hw = {
	.no_regcache = true,
};

/* no-restore-gpio-pins of gpio0 in the J314s ADT */
static const u16 apple_gpio_t6000_s2r_skip_pins[] = { 114, 115, 119 };

static const struct apple_gpio_hw apple_gpio_t6000_hw = {
	.s2r_save = true,
	.s2r_skip_pins = apple_gpio_t6000_s2r_skip_pins,
	.num_s2r_skip_pins = ARRAY_SIZE(apple_gpio_t6000_s2r_skip_pins),
};

static int apple_gpio_regmap_access(struct device *dev, unsigned int npins,
				    struct regmap_config *config)
{
	struct regmap_access_table *access;
	struct regmap_range *ranges;
	unsigned int start, count;
	int nvalues = 0, i, ret;

	if (!npins || npins > 512)
		return -EINVAL;

	if (of_property_present(dev->of_node, "gpio-reserved-ranges")) {
		nvalues = of_property_count_u32_elems(dev->of_node,
						    "gpio-reserved-ranges");
		if (nvalues < 0 || nvalues % 2)
			return -EINVAL;
	}

	access = devm_kzalloc(dev, sizeof(*access), GFP_KERNEL);
	ranges = devm_kcalloc(dev, nvalues / 2 + 1, sizeof(*ranges), GFP_KERNEL);
	if (!access || !ranges)
		return -ENOMEM;

	ranges[0].range_min = 0;
	ranges[0].range_max = REG_GPIO(npins - 1);
	for (i = 0; i < nvalues / 2; i++) {
		ret = of_property_read_u32_index(dev->of_node, "gpio-reserved-ranges",
						2 * i, &start);
		if (ret)
			return ret;
		ret = of_property_read_u32_index(dev->of_node, "gpio-reserved-ranges",
						2 * i + 1, &count);
		if (ret)
			return ret;
		if (!count || start >= npins || count > npins - start)
			return -EINVAL;
		ranges[i + 1].range_min = REG_GPIO(start);
		ranges[i + 1].range_max = REG_GPIO(start + count - 1);
	}
	access->yes_ranges = ranges;
	access->n_yes_ranges = 1;
	access->no_ranges = ranges + 1;
	access->n_no_ranges = nvalues / 2;
	config->rd_table = access;
	config->wr_table = access;
	config->max_register = REG_GPIO(npins - 1);
	return 0;
}

/*
 * Only a block whose power domain can go down loses its pins in
 * suspend-to-RAM: on t6000 that is the AP block. The NUB block sits in an
 * always-on domain, and the SMC and AOP blocks have no domain at all; the
 * ADT marks all three no-resume-restore.
 */
static bool apple_gpio_domain_can_power_down(struct device *dev)
{
	struct device_node *pd;
	bool ret;

	pd = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!pd)
		return false;

	ret = !of_property_read_bool(pd, "apple,always-on");
	of_node_put(pd);

	return ret;
}

static int apple_gpio_register(struct apple_gpio_pinctrl *pctl)
{
	struct gpio_irq_chip *girq = &pctl->gpio_chip.irq;
	void **irq_data = NULL;
	int ret;

	pctl->gpio_chip.label = dev_name(pctl->dev);
	pctl->gpio_chip.request = gpiochip_generic_request;
	pctl->gpio_chip.free = gpiochip_generic_free;
	pctl->gpio_chip.get_direction = apple_gpio_get_direction;
	pctl->gpio_chip.direction_input = apple_gpio_direction_input;
	pctl->gpio_chip.direction_output = apple_gpio_direction_output;
	pctl->gpio_chip.get = apple_gpio_get;
	pctl->gpio_chip.set = apple_gpio_set;
	pctl->gpio_chip.base = -1;
	pctl->gpio_chip.ngpio = pctl->pinctrl_desc.npins;
	pctl->gpio_chip.parent = pctl->dev;

	if (girq->num_parents) {
		int i;

		if (IS_ENABLED(CONFIG_APPLE_S2R) && of_machine_is_compatible("apple,t6000"))
			gpio_irq_chip_set_chip(girq, &apple_gpio_irqchip_wake);
		else
			gpio_irq_chip_set_chip(girq, &apple_gpio_irqchip);
		girq->parent_handler = apple_gpio_irq_handler;

		girq->parents = kmalloc_array(girq->num_parents,
					      sizeof(*girq->parents),
					      GFP_KERNEL);
		irq_data = kmalloc_objs(*irq_data, girq->num_parents);
		if (!girq->parents || !irq_data) {
			ret = -ENOMEM;
			goto out_free_irq_data;
		}

		for (i = 0; i < girq->num_parents; i++) {
			ret = platform_get_irq(to_platform_device(pctl->dev), i);
			if (ret < 0)
				goto out_free_irq_data;

			girq->parents[i] = ret;
			if (!i)
				pctl->grp0_irq = ret;
			pctl->irqgrps[i].pctl = pctl;
			pctl->irqgrps[i].index = pctl->irq_group_base + i;
			irq_data[i] = &pctl->irqgrps[i];
		}

		girq->parent_handler_data_array = irq_data;
		girq->per_parent_data = true;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_level_irq;
	}

	ret = devm_gpiochip_add_data(pctl->dev, &pctl->gpio_chip, pctl);

out_free_irq_data:
	kfree(girq->parents);
	kfree(irq_data);

	return ret;
}

static int apple_gpio_pinctrl_probe(struct platform_device *pdev)
{
	struct apple_gpio_pinctrl *pctl;
	struct regmap_config pctl_regmap_config = regmap_config;
	const struct apple_gpio_hw *hw = device_get_match_data(&pdev->dev);
	struct pinctrl_pin_desc *pins;
	unsigned int npins;
	const char **pin_names;
	unsigned int *pin_nums;
	static const char *pinmux_functions[] = {
		"gpio", "periph1", "periph2", "periph3"
	};
	unsigned int i, nirqs = 0, irq_group_base = 0;
	struct resource *resource;
	int res;

	if (of_property_read_bool(pdev->dev.of_node, "interrupt-controller")) {
		res = platform_irq_count(pdev);
		if (res > 0)
			nirqs = res;
	}

	if (of_property_present(pdev->dev.of_node, "apple,irq-group-base")) {
		res = of_property_read_u32(pdev->dev.of_node, "apple,irq-group-base",
					  &irq_group_base);
		if (res)
			return res;
	}
	if (irq_group_base > 6 || nirqs > 7 - irq_group_base)
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid host IRQ groups\n");

	pctl = devm_kzalloc(&pdev->dev, struct_size(pctl, irqgrps, nirqs),
			    GFP_KERNEL);
	if (!pctl)
		return -ENOMEM;
	pctl->dev = &pdev->dev;
	pctl->irq_group_base = irq_group_base;
	pctl->gpio_chip.irq.num_parents = nirqs;
	dev_set_drvdata(&pdev->dev, pctl);

	if (of_property_read_u32(pdev->dev.of_node, "apple,npins", &npins))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "apple,npins property not found\n");

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || apple_gpio_irq_layout(npins, irq_group_base, nirqs,
					    resource_size(resource)))
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid GPIO register aperture\n");

	pins = devm_kmalloc_array(&pdev->dev, npins, sizeof(pins[0]),
				  GFP_KERNEL);
	pin_names = devm_kmalloc_array(&pdev->dev, npins, sizeof(pin_names[0]),
				       GFP_KERNEL);
	pin_nums = devm_kmalloc_array(&pdev->dev, npins, sizeof(pin_nums[0]),
				      GFP_KERNEL);
	if (!pins || !pin_names || !pin_nums)
		return -ENOMEM;

	pctl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pctl->base))
		return PTR_ERR(pctl->base);

	if (hw && hw->no_regcache) {
		pctl_regmap_config.cache_type = REGCACHE_NONE;
		pctl_regmap_config.num_reg_defaults_raw = 0;
		res = apple_gpio_regmap_access(&pdev->dev, npins, &pctl_regmap_config);
		if (res)
			return dev_err_probe(&pdev->dev, res, "invalid GPIO register ranges\n");
	} else {
		/*
		 * The flat cache is filled by reading the block back: read only
		 * the registers of the pins this bank has, not all 512 slots.
		 */
		if (!npins || npins > 512)
			return dev_err_probe(&pdev->dev, -EINVAL, "invalid apple,npins\n");
		pctl_regmap_config.max_register = REG_GPIO(npins - 1);
		pctl_regmap_config.num_reg_defaults_raw = npins;
	}

	pctl->map = devm_regmap_init_mmio(&pdev->dev, pctl->base,
					 &pctl_regmap_config);
	if (IS_ERR(pctl->map))
		return dev_err_probe(&pdev->dev, PTR_ERR(pctl->map),
				     "Failed to create regmap\n");

	pctl->hw = hw;
	if (hw && hw->s2r_save && apple_gpio_domain_can_power_down(&pdev->dev)) {
		pctl->s2r_cfg = devm_kcalloc(&pdev->dev, npins, sizeof(*pctl->s2r_cfg),
					     GFP_KERNEL);
		if (!pctl->s2r_cfg)
			return -ENOMEM;
	}

	for (i = 0; i < npins; i++) {
		pins[i].number = i;
		pins[i].name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "PIN%u", i);
		if (!pins[i].name)
			return -ENOMEM;

		pins[i].drv_data = pctl;
		pin_names[i] = pins[i].name;
		pin_nums[i] = i;
	}

	pctl->pinctrl_desc.name = dev_name(pctl->dev);
	pctl->pinctrl_desc.pins = pins;
	pctl->pinctrl_desc.npins = npins;
	pctl->pinctrl_desc.pctlops = &apple_gpio_pinctrl_ops;
	pctl->pinctrl_desc.pmxops = &apple_gpio_pinmux_ops;

	pctl->pctldev =	devm_pinctrl_register(&pdev->dev, &pctl->pinctrl_desc, pctl);
	if (IS_ERR(pctl->pctldev))
		return dev_err_probe(&pdev->dev, PTR_ERR(pctl->pctldev),
				     "Failed to register pinctrl device.\n");

	for (i = 0; i < npins; i++) {
		res = pinctrl_generic_add_group(pctl->pctldev, pins[i].name,
						pin_nums + i, 1, pctl);
		if (res < 0)
			return dev_err_probe(pctl->dev, res,
					     "Failed to register group");
	}

	for (i = 0; i < ARRAY_SIZE(pinmux_functions); ++i) {
		res = pinmux_generic_add_function(pctl->pctldev, pinmux_functions[i],
						  pin_names, npins, pctl);
		if (res < 0)
			return dev_err_probe(pctl->dev, res,
					     "Failed to register function.");
	}

	return apple_gpio_register(pctl);
}

/* Suspend-to-RAM */

static bool apple_gpio_s2r_skip(struct apple_gpio_pinctrl *pctl, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < pctl->hw->num_s2r_skip_pins; i++)
		if (pctl->hw->s2r_skip_pins[i] == pin)
			return true;

	return false;
}

/*
 * The bits that tell whether a pin kept its configuration. DATA only counts
 * on a GPIO output; on any other pin it follows the pad.
 */
static u32 apple_gpio_s2r_mask(u32 cfg)
{
	if (!FIELD_GET(REG_GPIOx_PERIPH, cfg) &&
	    FIELD_GET(REG_GPIOx_MODE, cfg) == REG_GPIOx_OUT)
		return ~0U;

	return ~(u32)REG_GPIOx_DATA;
}

/*
 * Suspend-to-RAM takes the AP block's power domain down with the SoC. Save
 * every pin after suspend_device_irqs() has masked the pins that are not
 * wake sources, so the saved IRQ modes are the ones the IRQ core unmasks
 * again on resume. s2idle stays as it was: only apple_s2r offers
 * PM_SUSPEND_MEM.
 */
static int apple_gpio_suspend_noirq(struct device *dev)
{
	struct apple_gpio_pinctrl *pctl = dev_get_drvdata(dev);
	unsigned int pin;

	if (!pctl->s2r_cfg || pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;

	for (pin = 0; pin < pctl->pinctrl_desc.npins; pin++)
		pctl->s2r_cfg[pin] = readl_relaxed(pctl->base + REG_GPIO(pin));
	pctl->s2r_input_enable = readl_relaxed(pctl->base + REG_INPUT_ENABLE);
	pctl->s2r_saved = true;

	return 0;
}

/*
 * By now genpd has the domain on again, or the power state driver (our
 * supplier) has if a wake source such as the keyboard's spien pin kept
 * genpd from turning it off. Only pins that changed are written, so this
 * does nothing when the domain kept its state. The writes go through the
 * regmap so its cache follows: regcache_sync() would skip every register
 * that equals the value read at probe. The IRQ status registers are left
 * alone, a bit latched there is for the IRQ handler to see.
 */
static int apple_gpio_resume_noirq(struct device *dev)
{
	struct apple_gpio_pinctrl *pctl = dev_get_drvdata(dev);
	unsigned int pin, first = 0, lost = 0, stuck = 0;
	u32 cfg, saved, first_cfg = 0;

	if (!pctl->s2r_saved)
		return 0;
	pctl->s2r_saved = false;

	/* First, so the pins below come back with their real input levels */
	cfg = readl_relaxed(pctl->base + REG_INPUT_ENABLE);
	if (cfg != pctl->s2r_input_enable) {
		writel_relaxed(pctl->s2r_input_enable, pctl->base + REG_INPUT_ENABLE);
		dev_dbg(dev, "restored the input enable after suspend-to-RAM (was 0x%x)\n", cfg);
	}

	for (pin = 0; pin < pctl->pinctrl_desc.npins; pin++) {
		if (apple_gpio_s2r_skip(pctl, pin))
			continue;

		saved = pctl->s2r_cfg[pin];
		cfg = readl_relaxed(pctl->base + REG_GPIO(pin));
		if (!((cfg ^ saved) & apple_gpio_s2r_mask(saved)))
			continue;

		if (!lost++) {
			first = pin;
			first_cfg = cfg;
		}

		if (regmap_write(pctl->map, REG_GPIO(pin), saved)) {
			stuck++;
			continue;
		}

		cfg = readl_relaxed(pctl->base + REG_GPIO(pin));
		if ((cfg ^ saved) & apple_gpio_s2r_mask(saved))
			stuck++;
	}

	if (!lost)
		dev_dbg(dev, "pins kept their configuration through suspend-to-RAM\n");
	else
		dev_dbg(dev, "restored %u pins after suspend-to-RAM (pin %u was 0x%08x, saved 0x%08x)\n",
			lost, first, first_cfg, pctl->s2r_cfg[first]);

	if (stuck)
		dev_warn(dev, "%u pins did not take their saved configuration\n", stuck);

	return 0;
}

static const struct dev_pm_ops apple_gpio_pinctrl_pm_ops = {
	.suspend_noirq = pm_sleep_ptr(apple_gpio_suspend_noirq),
	.resume_noirq = pm_sleep_ptr(apple_gpio_resume_noirq),
};

static const struct of_device_id apple_gpio_pinctrl_of_match[] = {
	{ .compatible = "apple,t8140-pinctrl", .data = &apple_gpio_uncached_hw },
	{ .compatible = "apple,t8132-pinctrl", .data = &apple_gpio_uncached_hw },
	{ .compatible = "apple,t6050-pinctrl", .data = &apple_gpio_uncached_hw },
	{ .compatible = "apple,t6000-pinctrl", .data = &apple_gpio_t6000_hw },
	{ .compatible = "apple,t8103-pinctrl", },
	{ .compatible = "apple,pinctrl", },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_gpio_pinctrl_of_match);

static struct platform_driver apple_gpio_pinctrl_driver = {
	.driver = {
		.name = "apple-gpio-pinctrl",
		.of_match_table = apple_gpio_pinctrl_of_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&apple_gpio_pinctrl_pm_ops),
	},
	.probe = apple_gpio_pinctrl_probe,
};
module_platform_driver(apple_gpio_pinctrl_driver);

MODULE_DESCRIPTION("Apple pinctrl/GPIO driver");
MODULE_AUTHOR("Stan Skowronek <stan@corellium.com>");
MODULE_AUTHOR("Joey Gouly <joey.gouly@arm.com>");
MODULE_LICENSE("GPL v2");
