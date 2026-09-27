// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SART device driver
 * Copyright (C) The Asahi Linux Contributors
 *
 * Apple SART is a simple address filter for some DMA transactions.
 * Regions of physical memory must be added to the SART's allow
 * list before any DMA can target these. Unlike a proper
 * IOMMU no remapping can be done and special support in the
 * consumer driver is required since not all DMA transactions of
 * a single device are subject to SART filtering.
 */

#include <linux/soc/apple/sart.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/dma-direct.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#define APPLE_SART_MAX_ENTRIES 16

/* One-operation sptm2mmio batch, shared with x1n1 src/sptm.c. */
struct sart_x1n1_batch {
	u32 count, flags;
	s32 status;
	u32 done;
	u32 op, op_flags;
	u64 arg[6], ret;
};

/* SARTv0 registers */
#define APPLE_SART0_CONFIG(idx)       (0x00 + 4 * (idx))
#define APPLE_SART0_CONFIG_FLAGS      GENMASK(28, 24)
#define APPLE_SART0_CONFIG_SIZE       GENMASK(18, 0)
#define APPLE_SART0_CONFIG_SIZE_SHIFT 12
#define APPLE_SART0_CONFIG_SIZE_MAX   GENMASK(18, 0)

#define APPLE_SART0_PADDR(idx)  (0x40 + 4 * (idx))
#define APPLE_SART0_PADDR_SHIFT 12

#define APPLE_SART0_FLAGS_ALLOW 0xf

/* SARTv2 registers */
#define APPLE_SART2_CONFIG(idx)	      (0x00 + 4 * (idx))
#define APPLE_SART2_CONFIG_FLAGS      GENMASK(31, 24)
#define APPLE_SART2_CONFIG_SIZE	      GENMASK(23, 0)
#define APPLE_SART2_CONFIG_SIZE_SHIFT 12
#define APPLE_SART2_CONFIG_SIZE_MAX   GENMASK(23, 0)

#define APPLE_SART2_PADDR(idx)	(0x40 + 4 * (idx))
#define APPLE_SART2_PADDR_SHIFT 12

#define APPLE_SART2_FLAGS_ALLOW 0xff

/* SARTv3 registers */
#define APPLE_SART3_CONFIG(idx) (0x00 + 4 * (idx))

#define APPLE_SART3_PADDR(idx)	(0x40 + 4 * (idx))
#define APPLE_SART3_PADDR_SHIFT 12

#define APPLE_SART3_SIZE(idx)  (0x80 + 4 * (idx))
#define APPLE_SART3_SIZE_SHIFT 12
#define APPLE_SART3_SIZE_MAX   GENMASK(29, 0)

#define APPLE_SART3_FLAGS_ALLOW 0xff

struct apple_sart_ops {
	void (*get_entry)(struct apple_sart *sart, int index, u8 *flags,
			  phys_addr_t *paddr, size_t *size);
	void (*set_entry)(struct apple_sart *sart, int index, u8 flags,
			  phys_addr_t paddr_shifted, size_t size_shifted);
	/* This is probably a bitfield but the exact meaning of each bit is unknown. */
	unsigned int flags_allow;
	unsigned int size_shift;
	unsigned int paddr_shift;
	size_t size_max;
};

struct apple_sart {
	struct device *dev;
	void __iomem *regs;

	const struct apple_sart_ops *ops;

	unsigned long protected_entries;
	unsigned long used_entries;
	bool x1n1, x1n1_active;
	struct mutex x1n1_lock;
	struct sart_x1n1_batch *batch;
	struct {
		phys_addr_t paddr;
		size_t size;
	} regions[APPLE_SART_MAX_ENTRIES];
};

/* Caller holds x1n1_lock. SPTM re-dispatch changes SP_EL1, so retain the
 * kernel SP in a GPR exactly as the qualified DART transport does.
 */
static int sart_x1n1_call(struct apple_sart *sart, u32 op,
			 u64 a0, u64 a1, u64 a2, u64 a3)
{
	struct sart_x1n1_batch *b = sart->batch;
	unsigned long flags;
	u64 pa = virt_to_phys(b);
	void __iomem *db = sart->regs + 0x1000;

	static_assert(sizeof(*b) == 80);
	memset(b, 0, sizeof(*b));
	b->count = 1;
	b->op = op;
	b->arg[0] = a0;
	b->arg[1] = a1;
	b->arg[2] = a2;
	b->arg[3] = a3;
	wmb();
	local_irq_save(flags);
	asm volatile("mov x9, sp\n"
		     "str %x[pa], [%[db]]\n"
		     "mov sp, x9\n"
		     : : [pa] "r" (pa), [db] "r" (db) : "x9", "memory");
	local_irq_restore(flags);
	rmb();
	return b->status || b->done != 1 || b->ret ? -EIO : 0;
}

