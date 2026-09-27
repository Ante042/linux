/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_AURORA_EL1_SYSREGS_H
#define __ASM_AURORA_EL1_SYSREGS_H

#include <linux/types.h>
#include <asm/ptrace.h>

#ifdef CONFIG_AURORA_EL1_SYSREG_PROBE
bool aurora_el1_sysreg_undef(struct pt_regs *regs, unsigned long esr, u32 insn);
#else
static inline bool aurora_el1_sysreg_undef(struct pt_regs *regs,
					 unsigned long esr, u32 insn)
{
	return false;
}
#endif
#endif
