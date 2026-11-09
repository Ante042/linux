/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_SPIN_TABLE_H
#define __ASM_SPIN_TABLE_H

#include <asm/cpu_ops.h>

extern struct cpu_operations smp_spin_table_ops;

/*
 * The spin-table boot protocol has no way to take a CPU away. A platform
 * whose CPUs restart through the boot loader's spin table after a power
 * cycle can provide CPU hotplug with these.
 */
struct spin_table_hotplug_ops {
	/* Whether a CPU can go, optional */
	bool (*cpu_can_disable)(unsigned int cpu);
	/* Power the calling CPU down. Must not return. */
	void (*cpu_die)(unsigned int cpu);
	/* Confirm from another CPU that a dying CPU is gone */
	int (*cpu_kill)(unsigned int cpu);
	/* Power a dead CPU back up once its pen release is written */
	int (*cpu_boot)(unsigned int cpu);
};

#ifdef CONFIG_HOTPLUG_CPU
void spin_table_set_hotplug_ops(const struct spin_table_hotplug_ops *ops);
#else
static inline void spin_table_set_hotplug_ops(const struct spin_table_hotplug_ops *ops) {}
#endif

#endif
