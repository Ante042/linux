// SPDX-License-Identifier: GPL-2.0
/* PCI host for an Apple controller initialized and owned by x1n1.
 * ECAM and BAR windows are direct guest mappings. MSI writes are intercepted
 * by the physical controller and delivered to its native AIC IRQ range.
 *
 * Hierarchical MSI allocation follows drivers/pci/controller/pcie-apple.c:
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2021 Google LLC
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 * Original authors: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 *                   Marc Zyngier <maz@kernel.org>
 */
#include <linux/bitmap.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of_irq.h>
#include <linux/pci-ecam.h>
#include <linux/pci-apple-x1n1.h>
#include "t6050.h"
#include <linux/platform_device.h>
#include "../pci-host-common.h"

struct x1n1_pcie {
	struct t6050_pcie *t6050;
	struct mutex lock;
	unsigned long *bitmap;
	u32 nvecs;
	u64 doorbell;
	struct irq_fwspec fwspec;
	struct irq_domain *msi_domain;
};

static void x1n1_msi_compose(struct irq_data *data, struct msi_msg *msg)
{
	struct x1n1_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_hi = upper_32_bits(pcie->doorbell);
	msg->address_lo = lower_32_bits(pcie->doorbell);
	msg->data = data->hwirq;
}

static struct irq_chip x1n1_msi_chip = {
	.name = "x1n1-PCI-MSI",
	.irq_mask = irq_chip_mask_parent,
	.irq_unmask = irq_chip_unmask_parent,
	.irq_eoi = irq_chip_eoi_parent,
	.irq_set_affinity = irq_chip_set_affinity_parent,
	.irq_set_type = irq_chip_set_type_parent,
	.irq_compose_msi_msg = x1n1_msi_compose,
};

static int x1n1_msi_alloc(struct irq_domain *domain, unsigned int virq,
			  unsigned int nr_irqs, void *args)
{
	struct x1n1_pcie *pcie = domain->host_data;
	struct irq_fwspec fwspec = pcie->fwspec;
	int hwirq, ret;

	mutex_lock(&pcie->lock);
	hwirq = bitmap_find_free_region(pcie->bitmap, pcie->nvecs,
					order_base_2(nr_irqs));
	mutex_unlock(&pcie->lock);
	if (hwirq < 0)
		return -ENOSPC;

	fwspec.param[fwspec.param_count - 2] += hwirq;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);
	if (ret) {
		mutex_lock(&pcie->lock);
		bitmap_release_region(pcie->bitmap, hwirq, order_base_2(nr_irqs));
		mutex_unlock(&pcie->lock);
		return ret;
	}
	for (unsigned int i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &x1n1_msi_chip, pcie);
	return 0;
}

static void x1n1_msi_free(struct irq_domain *domain, unsigned int virq,
			  unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	struct x1n1_pcie *pcie = domain->host_data;

	mutex_lock(&pcie->lock);
	bitmap_release_region(pcie->bitmap, data->hwirq, order_base_2(nr_irqs));
	mutex_unlock(&pcie->lock);
}

static const struct irq_domain_ops x1n1_msi_domain_ops = {
	.alloc = x1n1_msi_alloc,
	.free = x1n1_msi_free,
};

static const struct msi_parent_ops x1n1_msi_parent_ops = {
	.supported_flags = MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX |
			   MSI_FLAG_MULTI_PCI_MSI,
	.required_flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
			  MSI_FLAG_PCI_MSI_MASK_PARENT,
	.chip_flags = MSI_CHIP_FLAG_SET_EOI,
	.bus_select_token = DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info = msi_lib_init_dev_msi_info,
};

static void x1n1_msi_cleanup(void *data)
{
	struct x1n1_pcie *pcie = data;

	irq_domain_remove(pcie->msi_domain);
}

