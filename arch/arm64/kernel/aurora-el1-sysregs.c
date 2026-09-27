// SPDX-License-Identifier: GPL-2.0-only
/* Explicitly opt-in, bounded, read-only EL1 sysreg diagnostic. */
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <asm/aurora-el1-sysregs.h>
#include <asm/daifflags.h>
#include <asm/esr.h>
#include <asm/sysreg.h>

#include "aurora-el1-sysregs-list.h"

#define PROBE_COOKIE 0x53595245474c4e58ULL
#define PROBE_UNDEF 0xbad0bad0bad0bad0ULL
#define PROBE_TRAP 0xe12e12e12e12e12eULL

struct probe_value {
	u64 value, status, esr, raw_esr;
};

struct probe_capture {
	struct probe_value values[ARRAY_SIZE(probe_opcodes)];
	u64 mpidr, midr;
	int cpu, armed, disarmed;
};

struct probe_active {
	unsigned long pc;
	u32 opcode;
};

static DEFINE_PER_CPU(struct probe_active, probe_active);
static bool probe_enabled;

extern char aurora_el1_sysregs_begin[], aurora_el1_sysregs_end[];
extern const unsigned long aurora_el1_sysregs_sites[];
extern void aurora_el1_sysreg_read(struct probe_value *result, unsigned int index);

static int __init aurora_el1_sysregs_param(char *str)
{
	return kstrtobool(str, &probe_enabled);
}
early_param("aurora_el1_sysregs", aurora_el1_sysregs_param);

/* Invoked only from do_el1_undef, after the instruction was safely read.
 * No other UNDEF or exception-table entry is granted recovery. */
bool aurora_el1_sysreg_undef(struct pt_regs *regs, unsigned long esr, u32 insn)
{
	struct probe_active *active = this_cpu_ptr(&probe_active);
	unsigned long pc = READ_ONCE(active->pc);

	if (!READ_ONCE(probe_enabled) || !pc || regs->pc != pc ||
	    ESR_ELx_EC(esr) != ESR_ELx_EC_UNKNOWN ||
	    insn != READ_ONCE(active->opcode) ||
	    (insn && (insn & 0xffe0001f) != 0xd5200000))
		return false;
	regs->regs[0] = PROBE_UNDEF;
	regs->regs[1] = 1;
	regs->regs[2] = esr;
	regs->regs[3] = esr;
	regs->pc += 4;
	return true;
}

static u64 probe_hvc(unsigned long begin, unsigned long end)
{
	register u64 x0 asm("x0") = PROBE_COOKIE;
	register u64 x1 asm("x1") = begin;
	register u64 x2 asm("x2") = end;

	asm volatile("hvc #0x5352" : "+r" (x0), "+r" (x1), "+r" (x2)
		     : : "x3", "x4", "x5", "x6", "x7", "memory", "cc");
	return x0;
}

static void probe_this_cpu(void *arg)
{
	struct probe_capture *capture = arg;
	struct probe_active *active = this_cpu_ptr(&probe_active);
	unsigned long flags;
	unsigned int i;

	/* The synchronous callback is pinned with IRQs disabled. Preserve the
	 * caller's complete DAIF state, including FIQ/SError/debug masks. */
	flags = local_daif_save();
	capture->cpu = raw_smp_processor_id();
	capture->mpidr = read_sysreg(mpidr_el1);
	capture->midr = read_sysreg(midr_el1);
	capture->armed = probe_hvc((unsigned long)aurora_el1_sysregs_begin,
		(unsigned long)aurora_el1_sysregs_end) == PROBE_COOKIE;
	if (!capture->armed)
		goto out;
	for (i = 0; i < ARRAY_SIZE(probe_opcodes); i++) {
		WRITE_ONCE(active->opcode, probe_opcodes[i]);
		WRITE_ONCE(active->pc, aurora_el1_sysregs_sites[i]);
		barrier();
		aurora_el1_sysreg_read(&capture->values[i], i);
		barrier();
		WRITE_ONCE(active->pc, 0);
	}
	capture->disarmed = probe_hvc(0, 0) == PROBE_COOKIE;
out:
	WRITE_ONCE(active->pc, 0);
	local_daif_restore(flags);
}

static int __init aurora_el1_sysregs_init(void)
{
	struct probe_capture capture;
	int cpu, ret, failures = 0, tested = 0;
	unsigned int i;

	if (!probe_enabled)
		return 0;
	pr_info("AURORA_LINUX_SYSREG_BEGIN count=%zu\n", ARRAY_SIZE(probe_opcodes));
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		bool controls_ok;

		memset(&capture, 0, sizeof(capture));
		ret = smp_call_function_single(cpu, probe_this_cpu, &capture, 1);
		if (ret) {
			pr_err("AURORA_LINUX_SYSREG_CPU_FAILED cpu=%d error=%d\n", cpu, ret);
			failures++;
			continue;
		}
		pr_info("AURORA_LINUX_SYSREG_CPU cpu=%d mpidr=%016llx midr=%016llx armed=%d disarmed=%d\n",
			capture.cpu, capture.mpidr, capture.midr, capture.armed, capture.disarmed);
		if (!capture.armed || !capture.disarmed) {
			failures++;
			break;
		}
		controls_ok = capture.values[0].status == 0 && capture.values[0].value == 4 &&
			capture.values[1].status == 1 && capture.values[1].value == PROBE_UNDEF &&
			capture.values[4].status == 2 && capture.values[4].value == PROBE_TRAP;
		for (i = 0; i < ARRAY_SIZE(probe_opcodes); i++) {
			struct probe_value *v = &capture.values[i];

			pr_info("AURORA_LINUX_SYSREG_READ cpu=%d index=%u insn=%08x value=%016llx status=%llu esr=%016llx raw=%016llx\n",
				cpu, i, probe_opcodes[i], v->value, v->status, v->esr, v->raw_esr);
			if (v->status > 2 || (v->status == 1 && v->value != PROBE_UNDEF) ||
			    (v->status == 2 && v->value != PROBE_TRAP))
				controls_ok = false;
		}
		pr_info("AURORA_LINUX_SYSREG_CPU_DONE cpu=%d controls=%s\n",
			cpu, controls_ok ? "PASS" : "FAIL");
		tested++;
		if (!controls_ok)
			failures++;
	}
	cpus_read_unlock();
	pr_info("AURORA_LINUX_SYSREG_DONE cpus=%d failures=%d\n", tested, failures);
	return failures ? -EIO : 0;
}
late_initcall(aurora_el1_sysregs_init);
