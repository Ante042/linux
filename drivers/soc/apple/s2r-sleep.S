/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple Silicon CPU power down without state retention
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>

#define SYS_IMP_APL_CYC_OVRD	sys_reg(3, 5, 15, 5, 0)
#define SYS_IMP_APL_ACC_OVRD	sys_reg(3, 5, 15, 6, 0)
#define SYS_IMP_APL_SIQ_CFG	sys_reg(3, 4, 15, 10, 4)

/*
 * void apple_s2r_cpu_off(u64 cyc_ovrd, u64 acc_ovrd, bool flush_l1)
 *
 * Writes back the L1 data cache by set/way as XNU does before a core goes
 * down (the L2 and the SLC are handled by the hardware), programs the
 * overrides and waits in WFI. Without the deep sleep overrides it also sets
 * SIQ_CFG[1:0] as XNU does for a core going offline. Timers are stopped first. The core does not
 * retain state, so it only comes back through a reset. Uses no stack once the
 * data cache is off.
 */
SYM_FUNC_START(apple_s2r_cpu_off)
	mov	x11, #0
	b	.Lcpu_off
SYM_FUNC_END(apple_s2r_cpu_off)

/*
 * int apple_s2r_cpu_sleep(u64 cyc_ovrd, u64 acc_ovrd, bool flush_l1)
 *
 * The same for the last CPU going to sleep, which keeps its IRQs routed so
 * that a wake source can bring it back. When an IRQ is already pending the
 * core does not power down and WFI falls through; turn the data cache back
 * on and return -EBUSY instead of spinning there forever.
 */
SYM_FUNC_START(apple_s2r_cpu_sleep)
	mov	x11, #1
.Lcpu_off:
	msr	daifset, #0xf

	cbz	x2, 3f

	mrs	x3, sctlr_el1
	bic	x3, x3, #SCTLR_ELx_C
	msr	sctlr_el1, x3
	isb

	msr	csselr_el1, xzr
	isb
	mrs	x3, ccsidr_el1
	and	x4, x3, #7
	add	x4, x4, #4			// set shift
	ubfx	x5, x3, #3, #10			// highest way
	ubfx	x6, x3, #13, #15		// highest set
	clz	w7, w5				// way shift
1:	mov	x8, x6
2:	lsl	x9, x5, x7
	lsl	x10, x8, x4
	orr	x9, x9, x10
	dc	cisw, x9
	subs	x8, x8, #1
	b.ge	2b
	subs	x5, x5, #1
	b.ge	1b
	dsb	sy
	isb

	/* A pending timer, even a masked one, aborts the power down */
3:	msr	cntp_ctl_el0, xzr
	msr	cntv_ctl_el0, xzr
	mrs	x3, CurrentEL
	cmp	x3, #(2 << 2)
	b.ne	8f
	msr_s	SYS_CNTP_CTL_EL02, xzr
	msr_s	SYS_CNTV_CTL_EL02, xzr
8:

	cbz	x1, 6f
	msr_s	SYS_IMP_APL_ACC_OVRD, x1
	b	4f
6:	cbnz	x11, 4f
	mrs_s	x3, SYS_IMP_APL_SIQ_CFG
	orr	x3, x3, #3
	msr_s	SYS_IMP_APL_SIQ_CFG, x3
4:	msr_s	SYS_IMP_APL_CYC_OVRD, x0
	isb
5:	dsb	sy
	wfi
	cbnz	x11, 7f
	b	5b

7:
	mrs	x3, sctlr_el1
	orr	x3, x3, #SCTLR_ELx_C
	msr	sctlr_el1, x3
	isb
	mov	x0, #-16			// -EBUSY
	ret
SYM_FUNC_END(apple_s2r_cpu_sleep)