static int x1n1_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct x1n1_pcie *pcie;
	struct of_phandle_args args;
	struct irq_domain_info info = {};
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;
	pcie = pci_host_bridge_priv(bridge);
	mutex_init(&pcie->lock);
	pcie->t6050 = t6050_pcie_init(pdev);
	if (IS_ERR(pcie->t6050))
		return PTR_ERR(pcie->t6050);

	ret = of_property_read_u64(dev->of_node, "apple,msi-address", &pcie->doorbell);
	if (ret || (pcie->doorbell & 0xf))
		return dev_err_probe(dev, -EINVAL, "missing/unaligned MSI doorbell\n");
	ret = of_parse_phandle_with_args(dev->of_node, "msi-ranges",
					 "#interrupt-cells", 0, &args);
	if (ret)
		return dev_err_probe(dev, ret, "missing MSI IRQ range\n");
	/* This transport exposes the native four-cell AIC IRQ binding. */
	if (args.args_count != 4 || args.args[0] != 0 || args.args[3] != IRQ_TYPE_EDGE_RISING) {
		of_node_put(args.np);
		return dev_err_probe(dev, -EINVAL, "MSI range must use AIC edge IRQs\n");
	}
	ret = of_property_read_u32_index(dev->of_node, "msi-ranges", 5, &pcie->nvecs);
	if (ret || !is_power_of_2(pcie->nvecs) || pcie->nvecs > 256 ||
	    args.args[2] > U32_MAX - pcie->nvecs) {
		of_node_put(args.np);
		return dev_err_probe(dev, -EINVAL, "invalid MSI vector count\n");
	}
	of_phandle_args_to_fwspec(args.np, args.args, args.args_count, &pcie->fwspec);
	of_node_put(args.np);
	info.parent = irq_find_matching_fwspec(&pcie->fwspec, DOMAIN_BUS_WIRED);
	if (!info.parent)
		return -EPROBE_DEFER;
	pcie->bitmap = devm_bitmap_zalloc(dev, pcie->nvecs, GFP_KERNEL);
	if (!pcie->bitmap)
		return -ENOMEM;
	info.fwnode = dev_fwnode(dev);
	info.ops = &x1n1_msi_domain_ops;
	info.size = pcie->nvecs;
	info.host_data = pcie;
	pcie->msi_domain = msi_create_parent_irq_domain(&info, &x1n1_msi_parent_ops);
	if (!pcie->msi_domain)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, x1n1_msi_cleanup, pcie);
	if (ret)
		return ret;
	dev_info(dev, "%u MSI vectors at AIC %u, doorbell %#llx\n",
		 pcie->nvecs, pcie->fwspec.param[2], pcie->doorbell);
	return pci_host_common_init(pdev, bridge, &pci_generic_ecam_ops);
}

int apple_x1n1_pcie_cycle(struct pci_dev *pdev)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);
	struct x1n1_pcie *pcie;

	if (!bridge || !of_device_is_compatible(bridge->dev.parent->of_node,
						"apple,x1n1-pcie"))
		return -ENODEV;
	pcie = pci_host_bridge_priv(bridge);
	if (!pcie->t6050)
		return -EOPNOTSUPP;
	return t6050_pcie_cycle(pcie->t6050, pdev);
}
EXPORT_SYMBOL_GPL(apple_x1n1_pcie_cycle);

static const struct of_device_id x1n1_pcie_match[] = {
	{ .compatible = "apple,x1n1-pcie" },
	{},
};
MODULE_DEVICE_TABLE(of, x1n1_pcie_match);

static struct platform_driver x1n1_pcie_driver = {
	.probe = x1n1_pcie_probe,
	.remove = pci_host_common_remove,
	.driver = {
		.name = "pcie-apple-x1n1",
		.of_match_table = x1n1_pcie_match,
	},
};
module_platform_driver(x1n1_pcie_driver);
MODULE_DESCRIPTION("x1n1 preinitialized Apple PCIe host with native AIC MSI");
MODULE_LICENSE("GPL");
