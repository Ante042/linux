/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_IDLE_H
#define __ASM_APPLE_IDLE_H

#include <linux/jump_label.h>

/* WFI that preserves x18-x30 on cores that lose them (T8140/T6050). */
void apple_cpu_wfi(void);

#ifdef CONFIG_ARCH_APPLE
DECLARE_STATIC_KEY_FALSE(apple_wfi_loses_regs);
static __always_inline bool apple_wfi_needs_save(void)
{
	return static_branch_unlikely(&apple_wfi_loses_regs);
}
#else
static __always_inline bool apple_wfi_needs_save(void)
{
	return false;
}
#endif

#endif