static int sart_x1n1_region(struct apple_sart *sart, phys_addr_t paddr,
			    size_t size, bool map)
{
	int i, ret;

	if (!size || (paddr | size) & (SZ_16K - 1))
		return -EINVAL;
	mutex_lock(&sart->x1n1_lock);
	if (!sart->x1n1_active) {
		ret = sart_x1n1_call(sart, 1, 1, 0, 0, 0);
		if (ret)
			goto out;
		sart->x1n1_active = true;
	}
	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++) {
		if (map ? !sart->regions[i].size :
		    (sart->regions[i].paddr == paddr && sart->regions[i].size == size))
			break;
	}
	if (i == APPLE_SART_MAX_ENTRIES) {
		ret = map ? -EBUSY : -ENOENT;
		goto out;
	}
	/* Consumers supply DMA bus addresses. Reverse the DT dma-ranges offset
	 * before the HV validates/translates the guest IPA to its backing page.
	 * perm=1 permits device writes; guard=1 holds the firmware power canary.
	 */
	ret = sart_x1n1_call(sart, map ? 2 : 3,
			     dma_to_phys(sart->dev, paddr), size, map, map);
	if (!ret) {
		sart->regions[i].paddr = map ? paddr : 0;
		sart->regions[i].size = map ? size : 0;
	}
out:
	mutex_unlock(&sart->x1n1_lock);
	return ret;
}

static void sart_x1n1_selftest(struct apple_sart *sart)
{
	void *page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	phys_addr_t pa;
	int ret;

	if (!page)
		return;
	pa = virt_to_phys(page);
	ret = sart_x1n1_region(sart, phys_to_dma(sart->dev, pa), PAGE_SIZE, true);
	dev_info(sart->dev, "x1n1 selftest: map %pa size %#lx status %d\n",
		 &pa, PAGE_SIZE, ret);
	if (!ret) {
		ret = sart_x1n1_region(sart, phys_to_dma(sart->dev, pa), PAGE_SIZE, false);
		dev_info(sart->dev, "x1n1 selftest: unmap status %d\n", ret);
		if (ret)
			return; /* Do not free memory while a DMA grant might remain. */
	}
	free_page((unsigned long)page);
}

static void sart0_get_entry(struct apple_sart *sart, int index, u8 *flags,
	phys_addr_t *paddr, size_t *size)
{
	u32 cfg = readl(sart->regs + APPLE_SART0_CONFIG(index));
	phys_addr_t paddr_ = readl(sart->regs + APPLE_SART0_PADDR(index));
	size_t size_ = FIELD_GET(APPLE_SART0_CONFIG_SIZE, cfg);

	*flags = FIELD_GET(APPLE_SART0_CONFIG_FLAGS, cfg);
	*size = size_ << APPLE_SART0_CONFIG_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART0_PADDR_SHIFT;
}

