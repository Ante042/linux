// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spin Table SMP initialisation
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/mm.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/io.h>
#include <asm/smp_plat.h>
#include <asm/spin_table.h>

extern void secondary_holding_pen(void);
volatile unsigned long __section(".mmuoff.data.read")
secondary_holding_pen_release = INVALID_HWID;

static phys_addr_t cpu_release_addr[NR_CPUS];

/*
 * Write secondary_holding_pen_release in a way that is guaranteed to be
 * visible to all observers, irrespective of whether they're taking part
 * in coherency or not.  This is necessary for the hotplug code to work
 * reliably.
 */
static void write_pen_release(u64 val)
{
	void *start = (void *)&secondary_holding_pen_release;
	unsigned long size = sizeof(secondary_holding_pen_release);

	secondary_holding_pen_release = val;
	dcache_clean_inval_poc((unsigned long)start, (unsigned long)start + size);
}


static int smp_spin_table_cpu_init(unsigned int cpu)
{
	struct device_node *dn;
	int ret;

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;

	/*
	 * Determine the address from which the CPU is polling.
	 */
	ret = of_property_read_u64(dn, "cpu-release-addr",
				   &cpu_release_addr[cpu]);
	if (ret)
		pr_err("CPU %d: missing or invalid cpu-release-addr property\n",
		       cpu);

	of_node_put(dn);

	return ret;
}

static int smp_spin_table_cpu_prepare(unsigned int cpu)
{
	__le64 __iomem *release_addr;
	phys_addr_t pa_holding_pen = __pa_symbol(secondary_holding_pen);

	if (!cpu_release_addr[cpu])
		return -ENODEV;

	/*
	 * The cpu-release-addr may or may not be inside the linear mapping.
	 * As ioremap_cache will either give us a new mapping or reuse the
	 * existing linear mapping, we can use it to cover both cases. In
	 * either case the memory will be MT_NORMAL.
	 */
	release_addr = ioremap_cache(cpu_release_addr[cpu],
				     sizeof(*release_addr));
	if (!release_addr)
		return -ENOMEM;

	/*
	 * We write the release address as LE regardless of the native
	 * endianness of the kernel. Therefore, any boot-loaders that
	 * read this address need to convert this address to the
	 * boot-loader's endianness before jumping. This is mandated by
	 * the boot protocol.
	 */
	writeq_relaxed(pa_holding_pen, release_addr);
	dcache_clean_inval_poc((__force unsigned long)release_addr,
			    (__force unsigned long)release_addr +
				    sizeof(*release_addr));

	/*
	 * Send an event to wake up the secondary CPU.
	 */
	sev();

	iounmap(release_addr);

	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static const struct spin_table_hotplug_ops *hotplug_ops __ro_after_init;

static bool smp_spin_table_cpu_can_disable(unsigned int cpu)
{
	if (!hotplug_ops)
		return false;
	return !hotplug_ops->cpu_can_disable || hotplug_ops->cpu_can_disable(cpu);
}

static int smp_spin_table_cpu_disable(unsigned int cpu)
{
	return smp_spin_table_cpu_can_disable(cpu) ? 0 : -EOPNOTSUPP;
}

static void smp_spin_table_cpu_die(unsigned int cpu)
{
	/* A reset that brings this CPU back to the pen must not let it out */
	write_pen_release(INVALID_HWID);

	if (hotplug_ops)
		hotplug_ops->cpu_die(cpu);
}

static int smp_spin_table_cpu_kill(unsigned int cpu)
{
	if (hotplug_ops && hotplug_ops->cpu_kill)
		return hotplug_ops->cpu_kill(cpu);
	return 0;
}
#endif

static int smp_spin_table_cpu_boot(unsigned int cpu)
{
	/*
	 * Update the pen release flag.
	 */
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Send an event, causing the secondaries to read pen_release.
	 */
	sev();

#ifdef CONFIG_HOTPLUG_CPU
	if (hotplug_ops && hotplug_ops->cpu_boot)
		return hotplug_ops->cpu_boot(cpu);
#endif
	return 0;
}

/* The hotplug methods are only filled in by spin_table_set_hotplug_ops() */
struct cpu_operations smp_spin_table_ops __ro_after_init = {
	.name		= "spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_spin_table_cpu_boot,
};

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Only a platform that can power its CPUs down and up again gets the hotplug
 * methods; elsewhere spin-table CPUs stay as they were. Called before the
 * secondary CPUs boot.
 */
void __init spin_table_set_hotplug_ops(const struct spin_table_hotplug_ops *ops)
{
	hotplug_ops = ops;
	smp_spin_table_ops.cpu_can_disable = smp_spin_table_cpu_can_disable;
	smp_spin_table_ops.cpu_disable = smp_spin_table_cpu_disable;
	smp_spin_table_ops.cpu_die = smp_spin_table_cpu_die;
	smp_spin_table_ops.cpu_kill = smp_spin_table_cpu_kill;
}
#endif