static void sart0_set_entry(struct apple_sart *sart, int index, u8 flags,
	phys_addr_t paddr_shifted, size_t size_shifted)
{
	u32 cfg;

	cfg = FIELD_PREP(APPLE_SART0_CONFIG_FLAGS, flags);
	cfg |= FIELD_PREP(APPLE_SART0_CONFIG_SIZE, size_shifted);

	writel(paddr_shifted, sart->regs + APPLE_SART0_PADDR(index));
	writel(cfg, sart->regs + APPLE_SART0_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v0 = {
	.get_entry = sart0_get_entry,
	.set_entry = sart0_set_entry,
	.flags_allow = APPLE_SART0_FLAGS_ALLOW,
	.size_shift = APPLE_SART0_CONFIG_SIZE_SHIFT,
	.paddr_shift = APPLE_SART0_PADDR_SHIFT,
	.size_max = APPLE_SART0_CONFIG_SIZE_MAX,
};

static void sart2_get_entry(struct apple_sart *sart, int index, u8 *flags,
			    phys_addr_t *paddr, size_t *size)
{
	u32 cfg = readl(sart->regs + APPLE_SART2_CONFIG(index));
	phys_addr_t paddr_ = readl(sart->regs + APPLE_SART2_PADDR(index));
	size_t size_ = FIELD_GET(APPLE_SART2_CONFIG_SIZE, cfg);

	*flags = FIELD_GET(APPLE_SART2_CONFIG_FLAGS, cfg);
	*size = size_ << APPLE_SART2_CONFIG_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART2_PADDR_SHIFT;
}

static void sart2_set_entry(struct apple_sart *sart, int index, u8 flags,
			    phys_addr_t paddr_shifted, size_t size_shifted)
{
	u32 cfg;

	cfg = FIELD_PREP(APPLE_SART2_CONFIG_FLAGS, flags);
	cfg |= FIELD_PREP(APPLE_SART2_CONFIG_SIZE, size_shifted);

	writel(paddr_shifted, sart->regs + APPLE_SART2_PADDR(index));
	writel(cfg, sart->regs + APPLE_SART2_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v2 = {
	.get_entry = sart2_get_entry,
	.set_entry = sart2_set_entry,
	.flags_allow = APPLE_SART2_FLAGS_ALLOW,
	.size_shift = APPLE_SART2_CONFIG_SIZE_SHIFT,
	.paddr_shift = APPLE_SART2_PADDR_SHIFT,
	.size_max = APPLE_SART2_CONFIG_SIZE_MAX,
};

static void sart3_get_entry(struct apple_sart *sart, int index, u8 *flags,
			    phys_addr_t *paddr, size_t *size)
{
	phys_addr_t paddr_ = readl(sart->regs + APPLE_SART3_PADDR(index));
	size_t size_ = readl(sart->regs + APPLE_SART3_SIZE(index));

	*flags = readl(sart->regs + APPLE_SART3_CONFIG(index));
	*size = size_ << APPLE_SART3_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART3_PADDR_SHIFT;
}

static void sart3_set_entry(struct apple_sart *sart, int index, u8 flags,
			    phys_addr_t paddr_shifted, size_t size_shifted)
{
	writel(paddr_shifted, sart->regs + APPLE_SART3_PADDR(index));
	writel(size_shifted, sart->regs + APPLE_SART3_SIZE(index));
	writel(flags, sart->regs + APPLE_SART3_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v3 = {
	.get_entry = sart3_get_entry,
	.set_entry = sart3_set_entry,
	.flags_allow = APPLE_SART3_FLAGS_ALLOW,
	.size_shift = APPLE_SART3_SIZE_SHIFT,
	.paddr_shift = APPLE_SART3_PADDR_SHIFT,
	.size_max = APPLE_SART3_SIZE_MAX,
};

static int apple_sart_probe(struct platform_device *pdev)
{
	int i;
	struct apple_sart *sart;
	struct device *dev = &pdev->dev;

	sart = devm_kzalloc(dev, sizeof(*sart), GFP_KERNEL);
	if (!sart)
		return -ENOMEM;

	sart->dev = dev;
	sart->ops = of_device_get_match_data(dev);

	sart->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sart->regs))
		return PTR_ERR(sart->regs);
	if (of_device_is_compatible(dev->of_node, "apple,sart-x1n1")) {
		if (readq(sart->regs) != 0x78316e31 || readq(sart->regs + 8) != 1 ||
		    !(readq(sart->regs + 0x18) & BIT(1)))
			return -ENODEV;
		sart->batch = devm_kzalloc(dev, PAGE_SIZE, GFP_KERNEL);
		if (!sart->batch)
			return -ENOMEM;
		mutex_init(&sart->x1n1_lock);
		sart->x1n1 = true;
		platform_set_drvdata(pdev, sart);
		dev_info(dev, "x1n1 SART via sptm2mmio\n");
		if (of_property_read_bool(dev->of_node, "apple,x1n1-selftest"))
			sart_x1n1_selftest(sart);
		return 0;
	}

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		u8 flags;
		size_t size;
		phys_addr_t paddr;

		sart->ops->get_entry(sart, i, &flags, &paddr, &size);

		if (!flags)
			continue;

		dev_dbg(sart->dev,
			"SART bootloader entry: index %02d; flags: 0x%02x; paddr: %pa; size: 0x%zx\n",
			i, flags, &paddr, size);
		set_bit(i, &sart->protected_entries);
	}

	platform_set_drvdata(pdev, sart);
	return 0;
}

struct apple_sart *devm_apple_sart_get(struct device *dev)
{
	struct device_node *sart_node;
	struct platform_device *sart_pdev;
	struct apple_sart *sart;

	sart_node = of_parse_phandle(dev->of_node, "apple,sart", 0);
	if (!sart_node)
		return ERR_PTR(-ENODEV);

	sart_pdev = of_find_device_by_node(sart_node);
	of_node_put(sart_node);

	if (!sart_pdev)
		return ERR_PTR(-ENODEV);

	sart = dev_get_drvdata(&sart_pdev->dev);
	if (!sart) {
		put_device(&sart_pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	device_link_add(dev, &sart_pdev->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);

	put_device(&sart_pdev->dev);

	return sart;
}
EXPORT_SYMBOL_GPL(devm_apple_sart_get);

static int sart_set_entry(struct apple_sart *sart, int index, u8 flags,
			  phys_addr_t paddr, size_t size)
{
	if (size & ((1 << sart->ops->size_shift) - 1))
		return -EINVAL;
	if (paddr & ((1 << sart->ops->paddr_shift) - 1))
		return -EINVAL;

	paddr >>= sart->ops->size_shift;
	size >>= sart->ops->paddr_shift;

	if (size > sart->ops->size_max)
		return -EINVAL;

	sart->ops->set_entry(sart, index, flags, paddr, size);
	return 0;
}

int apple_sart_add_allowed_region(struct apple_sart *sart, phys_addr_t paddr,
				  size_t size)
{
	int i, ret;

	if (sart->x1n1)
		return sart_x1n1_region(sart, paddr, size, true);

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		if (test_bit(i, &sart->protected_entries))
			continue;
		if (test_and_set_bit(i, &sart->used_entries))
			continue;

		ret = sart_set_entry(sart, i, sart->ops->flags_allow, paddr,
				     size);
		if (ret) {
			dev_dbg(sart->dev,
				"unable to set entry %d to [%pa, 0x%zx]\n",
				i, &paddr, size);
			clear_bit(i, &sart->used_entries);
			return ret;
		}

		dev_dbg(sart->dev, "wrote [%pa, 0x%zx] to %d\n", &paddr, size,
			i);
		return 0;
	}

	dev_warn(sart->dev,
		 "no free entries left to add [paddr: 0x%pa, size: 0x%zx]\n",
		 &paddr, size);

	return -EBUSY;
}
EXPORT_SYMBOL_GPL(apple_sart_add_allowed_region);

int apple_sart_remove_allowed_region(struct apple_sart *sart, phys_addr_t paddr,
				     size_t size)
{
	int i;

	if (sart->x1n1)
		return sart_x1n1_region(sart, paddr, size, false);

	dev_dbg(sart->dev,
		"will remove [paddr: %pa, size: 0x%zx] from allowed regions\n",
		&paddr, size);

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		u8 eflags;
		size_t esize;
		phys_addr_t epaddr;

		if (test_bit(i, &sart->protected_entries))
			continue;

		sart->ops->get_entry(sart, i, &eflags, &epaddr, &esize);

		if (epaddr != paddr || esize != size)
			continue;

		sart->ops->set_entry(sart, i, 0, 0, 0);

		clear_bit(i, &sart->used_entries);
		dev_dbg(sart->dev, "cleared entry %d\n", i);
		return 0;
	}

	dev_warn(sart->dev, "entry [paddr: 0x%pa, size: 0x%zx] not found\n",
		 &paddr, size);

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(apple_sart_remove_allowed_region);

static void apple_sart_shutdown(struct platform_device *pdev)
{
	struct apple_sart *sart = dev_get_drvdata(&pdev->dev);
	int i;

	if (sart->x1n1) {
		for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++)
			if (sart->regions[i].size)
				sart_x1n1_region(sart, sart->regions[i].paddr,
						 sart->regions[i].size, false);
		return;
	}

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		if (test_bit(i, &sart->protected_entries))
			continue;

		sart->ops->set_entry(sart, i, 0, 0, 0);
	}
}

static const struct of_device_id apple_sart_of_match[] = {
	{ .compatible = "apple,sart-x1n1" },
	{
		.compatible = "apple,t6000-sart",
		.data = &sart_ops_v3,
	},
	{
		.compatible = "apple,t8103-sart",
		.data = &sart_ops_v2,
	},
	{
		.compatible = "apple,t8015-sart",
		.data = &sart_ops_v0,
	},
	{}
};
MODULE_DEVICE_TABLE(of, apple_sart_of_match);

static struct platform_driver apple_sart_driver = {
	.driver = {
		.name = "apple-sart",
		.of_match_table = apple_sart_of_match,
	},
	.probe = apple_sart_probe,
	.shutdown = apple_sart_shutdown,
};
module_platform_driver(apple_sart_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple SART driver");
