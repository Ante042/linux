// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Silicon suspend-to-RAM
 *
 * Waking from S2R resets the CPUs to RVBAR, which iBoot locked to m1n1, so
 * the boot loader takes part. m1n1 publishes a mailbox in
 * /chosen/asahi,s2r-mailbox (layout in m1n1's src/s2r.h): the boot CPU
 * leaves its resume entry there, and secondaries that reset without one go
 * back to m1n1's spin table and from there to the holding pen. That also
 * gives spin-table CPUs hotplug, which suspend needs.
 *
 * The sequence follows macOS 13 on t6000 (XNU ml_arm_sleep and
 * arm64_prepare_for_sleep, AppleSMC, AppleT6000PMGR):
 *  - SMC MBSE 'slpw' when sleep starts, 'slpa' once devices are suspended,
 *    'waka' after wake, with the wake reason read back from MBSE and MBSW
 *  - secondaries park until the boot CPU commits, since the global deep
 *    sleep bit hangs a CPU that idles in WFI before then, and then all CPUs
 *    write back their L1, set the deep sleep overrides and wait in WFI
 *    without retention. The SoC sleeps once every cluster is down.
 *
 * On t6000 it also follows the rest of the macOS sequence: taking the
 * display blocks, the GPU and AOP firmware and the PMP down, starting the
 * PMGR S2R timer last, arming a watchdog deadline across the sleep, and
 * powering the PMGR domains the sleep lost back on before devices resume.
 *
 * The SMC rings its mailbox for every event, so closing the lid or plugging
 * in the charger wakes the AP as well. Such wakes go back to sleep unless a
 * driver reported a wakeup event.
 *
 * Everything that is not needed for CPU hotplug stays off until
 * apple_s2r.enable=1.
 *
 * Only t6000 (M1 Pro) is supported. On any other machine the driver leaves
 * the m1n1 mailbox alone, so it offers neither CPU hotplug nor deep sleep.
 */

#define pr_fmt(fmt) "apple-s2r: " fmt

#include <linux/bitfield.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/hex.h>
#include <linux/iopoll.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/s2r.h>
#include <linux/suspend.h>

#include <asm/arch_timer.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/spin_table.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>

#include "s2r-sleep.h"

#define SYS_IMP_APL_CYC_OVRD		sys_reg(3, 5, 15, 5, 0)
#define CYC_OVRD_DISABLE_WFI_RET	BIT(0)
#define CYC_OVRD_FIQ_MODE		GENMASK(21, 20)
#define CYC_OVRD_IRQ_MODE		GENMASK(23, 22)
#define CYC_OVRD_WFI_MODE		GENMASK(25, 24)

#define SYS_IMP_APL_ACC_OVRD		sys_reg(3, 5, 15, 6, 0)
#define ACC_OVRD_PWR_DN_SRM		GENMASK(14, 13)
#define ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP	GENMASK(16, 15)
#define ACC_OVRD_TRAIN_DOWN_LINK	GENMASK(18, 17)
#define ACC_OVRD_POWER_DOWN_CPM		GENMASK(26, 25)
#define ACC_OVRD_DISABLE_PIO_ON_WFI_CPU	BIT(32)
#define ACC_OVRD_DEEP_SLEEP		BIT(34)

/* AICv2 delivers IRQs to any CPU whose route is not none */
#define SYS_IMP_APL_SIQ_CFG		sys_reg(3, 4, 15, 10, 4)
#define SIQ_CFG_ROUTE			GENMASK(1, 0)
#define SIQ_CFG_ROUTE_NONE		3

#define SYS_IMP_APL_IPI_RR_GLOBAL	sys_reg(3, 5, 15, 0, 1)
#define SYS_IMP_APL_IPI_SR_EL1		sys_reg(3, 5, 15, 1, 1)
#define SYS_IMP_APL_PMCR0_EL1		sys_reg(3, 1, 15, 0, 0)

#define CPU_IMPL_RVBAR			0x0
/* Bits 44 and up hold per-core fields that read back differently on each core */
#define CPU_IMPL_RVBAR_ADDR		GENMASK(43, 12)
#define CPU_IMPL_PWRSTATE		0x100

#define PMGR_CPU_STOP			0x0
#define PMGR_CPU_START			0x4

/*
 * AP wake sources in the always-on PMGR block of t6000: ADT /arm-io/pmgr
 * reg[7], which no DT node covers. One bit per ADT ap-wake-sources id, in 7
 * mask words followed by 7 status words. macOS clears exactly the ids it
 * wants before sleep, so 1 most likely masks. Not to be confused with
 * reg[42] at 0x28e200000, which hung the machine when written.
 */
#define T6000_PMGR_AP_WAKE_BASE		0x292004000ULL
/* SoC watchdog WD1, armed across system sleep as a dead-man */
#define T6000_WDT_BASE			0x2922b0000ULL
#define WDT_WD1_CUR			0x10
#define WDT_WD1_BITE			0x14
#define WDT_WD1_CTL			0x1c
#define WDT_CTL_RESET_EN		BIT(2)
#define WDT_TICKS_PER_S			24000000U
/*
 * WD1 bites this long after GO (at most 170 s, it stands still in S2R): armed
 * at syscore suspend instead, it resets the SoC before GO. Right after a wake
 * it restarts with S2R_WDT_WAKE_S, so a hung resume still ends in a reset.
 */
#define S2R_WDT_GO_ARM_S		120
#define S2R_WDT_WAKE_S			170
#define T6000_PMGR_AP_WAKE_SIZE		0x40
#define PMGR_AP_WAKE_MASK		0x08
#define PMGR_AP_WAKE_STATUS		0x24
#define PMGR_AP_WAKE_WORDS		7

/*
 * PMGR control word with "wait for the SEP to power gate" in bit 2, set at
 * boot (ADT /arm-io/pmgr reg[0] + 0x4c200). The S2R sequencer then waits for
 * a SEP power gate that never comes, since Linux never puts the SEP to
 * sleep. macOS sends the SEP its sleep message instead and only clears the
 * bit for the wait-sep-pg=0 boot-arg (AppleT6000PMGR::_enableWaitSEPPG).
 */
#define T6000_PMGR_SEP_PG		0x28e0cc200ULL
#define PMGR_WAIT_SEP_PG		BIT(2)

/*
 * Cluster power states (ADT /arm-io/pmgr reg[0] + 0x18000), one word per
 * soc-clusters entry, PACC0 and PACC1 in words 1 and 2: target in [3:0],
 * bit 4 set while it changes. AppleT6000PMGR::quiesceHW sets the target of
 * every cluster but the boot CPU's to 0 before the last WFI, so that each
 * can go down with its cores.
 */
#define T6000_PMGR_CLUSTER_PS		0x28e098000ULL
#define T6000_PMGR_CLUSTERS		9
#define CLUSTER_PS_TARGET		GENMASK(3, 0)
#define CLUSTER_PS_BUSY			BIT(4)

#define PS_TARGET			GENMASK(3, 0)
#define PS_ACTUAL			GENMASK(7, 4)
#define PS_AUTO_ENABLE			BIT(28)

#define T6000_PMGR_PS_BASE		0x28e080000ULL
#define T6000_PMGR_EAST_PS_BASE		0x28e580000ULL
#define T6000_PMGR_PS_SIZE		0x1000

/*
 * The rest of AppleT6000PMGR::quiesceHW, one SOCQ_ bit per step in the
 * order macOS takes them. The CPU complexes' ACC blocks (64-bit registers)
 * sit 0x1000000 apart for ECPU, PCPU0 and PCPU1; DVFS_CMD is the register
 * apple-soc-cpufreq drives.
 */
#define T6000_ACC_BASE			0x210e20000ULL
#define T6000_ACC_STRIDE		0x1000000ULL
#define T6000_ACCS			3
#define T6000_ACC_SIZE			0x29000
#define ACC_DVFS_CMD			0x00020
#define ACC_DVFS_APSC_OFF		BIT_ULL(22)
#define ACC_DVFS_APSC_BUSY		BIT_ULL(7)
#define ACC_LLC_THRTL			0x20240
#define ACC_AMX_THRTL			0x20250
#define ACC_THRTL_EN			BIT_ULL(63)
#define ACC_PERF_CTL			0x00660
#define ACC_PERF_EN			0x15ULL
#define ACC_ENERGY_CTL			0x28080
#define ACC_ENERGY_EN			(BIT_ULL(63) | BIT_ULL(61))
#define ACC_PPT_THRTL			0x28400

enum { ACCR_PERF, ACCR_ENERGY, ACCR_AMX, ACCR_LLC, ACCR_PPT, ACCR_DVFS, ACCR_N };

/* ClkGenSOC muxes, switched to the reference clock in clkgen_mux_off order */
#define T6000_CLKGEN_MUX		0x28e03815cULL
#define T6000_CLKGEN_SIZE		0x18
#define T6000_CLKGEN_MUXES		5
#define CLKGEN_MUX_ON			BIT(31)
#define CLKGEN_MUX_BUSY			BIT(30)
#define CLKGEN_MUX_SEL			GENMASK(27, 24)

/* PMP RTKit status as macOS SetDeviceState reads it, 1 while running */
#define T6000_PMP_STATUS		0x28e3c0010ULL
/* PLL 0 in the PLL block (regmap 0x37), off mode 3 as setPLLOffMode() leaves it */
#define T6000_PLL0			0x28e224000ULL
#define PLL_ON				BIT(0)
#define PLL_OFF_OK			BIT(1)
#define PLL_OFF_MODE			GENMASK(5, 3)

/* Master mini-PMGR, startS2RTimer(): writing 1 is macOS's last PMGR act before the CPUs go down */
#define T6000_S2R_TIMER			0x2925c4000ULL
/* NUB SPMI controller vector enables (word n at +4n), the clear words sit 0x40 above */
#define T6000_NUB_SPMI_IRQ_EN		0x2920a1320ULL
/* Vector 2 (word 0), on before GO as macOS has it, so the PMU RTC alarm can wake the SoC */
#define S2R_SPMI_VEC			BIT(2)
#define T6000_NUB_PS			0x292280000ULL
#define PLL_BUSY			BIT(31)

#define SOCQ_COUNTERS			BIT(0)
#define SOCQ_THROTTLERS			BIT(1)
#define SOCQ_APSC			BIT(2)
#define SOCQ_CLKGEN			BIT(3)
#define SOCQ_PLL			BIT(4)
#define SOCQ_DCS			BIT(5)
#define SOCQ_AFR			BIT(6)
#define SOCQ_ZEROS			BIT(7)
#define SOCQ_APCIE			BIT(8)
#define SOCQ_RELOCK			BIT(9)
#define SOCQ_MCC			BIT(10)

/*
 * Memory controller sleep tail, macOS 13.7.8 (hv trace 1/11 20:25/20:34): after the
 * PMP goes AP-idle and before the error handler zeros, XNU drains each AMCC
 * (+0x1c00 = 0 under the +0x10000c lock, then polls +0x1c04 to 0) and takes the
 * SLC down one way at a time (W.8 +0x158000 with one more bit clear, each followed
 * by W.4 +0x152000 = 0x11), shrinking the partition way masks in between. Linux
 * only drains: it leaves the SLC on. The 64-bit registers must be accessed 64
 * bits wide: a 32-bit peek of +0x158000 reset the machine.
 */
#define T6000_AMCCS			4
#define T6000_AMCC_BASE			0x200000000ULL
#define T6000_AMCC_STRIDE		0x1000000ULL
#define T6000_AMCC_MAP			0x160000
#define AMCC_DRAIN_CTL			0x1c00
#define AMCC_DRAIN_CNT			0x1c04
#define AMCC_LOCK			0x10000c
#define MCC_DRAIN			BIT(0)
#define MCC_DCS_LP			BIT(2)
/*
 * XNU at boot reads DCS0 +0x10 of each AMCC (iBoot leaves 0x70a0100) and writes
 * it back with bit 12 set to all four DCS; it stays set through sleep. Linux
 * never sets it (hv trace 1/11 20:34).
 */
#define T6000_DCS_PER_AMCC		4
#define DCS_OFF(k)			(0x200000ULL * ((k) + 1))
#define DCS_CTL				0x10
#define DCS_CTL_IBOOT			0x70a0100
#define DCS_CTL_MACOS			0x70a1100

/* macOS quiesceHW 24b: a handshake on the apcie block, 0x8014 = 1 then 0x82bc = 1, 2, 4 */
#define T6000_APCIE_Q_8014		0x58c008014ULL
#define T6000_APCIE_Q_82BC		0x58c0082bcULL
/* 24c/24f: ACC0/1 DVFS 0x100101 (pll relock), then APSC 0x40000500101 twice on all three */
#define ACC_DVFS_RELOCK			0x100101ULL

/* PMP telemetry (PTD), regmap 8: reads at +0x10 * entry, writes at +0x10000 + 8 * entry */
#define T6000_PTD_BASE			0x28e3c0000ULL
#define T6000_PTD_SIZE			0x19000
#define PTD_PS_REQ_RD			0xf80
#define PTD_PS_ACK_RD			0x1000
#define PTD_PS_REQ_WR			0x107c0
/* macOS sleep tail: 0x10840, 0x10848, 0x10850, 0x10860, never 0x10858 (hv trace 1/11) */
#define PTD_DVFS_WR(n)			((n) == 3 ? 0x10860 : 0x10840 + 8 * (n))
#define PTD_SLEEP			0x18328
#define PTD_DVFS_IDLE			0x2000000000000000ULL
/* Read side of the same words: +0x10 per entry, entries 0x108-0x10a and 0x10c */
#define PTD_DVFS_RD(n)			((n) == 3 ? 0x10c0 : 0x1080 + 0x10 * (n))
#define ACC_DVFS_APSC_MACOS		0x40000500101ULL

/*
 * macOS quiesceHW head, in its order: error-handler enables (22), the perf
 * counter zero burst (23), then the SoC TVMs (24e). All written 0.
 */
static const u64 socq_zero_pa[] = {
	0x282000a00, 0x304000a00, 0x383000a00, 0x402000a00,
	0x28e0ad000, 0x28e0f8000, 0x28e0f9000, 0x28e0fa000, 0x28e0fb000, 0x28e0fc000,
	0x28e0fd000, 0x28e070000, 0x28e5d8000,
	0x28e5e0000, 0x28e5e4000, 0x28e0c8200, 0x28e0c8400, 0x28e0c8600, 0x28e0c8000,
};

#define SOCQ_ZERO_N			ARRAY_SIZE(socq_zero_pa)

/* _setDCSPerfState(): MPMGR ClkGenSOC 0x800, [3:0] desired, bit31 busy, see notes/re12/dcs.md */
#define T6000_DCS_PS			0x28e20c800ULL
#define DCS_PS_TARGET			GENMASK(3, 0)
#define DCS_PS_BUSY			BIT(31)
/* The T6000 tail writes AFR PS 0 and waits for [7:4] before the clusters */
#define T6000_AFR_PS			0x28e0801e8ULL
#define T6000_SEP_PS			0x28e580c00ULL
/*
 * Before slpa, how long to wait for the SEP PS actual to reach 0, as macOS
 * does at quiesce priority 89999 after the enclave's SLEEP. The suspend is
 * aborted if it does not.
 */
#define S2R_SEP_WAIT_MS			5000
#define S2R_PS_DEV_DISABLE		BIT(10)
#define T6000_PMP_PS			0x28e0802d8ULL

/* A cluster's word at 0x28e098000 is byte 0 of its ADT soc-clusters entry */
#define CLUSTER_EACC			0
#define CLUSTER_PACC0			1
#define CLUSTER_PACC1			2
#define CLUSTER_ANE0			3
#define CLUSTER_AFR			5
#define CLUSTER_DISPLAY			6
#define CLUSTER_AVEMSR			8
#define CLUSTERS_CPU			(BIT(CLUSTER_EACC) | BIT(CLUSTER_PACC0) | \
					 BIT(CLUSTER_PACC1))
#define CLUSTERS_VALID			(CLUSTERS_CPU | BIT(CLUSTER_ANE0) | BIT(CLUSTER_AFR) | \
					 BIT(CLUSTER_DISPLAY) | BIT(CLUSTER_AVEMSR))

#define S2R_SMC_SLPW			__SMC_KEY('s', 'l', 'p', 'w')
#define S2R_SMC_SLPA			__SMC_KEY('s', 'l', 'p', 'a')
#define S2R_SMC_WAKA			__SMC_KEY('w', 'a', 'k', 'a')
/* Written by macOS between slpw and slpa, see notes/re12/hv-trace.md */
#define S2R_SMC_PMVC			__SMC_KEY('p', 'm', 'V', 'C')
#define S2R_SMC_WKTP			__SMC_KEY('W', 'K', 'T', 'P')
/*
 * SMC GPIO pins driven low before slpa and high again on wake: gP1a, the SD
 * reader's power enable, which macOS writes 0 before sleep
 */
#define S2R_SMC_GPIO_OFF		BIT(26)

/* Mailbox, see m1n1 src/s2r.h */
#define S2R_MAGIC		0x31584f424d523253ULL
#define S2R_VERSION		1
#define S2R_MAX_CPUS		32

struct s2r_cpu {
	u64 mpidr;
	u64 impl_reg;
	u64 start_reg0;
	u64 start_reg1;
	u32 start_val0;
	u32 start_val1;
	u64 entry;
	u64 resets;
	u64 stage;
};

struct s2r_mailbox {
	u64 magic;
	u32 version;
	u32 ncpus;
	u64 rvbar;
	u64 quiet;
	u64 boot_mpidr;
	u64 resumes;
	u64 restore;	/* S2R_RESTORE_* for m1n1 to set up on the next wake */
	u64 restored;	/* m1n1: S2R_RESTORED_DONE and the result */
	struct s2r_cpu cpu[S2R_MAX_CPUS];
};

/* m1n1 sets the internal PCIe controller up again before Linux resumes */
#define S2R_RESTORE_APCIE	BIT_ULL(0)
#define S2R_RESTORED_DONE	BIT_ULL(63)

static u64 s2r_restored;

enum {
	S2R_CPU_ONLINE,
	S2R_CPU_PARKED,
	S2R_CPU_OFF,
	/* cpu_kill() gave up on it, so its state is unknown until reboot */
	S2R_CPU_STUCK,
};

enum {
	S2R_RELEASE_NONE,
	S2R_RELEASE_SLEEP,
	S2R_RELEASE_OFF,
};

struct apple_s2r_cpu {
	struct s2r_cpu *slot;
	void __iomem *impl;
	u64 siq_cfg;
	u64 resets_at_die;
	void __iomem *pmgr;
	void __iomem *start1;
	int state;
	u64 acc_ovrd;
};

static struct {
	struct s2r_mailbox *mbox;
	u64 boot_mpidr;
	struct apple_smc *smc;
	struct platform_device *smc_pdev;
	/* slpw went out and still needs a waka */
	bool smc_announced;
	/* slpa may have reached the SMC during this suspend */
	bool slpa_sent;
	/* The SMC RTKit may think the AP is idle and needs ON */
	bool smc_ap_idle_sent;
	bool sleeping;
	int release;
	/* Cores went down with the deep sleep overrides in this round */
	bool deep_round;

	/* What the SMC sent while asleep, for the re-sleep log */
	struct notifier_block smc_nb;
	bool smc_nb_registered;
	unsigned int smc_events;
	u32 smc_last_event;

	/* Wakes that went back to sleep in this suspend */
	unsigned int again;

	void __iomem *pmgr_wake;
	void __iomem *wdt;
	bool pmgr_written, pmgr_woke;
	u32 pmgr_wanted[PMGR_AP_WAKE_WORDS];
	u32 pmgr_saved[PMGR_AP_WAKE_WORDS];
	u32 pmgr_readback[PMGR_AP_WAKE_WORDS];
	u32 pmgr_restored[PMGR_AP_WAKE_WORDS];
	u32 pmgr_wake_mask[PMGR_AP_WAKE_WORDS];
	u32 pmgr_wake_status[PMGR_AP_WAKE_WORDS];

	void __iomem *sep_pg;
	/* The SEP has a driver, which may need it across sleep */
	bool sep_bound;
	bool sep_pg_written;
	u32 sep_pg_saved, sep_pg_readback, sep_pg_wake, sep_pg_restored;

	void __iomem *cluster_ps;
	void __iomem *pmgr_ps, *east_ps;
	/* This round quiesces the PMGR as macOS does, and how it went */
	bool quiesce;
	u32 cluster_written, cluster_skipped, cluster_stuck;
	u32 cluster_saved[T6000_PMGR_CLUSTERS], cluster_readback[T6000_PMGR_CLUSTERS];
	u32 cluster_wake[T6000_PMGR_CLUSTERS], cluster_restored[T6000_PMGR_CLUSTERS];

	/* The quiesce steps: what was there, and what this round changed */
	void __iomem *acc[T6000_ACCS];
	void __iomem *clkgen, *pll, *s2r_timer;
	void __iomem *spmi_irq;
	bool stops_written;
	u32 disp_stuck, disp_read[6];
	void __iomem *disp_hs[3];
	void __iomem *gfx_blk, *afr_gate;
	u32 gfx_stuck, gfx_seen[4];
	u32 socq_done, socq_stuck, mux_n;
	u64 acc_saved[T6000_ACCS][ACCR_N], acc_wrote[T6000_ACCS][ACCR_N];
	u64 acc_restored[T6000_ACCS][ACCR_N];
	u32 mux_saved[T6000_CLKGEN_MUXES], mux_wrote[T6000_CLKGEN_MUXES];
	u32 mux_wake[T6000_CLKGEN_MUXES], mux_restored[T6000_CLKGEN_MUXES];
	u32 pll_saved, pll_wrote, pll_wake, pll_restored;
	void __iomem *dcs, *afr_ps, *pmp_ps, *sep_ps;
	u32 socq_skipped, pmp_ps_seen;
	u32 dcs_saved, dcs_mid, dcs_wrote, dcs_wake, dcs_restored;
	u32 afr_saved, afr_wrote, afr_wake, afr_restored;
	void __iomem *zero[SOCQ_ZERO_N];
	void __iomem *apcie_8014, *apcie_82bc;
	void __iomem *ptd;
	u64 ptd_req_saved, ptd_ack_seen;
	bool ptd_written;
	bool ptd_noticed;
	u32 ptd_sleep_saved;
	int ptd_ack_ret;
	u32 apcie_saved[2];
	void __iomem *amcc[T6000_AMCCS], *dcs_blk[T6000_AMCCS][T6000_DCS_PER_AMCC];
	u32 mcc_done, mcc_drain_cnt[T6000_AMCCS];
	u64 relock_saved[2], relock_wrote[2];
	u32 zero_saved[SOCQ_ZERO_N], zero_wake[SOCQ_ZERO_N];
	bool pmp_idled;

	/* Results of the last attempt */
	unsigned int attempts_at_prepare;
	u32 smc_wake_reason;
	u64 counter_before, counter_after;
	u64 resumes_before, resumes_after;
	int last_ret;
	unsigned int attempts;
	u64 wake_isr, wake_ipi_sr, wake_pmcr0;
} s2r;

static struct apple_s2r_cpu s2r_cpus[NR_CPUS];

/*
 * Set by apple_s2r.enable. From then on every core writes back its L1 before it
 * powers down: the parked cores and plain CPU hotplug share that path.
 */
static bool s2r_enabled;

/*
 * Wakes that are no wakeup event go back to sleep, every dark wake counts:
 * this is only a guard against a runaway, after it the machine wakes up
 */
#define S2R_AGAIN_MAX 1000U

/*
 * Before a display block goes down, the fabric block setup macOS writes when it
 * powers it up: Linux leaves 0x28a/0x28d/0x38c000000 at reset values.
 * hv trace 1/11, macOS 13.7.8 boot: dispext0 0x28a000000 (dispext1 0x28d000000
 * identical), disp0 0x38c000000
 */
static const u32 disp_init_28a[][2] = {
	{ 0x00000, 0x11 }, { 0x00010, 0x6d0000 }, { 0x00014, 0x9 }, { 0x00018, 0x9 },
	{ 0x0001c, 0x1b }, { 0x00020, 0x1b }, { 0x00024, 0x1 }, { 0x00028, 0x1 },
	{ 0x0002c, 0x1b }, { 0x00030, 0x1b }, { 0x00034, 0x1b }, { 0x00400, 0x40a10001 },
	{ 0x00600, 0xaab }, { 0x00700, 0x1ffffff }, { 0x008a8, 0x2000005 }, { 0x008ac, 0x14d004e },
	{ 0x008b0, 0x14d004e }, { 0x008b4, 0x14d004e }, { 0x008b8, 0x14d004e },
	{ 0x008bc, 0x14d004e }, { 0x008c0, 0x14d004e }, { 0x008c4, 0x14d004e },
	{ 0x008c8, 0x14d004e }, { 0x00410, 0x1100 },
	{ 0x00420, 0x1100 }, { 0x00430, 0x1100 }, { 0x08000, 0x9 }, { 0x00a20, 0x2 },
	{ 0x08008, 0x7 }, { 0x08014, 0x1 }, { 0x08018, 0x1 }, { 0x00860, 0x1 },
	{ 0x08208, 0x4 }, { 0x08280, 0x20 }, { 0x08288, 0x3 }, { 0x0828c, 0xc },
	{ 0x08290, 0x18 }, { 0x08294, 0x30 }, { 0x08298, 0x78 }, { 0x0829c, 0xf0 },
	{ 0x082b8, 0x1 }, { 0x082bc, 0x1 }, { 0x082c0, 0x1 }, { 0x00800, 0x1 },
	{ 0x0820c, 0x3 }, { 0x08284, 0x20 }, { 0x082a0, 0x3 }, { 0x082a4, 0xc },
	{ 0x082a8, 0x18 }, { 0x082ac, 0x30 }, { 0x082b0, 0x78 }, { 0x082b4, 0xf0 },
	{ 0x082b8, 0x3 }, { 0x082bc, 0x2 }, { 0x082c0, 0x3 }, { 0x08210, 0x0 },
	{ 0x08408, 0x3 }, { 0x08418, 0x3 }, { 0x0841c, 0x0 }, { 0x08420, 0xffffffff },
	{ 0x08424, 0x0 }, { 0x08428, 0xfff }, { 0x082b8, 0x7 }, { 0x082bc, 0x4 },
	{ 0x082c0, 0x7 },
};

static const u32 disp_init_38c[][2] = {
	{ 0x00000, 0x11 }, { 0x00010, 0x610000 }, { 0x00014, 0x8 }, { 0x00018, 0x8 },
	{ 0x0001c, 0x18 }, { 0x00020, 0x18 }, { 0x00024, 0x1 }, { 0x00028, 0x1 },
	{ 0x0002c, 0x18 }, { 0x00030, 0x18 }, { 0x00034, 0x18 }, { 0x00400, 0x40a10001 },
	{ 0x00600, 0xaab }, { 0x00700, 0x1ffffff }, { 0x008a8, 0x2000005 }, { 0x008ac, 0x14d004e },
	{ 0x008b0, 0x14d004e }, { 0x008b4, 0x14d004e }, { 0x008b8, 0x14d004e },
	{ 0x008bc, 0x14d004e }, { 0x008c0, 0x14d004e }, { 0x008c4, 0x14d004e },
	{ 0x008c8, 0x14d004e }, { 0x00410, 0x1100 },
	{ 0x00420, 0x1100 }, { 0x00430, 0x1100 }, { 0x08000, 0x9 }, { 0x00a20, 0x2 },
	{ 0x08008, 0x7 }, { 0x08014, 0x1 }, { 0x08018, 0x1 }, { 0x00920, 0x3fff0001 },
	{ 0x08208, 0x8 }, { 0x08280, 0x20 }, { 0x08288, 0x3 }, { 0x0828c, 0xc },
	{ 0x08290, 0x18 }, { 0x08294, 0x30 }, { 0x08298, 0x78 }, { 0x0829c, 0xf0 },
	{ 0x082b8, 0x1 }, { 0x082bc, 0x1 }, { 0x082c0, 0x1 }, { 0x00860, 0x1 },
	{ 0x0820c, 0x5 }, { 0x08284, 0x20 }, { 0x082a0, 0x3 }, { 0x082a4, 0xc },
	{ 0x082a8, 0x18 }, { 0x082ac, 0x30 }, { 0x082b0, 0x78 }, { 0x082b4, 0xf0 },
	{ 0x082b8, 0x3 }, { 0x082bc, 0x2 }, { 0x082c0, 0x3 }, { 0x08210, 0x0 },
	{ 0x08408, 0x3 }, { 0x08418, 0x3 }, { 0x0841c, 0x0 }, { 0x08420, 0xffffffff },
	{ 0x08424, 0x0 }, { 0x08428, 0xfff }, { 0x082b8, 0x7 }, { 0x082bc, 0x4 },
	{ 0x082c0, 0x7 },
};

/* hv trace 1/11 17:43, macOS 13.7.8 sleep entry, 0x405000000 (pmgr reg 55) after GFX PS on */
static const u32 gfx_init[][2] = {
	{ 0x714, 0x2000000 },
	{ 0x718, 0x400080 },
	{ 0x71c, 0x400080 },
	{ 0x720, 0x400080 },
	{ 0x724, 0x400080 },
	{ 0x728, 0x400080 },
	{ 0x72c, 0x400080 },
	{ 0x730, 0x400080 },
	{ 0x734, 0x400080 },
	{ 0x00, 0x11 },
	{ 0x10, 0xd0000 },
	{ 0x14, 0x1 },
	{ 0x18, 0x1 },
	{ 0x1c, 0x3 },
	{ 0x20, 0x3 },
	{ 0x24, 0x3 },
	{ 0x28, 0x3 },
	{ 0x2c, 0x3 },
	{ 0x400, 0x40a10001 },
	{ 0x600, 0x1ffffff },
	{ 0x800, 0x101 },
	{ 0x410, 0x1100 },
	{ 0x420, 0x1100 },
	{ 0x430, 0x1100 },
	{ 0x8000, 0x9 },
	{ 0x820, 0x80 },
	{ 0x8008, 0x7 },
	{ 0x8014, 0x1 },
	{ 0x8018, 0x1 },
	{ 0x748, 0x1 },
	{ 0x8208, 0x2 },
	{ 0x8280, 0x20 },
	{ 0x8288, 0x3 },
	{ 0x828c, 0xc },
	{ 0x8290, 0x18 },
	{ 0x8294, 0x30 },
	{ 0x8298, 0x78 },
	{ 0x829c, 0xf0 },
	{ 0x82b8, 0x7 },
	{ 0x82bc, 0x1 },
	{ 0x82c0, 0x7 },
	{ 0x7a8, 0x1 },
	{ 0x820c, 0x5 },
	{ 0x8284, 0x20 },
	{ 0x82a0, 0x3 },
	{ 0x82a4, 0xc },
	{ 0x82a8, 0x18 },
	{ 0x82ac, 0x30 },
	{ 0x82b0, 0x78 },
	{ 0x82b4, 0xf0 },
	{ 0x82b8, 0x7 },
	{ 0x82bc, 0x2 },
	{ 0x82c0, 0x7 },
	{ 0x8210, 0x0 },
	{ 0x8408, 0xd },
	{ 0x8418, 0x3 },
	{ 0x841c, 0x0 },
	{ 0x8420, 0xffffffff },
	{ 0x8424, 0x0 },
	{ 0x8428, 0xfff },
	{ 0x82b8, 0x7 },
	{ 0x82bc, 0x4 },
	{ 0x82c0, 0x7 },
};

/* macOS's AFR PS writes after GFX goes down, in order */
static const u32 gfx_afr_down[] = {
	0x404004f, 0xf0403ff, 0x1f0400ff, 0x140f024f, 0xf0f02ff, 0x4040340,
};

/* Set by the apple-pmp module, which may come and go */
static DEFINE_MUTEX(s2r_pmp_lock);
static int (*s2r_pmp_sleep)(int op);
static void __iomem *s2r_pmp_status;

void apple_s2r_set_pmp_hook(int (*sleep)(int op))
{
	guard(mutex)(&s2r_pmp_lock);
	s2r_pmp_sleep = sleep;
}
EXPORT_SYMBOL_GPL(apple_s2r_set_pmp_hook);

static u32 apple_s2r_pmp_status(void)
{
	return s2r_pmp_status ? readl(s2r_pmp_status) : 0xdeadbeef;
}

static int apple_s2r_pmp_op(int op)
{
	u32 before;
	int ret;

	guard(mutex)(&s2r_pmp_lock);
	if (!s2r_pmp_sleep) {
		pr_info("no PMP driver for op %d\n", op);
		return -ENODEV;
	}
	if (!s2r_pmp_status)
		s2r_pmp_status = ioremap_np(T6000_PMP_STATUS, 4);
	/* The status sits in PMS_SRAM, which is off from a stop to a good start */
	if (op == APPLE_S2R_PMP_STOP) {
		before = apple_s2r_pmp_status();
		ret = s2r_pmp_sleep(op);
		pr_debug("PMP stop: %d, status before %#x\n", ret, before);
	} else {
		ret = s2r_pmp_sleep(op);
		if (ret) {
			pr_debug("PMP start: %d\n", ret);
		} else {
			u32 status = apple_s2r_pmp_status();

			pr_debug("PMP start: 0, status %#x\n", status);
		}
	}
	return ret;
}

#ifdef CONFIG_PM_SLEEP_DEBUG
/* Private to kernel/power, but this driver is always built in */
extern int pm_test_level;
#endif

static bool apple_s2r_pm_test(void)
{
#ifdef CONFIG_PM_SLEEP_DEBUG
	/* TEST_NONE, which kernel/power/power.h keeps first */
	return READ_ONCE(pm_test_level) != 0;
#else
	return false;
#endif
}

struct apple_s2r_bit {
	unsigned int bit;
	const char *name;
};

/* ADT /arm-io/pmgr ap-wake-sources on t6000 */
static const struct apple_s2r_bit t6000_ap_wake_sources[] = {
	{ 12, "AOP.GPIO.IRQ4" },
	{ 14, "AOP.GPIO.IRQ6" },
	{ 26, "AOP.OutboxNotEmpty" },
	{ 27, "AOP.CPUWakeupAP" },
	{ 40, "NUB.WakeupAP" },
	{ 43, "NUB.GPIO.IRQ2" },
	{ 49, "NUB.APWatchdog" },
	{ 50, "NUB.APWakeupTime" },
	{ 51, "NUB.SPMISw0IRQ" },
	{ 52, "NUB.SPMISw1IRQ" },
	{ 54, "NUB.SPMISw3IRQ" },
	{ 85, "SMC.CPUWakeupAP" },
	{ 90, "SMC.OutboxNotEmpty" },
	{ 126, "ATC0.CIOWakeup" },
	{ 127, "ATC1.CIOWakeup" },
	{ 128, "ATC2.CIOWakeup" },
	{ 129, "ATC3.CIOWakeup" },
	{ 130, "ATC0.USBWakeup" },
	{ 131, "ATC1.USBWakeup" },
	{ 132, "ATC2.USBWakeup" },
	{ 133, "ATC3.USBWakeup" },
};

/* ADT smc-pmu event_name-bit<N>, what MBSW says happened while asleep */
static const struct apple_s2r_bit apple_s2r_mbsw_bits[] = {
	{ 0, "pwrbtn" },
	{ 8, "lid" },
	{ 9, "charger" },
	{ 10, "codec" },
	{ 11, "wifibt" },
	{ 12, "trackpadkeyboard" },
	{ 14, "sdconn" },
	{ 40, "acattach" },
	{ 41, "acdetach" },
	{ 42, "USB-C_plug" },
	{ 43, "USB2_plug" },
	{ 44, "USB2_wake" },
	{ 45, "aupo" },
	{ 48, "rtc" },
};

/* The set bits of words, by name where one is known */
static void apple_s2r_bits(char *buf, size_t size, const u32 *words, unsigned int nwords,
			   const struct apple_s2r_bit *names, unsigned int nnames)
{
	unsigned int bit, i;
	int len = 0;

	for (bit = 0; bit < nwords * 32; bit++) {
		if (!(words[bit / 32] & BIT(bit % 32)))
			continue;
		for (i = 0; i < nnames && names[i].bit != bit; i++)
			;
		if (i < nnames)
			len += scnprintf(buf + len, size - len, " %s", names[i].name);
		else
			len += scnprintf(buf + len, size - len, " %u", bit);
	}
	if (!len)
		scnprintf(buf, size, " none");
}

static u64 apple_s2r_mpidr(unsigned int cpu)
{
	return cpu_logical_map(cpu) & MPIDR_HWID_BITMASK & 0xffffff;
}

static void apple_s2r_mbox_clean(void)
{
	dcache_clean_inval_poc((unsigned long)s2r.mbox,
			       (unsigned long)s2r.mbox + sizeof(*s2r.mbox));
}

static void apple_s2r_mbox_inval(void)
{
	dcache_inval_poc((unsigned long)s2r.mbox,
			 (unsigned long)s2r.mbox + sizeof(*s2r.mbox));
}

static u64 apple_s2r_cyc_ovrd(bool wake_on_irq)
{
	u64 cyc = read_sysreg_s(SYS_IMP_APL_CYC_OVRD);

	cyc &= ~(CYC_OVRD_WFI_MODE | CYC_OVRD_IRQ_MODE | CYC_OVRD_FIQ_MODE);
	cyc |= FIELD_PREP(CYC_OVRD_WFI_MODE, 3) | CYC_OVRD_DISABLE_WFI_RET;
	if (!wake_on_irq)
		cyc |= FIELD_PREP(CYC_OVRD_IRQ_MODE, 2) | FIELD_PREP(CYC_OVRD_FIQ_MODE, 2);

	return cyc;
}

static u64 apple_s2r_acc_ovrd_deep(void)
{
	u64 acc = read_sysreg_s(SYS_IMP_APL_ACC_OVRD);

	acc &= ~(ACC_OVRD_PWR_DN_SRM | ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP |
		 ACC_OVRD_TRAIN_DOWN_LINK | ACC_OVRD_POWER_DOWN_CPM);
	acc |= FIELD_PREP(ACC_OVRD_PWR_DN_SRM, 3) |
	       FIELD_PREP(ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP, 2) |
	       FIELD_PREP(ACC_OVRD_TRAIN_DOWN_LINK, 3) |
	       FIELD_PREP(ACC_OVRD_POWER_DOWN_CPM, 3) |
	       ACC_OVRD_DISABLE_PIO_ON_WFI_CPU | ACC_OVRD_DEEP_SLEEP;

	return acc;
}

static void __iomem *apple_s2r_ps(u64 pa)
{
	if (pa >= T6000_PMGR_PS_BASE && pa < T6000_PMGR_PS_BASE + T6000_PMGR_PS_SIZE)
		return s2r.pmgr_ps ? s2r.pmgr_ps + (pa - T6000_PMGR_PS_BASE) : NULL;
	if (pa >= T6000_PMGR_EAST_PS_BASE && pa < T6000_PMGR_EAST_PS_BASE + T6000_PMGR_PS_SIZE)
		return s2r.east_ps ? s2r.east_ps + (pa - T6000_PMGR_EAST_PS_BASE) : NULL;

	return NULL;
}

/* The apcie block sits behind PCIE0_REF, APCIE_GP and APCIE_GP_SYS (ADT power-gates 17, 20, 78) */
static const u64 apcie_q_ps[] = { 0x28e080138ULL, 0x28e080150ULL, 0x28e080320ULL };

/*
 * Kept out of struct s2r on purpose: adding them there shifted the fields
 * the parked CPUs touch on their way down, and S2R entry stopped working.
 */
static bool apcie_skipped;
static u32 apcie_ps_wake[3];

/* A write to it with any of them off never completes and takes the CPU with it */
static bool apple_s2r_apcie_on(u32 *seen)
{
	bool on = true;
	int i;

	for (i = 0; i < ARRAY_SIZE(apcie_q_ps); i++) {
		void __iomem *ps = apple_s2r_ps(apcie_q_ps[i]);

		seen[i] = ps ? readl(ps) : 0;
		if (!ps || FIELD_GET(PS_ACTUAL, seen[i]) != 0xf)
			on = false;
	}

	return on;
}

/*
 * CPU hotplug
 */

static bool apple_s2r_cpu_can_disable(unsigned int cpu)
{
	/* m1n1 cold boots when its boot CPU resets without a resume entry */
	return s2r_cpus[cpu].slot && apple_s2r_mpidr(cpu) != s2r.boot_mpidr;
}

static void apple_s2r_cpu_die(unsigned int cpu)
{
	struct apple_s2r_cpu *c = &s2r_cpus[cpu];

	/* cpu_kill() gave up on this core: go down, but stay out of use */
	if (READ_ONCE(c->state) == S2R_CPU_STUCK)
		apple_s2r_cpu_off(apple_s2r_cyc_ovrd(true), 0, READ_ONCE(s2r_enabled));

	if (READ_ONCE(s2r.sleeping)) {
		int release;

		/* Fails only when cpu_kill() just gave up on this core */
		if (cmpxchg(&c->state, S2R_CPU_ONLINE, S2R_CPU_PARKED) != S2R_CPU_ONLINE)
			apple_s2r_cpu_off(apple_s2r_cyc_ovrd(true), 0, READ_ONCE(s2r_enabled));
		/* Pairs with the finisher's release, which comes after stops_written */
		while (!(release = smp_load_acquire(&s2r.release)))
			wfe();

		if (release == S2R_RELEASE_SLEEP) {
			/*
			 * PE_cpu_machine_quiesce: the PMGR has to know the core is
			 * down for S2R. The finisher has written its stop already.
			 */
			if (!READ_ONCE(s2r.stops_written))
				writel(c->slot->start_val0, c->pmgr + PMGR_CPU_STOP);
			apple_s2r_cpu_off(apple_s2r_cyc_ovrd(true), apple_s2r_acc_ovrd_deep(),
					  READ_ONCE(s2r_enabled));
		}
	}

	/*
	 * Power down without retention. A fast IPI wakes the core through a
	 * reset into m1n1, which hands it back to the holding pen; the PMGR
	 * start bits do not wake a core that powered itself down. Requesting
	 * a PMGR stop first, as m1n1 does, keeps the core from powering down.
	 */
	apple_s2r_mbox_inval();
	c->resets_at_die = READ_ONCE(c->slot->resets);
	/* cpu_boot() reads resets_at_die once it has seen S2R_CPU_OFF */
	smp_store_release(&c->state, S2R_CPU_OFF);
	apple_s2r_cpu_off(apple_s2r_cyc_ovrd(true), 0, READ_ONCE(s2r_enabled));
}

static int apple_s2r_wait_off(unsigned int cpu)
{
	u64 val;
	int ret;

	ret = readq_poll_timeout_atomic(s2r_cpus[cpu].impl + CPU_IMPL_PWRSTATE, val,
					!(val & 0xff), 10, 100000);
	if (ret)
		pr_warn("CPU%u did not power down (0x%llx)\n", cpu, val);

	return ret;
}

static int apple_s2r_cpu_kill(unsigned int cpu)
{
	int i;

	/*
	 * Parked CPUs are still running and go down with the boot CPU. Wait
	 * until this one has parked, so cpu_boot() never sees it as online.
	 */
	if (READ_ONCE(s2r.sleeping)) {
		for (i = 0; i < 100000; i++) {
			if (READ_ONCE(s2r_cpus[cpu].state) != S2R_CPU_ONLINE)
				return 0;
			udelay(1);
		}
		/*
		 * Hotplug carries on as if it was down. Unless it parks right
		 * now, keep cpu_boot() from handing it work in whatever state
		 * it is in.
		 */
		if (cmpxchg(&s2r_cpus[cpu].state, S2R_CPU_ONLINE, S2R_CPU_STUCK) != S2R_CPU_ONLINE)
			return 0;
		pr_warn("CPU%u did not park, keeping it out of use\n", cpu);
		return -ETIMEDOUT;
	}

	/* cpu_boot() checks the power state again before it wakes the core */
	return apple_s2r_wait_off(cpu);
}

/*
 * Powering a core down with the deep sleep overrides can take the register
 * access of its cluster with it, and m1n1 never reads the power state back
 * after such a stop either. Wake it blind and wait for its reset in the
 * mailbox instead.
 */
static int apple_s2r_cpu_boot_blind(struct apple_s2r_cpu *c, unsigned int cpu)
{
	int i;

	WRITE_ONCE(c->state, S2R_CPU_ONLINE);
	/* A core released just before an aborted sleep may still be on its way down */
	mdelay(5);
	write_sysreg_s((c->slot->mpidr & 0xff) | ((c->slot->mpidr & 0xff00) << 8),
		       SYS_IMP_APL_IPI_RR_GLOBAL);
	isb();
	for (i = 0; i < 10000; i++) {
		apple_s2r_mbox_inval();
		if (READ_ONCE(c->slot->resets) != c->resets_at_die)
			return 0;
		udelay(10);
	}

	/* Not woken by the IPI: a core in reset after system sleep starts like at boot */
	pr_info("CPU%u: no reset after the wake IPI, starting it through the PMGR\n", cpu);
	writel_relaxed(c->slot->start_val0, c->pmgr + PMGR_CPU_START);
	writel_relaxed(c->slot->start_val1, c->start1);

	return 0;
}

static int apple_s2r_cpu_boot(unsigned int cpu)
{
	struct apple_s2r_cpu *c = &s2r_cpus[cpu];
	u64 val;

	switch (READ_ONCE(c->state)) {
	case S2R_CPU_ONLINE:
		/* First boot, the CPU is already in the spin table */
		return 0;
	case S2R_CPU_PARKED:
		if (!READ_ONCE(s2r.release)) {
			/* Suspend was abandoned or only tested: let the parked CPUs go down */
			smp_store_release(&s2r.release, S2R_RELEASE_OFF);
			dsb(sy);
			sev();
		}
		/* resets_at_die is stale until the core leaves the parking loop */
		if (read_poll_timeout_atomic(smp_load_acquire, val, val == S2R_CPU_OFF,
					     10, 100000, false, &c->state)) {
			pr_err("CPU%u: did not leave the parking loop\n", cpu);
			return -EBUSY;
		}
		break;
	case S2R_CPU_STUCK:
		pr_err("CPU%u did not park when it went down, not starting it\n", cpu);
		return -EBUSY;
	default:
		break;
	}

	/* Something woke it already: it went through m1n1 to the holding pen */
	apple_s2r_mbox_inval();
	if (READ_ONCE(c->slot->resets) != c->resets_at_die) {
		WRITE_ONCE(c->state, S2R_CPU_ONLINE);
		return 0;
	}

	if (READ_ONCE(s2r.deep_round))
		return apple_s2r_cpu_boot_blind(c, cpu);

	/* Only a powered down core resets on wakeup. Only the power state can be read now. */
	if (apple_s2r_wait_off(cpu))
		return -EBUSY;

	WRITE_ONCE(c->state, S2R_CPU_ONLINE);
	write_sysreg_s((c->slot->mpidr & 0xff) | ((c->slot->mpidr & 0xff00) << 8),
		       SYS_IMP_APL_IPI_RR_GLOBAL);
	isb();
	if (readq_poll_timeout_atomic(c->impl + CPU_IMPL_PWRSTATE, val, val & 0xff, 10, 10000)) {
		/* Not woken by the IPI: a core in reset after system sleep starts like at boot */
		writel_relaxed(c->slot->start_val0, c->pmgr + PMGR_CPU_START);
		writel_relaxed(c->slot->start_val1, c->start1);
	}

	return 0;
}

static const struct spin_table_hotplug_ops apple_s2r_hotplug_ops = {
	.cpu_can_disable = apple_s2r_cpu_can_disable,
	.cpu_die = apple_s2r_cpu_die,
	.cpu_kill = apple_s2r_cpu_kill,
	.cpu_boot = apple_s2r_cpu_boot,
};

/* Deep sleep overrides left behind when the SoC did not go down */
static int apple_s2r_cpu_online(unsigned int cpu)
{
	u64 acc = read_sysreg_s(SYS_IMP_APL_ACC_OVRD);

	if (acc & ACC_OVRD_DEEP_SLEEP) {
		pr_debug("CPU%u: clearing deep sleep overrides\n", cpu);
		write_sysreg_s(s2r_cpus[cpu].acc_ovrd, SYS_IMP_APL_ACC_OVRD);
		isb();
	}

	/* Back from an offline that was rolled back */
	if (FIELD_GET(SIQ_CFG_ROUTE, read_sysreg_s(SYS_IMP_APL_SIQ_CFG)) == SIQ_CFG_ROUTE_NONE &&
	    s2r_cpus[cpu].siq_cfg) {
		write_sysreg_s(s2r_cpus[cpu].siq_cfg, SYS_IMP_APL_SIQ_CFG);
		isb();
	}

	return 0;
}

/*
 * Stop the AIC from delivering IRQs to this CPU while it can still take them,
 * as XNU does for a core going offline. AICv2 IRQs cannot be steered, so one
 * delivered after the CPU masked them would stay pending there: the device
 * never sees it handled and the core keeps aborting its power down.
 */
static int apple_s2r_cpu_down_prep(unsigned int cpu)
{
	u64 siq = read_sysreg_s(SYS_IMP_APL_SIQ_CFG);

	s2r_cpus[cpu].siq_cfg = siq;
	write_sysreg_s(siq | FIELD_PREP(SIQ_CFG_ROUTE, SIQ_CFG_ROUTE_NONE), SYS_IMP_APL_SIQ_CFG);
	isb();

	return 0;
}

/*
 * Suspend
 */

static int apple_s2r_smc_write_key(smc_key key, const void *buf, size_t size)
{
	int (*write)(struct apple_smc *smc, smc_key key, const void *buf, size_t size);
	int ret;

	if (!s2r.smc)
		return -ENODEV;

	write = symbol_get(apple_smc_write);
	if (!write)
		return -ENODEV;

	ret = write(s2r.smc, key, buf, size);
	symbol_put(apple_smc_write);
	if (ret)
		pr_err("SMC %p4ch write failed: %d\n", &key, ret);

	return ret;
}

static int apple_s2r_smc_write(u32 val)
{
	int (*write)(struct apple_smc *smc, smc_key key, const void *buf, size_t size);
	__le32 buf = cpu_to_le32(val);
	int ret;

	if (!s2r.smc)
		return -ENODEV;

	/* pm_test keeps the AP awake, and an SMC that thinks it sleeps hung it for good */
	if (WARN_ON_ONCE(val == S2R_SMC_SLPA && apple_s2r_pm_test()))
		return -EPERM;

	write = symbol_get(apple_smc_write);
	if (!write)
		return -ENODEV;

	ret = write(s2r.smc, SMC_KEY(MBSE), &buf, sizeof(buf));
	symbol_put(apple_smc_write);
	if (ret)
		pr_err("MBSE = %p4ch failed: %d\n", &val, ret);

	return ret;
}

static int apple_s2r_smc_read(smc_key key, void *buf, size_t size)
{
	int (*read)(struct apple_smc *smc, smc_key key, void *buf, size_t size);
	int ret;

	if (!s2r.smc)
		return -ENODEV;

	read = symbol_get(apple_smc_read);
	if (!read)
		return -ENODEV;

	ret = read(s2r.smc, key, buf, size);
	symbol_put(apple_smc_read);
	if (ret < 0)
		return ret;

	/* It returns the number of bytes read */
	return ret == size ? 0 : -EIO;
}

/*
 * The battery charge counter (BAAC, mC) right before slpa and right after
 * the wake, logged as read. It does not track sleep time, so no power figure
 * is derived from it.
 */
static s64 s2r_baac0;
static u64 s2r_baac_cnt0;
static bool s2r_ac_online;

static bool apple_s2r_ac_read(void)
{
	u32 ac = 0;

	return !apple_s2r_smc_read(SMC_KEY(CHIS), &ac, sizeof(ac)) && ac;
}

static bool s2r_baac_ok;

static void apple_s2r_baac_mark(void)
{
	s64 q;

	s2r_ac_online = apple_s2r_ac_read();
	s2r_baac_ok = !apple_s2r_smc_read(SMC_KEY(BAAC), &q, sizeof(q));
	if (s2r_baac_ok) {
		s2r_baac0 = q;
		s2r_baac_cnt0 = __arch_counter_get_cntpct();
	}
}

static void apple_s2r_baac_report(void)
{
	u64 ms;
	s64 q;
	u16 mv = 0;

	if (!s2r_baac_ok)
		return;
	s2r_baac_ok = false;
	if (apple_s2r_smc_read(SMC_KEY(BAAC), &q, sizeof(q)))
		return;
	apple_s2r_smc_read(SMC_KEY(B0AV), &mv, sizeof(mv));
	ms = div_u64(__arch_counter_get_cntpct() - s2r_baac_cnt0, arch_timer_get_cntfrq() / 1000);
	pr_debug("BAAC %lld -> %lld mC over %llu ms, B0AV %u mV\n", s2r_baac0, q, ms, mv);
}

static int apple_s2r_smc_event(struct notifier_block *nb, unsigned long event, void *data)
{
	WRITE_ONCE(s2r.smc_events, s2r.smc_events + 1);
	WRITE_ONCE(s2r.smc_last_event, event);

	return NOTIFY_DONE;
}

static void apple_s2r_words(char *buf, size_t size, const u32 *w)
{
	scnprintf(buf, size, "%08x %08x %08x %08x %08x %08x %08x",
		  w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
}

static void apple_s2r_pmgr_map(void)
{
	int i;

	if (s2r.pmgr_wake || !of_machine_is_compatible("apple,t6000"))
		return;

	s2r.pmgr_wake = ioremap_np(T6000_PMGR_AP_WAKE_BASE, T6000_PMGR_AP_WAKE_SIZE);
	if (!s2r.pmgr_wake)
		pr_warn("cannot map the PMGR wake sources at 0x%llx\n", T6000_PMGR_AP_WAKE_BASE);
	s2r.wdt = ioremap_np(T6000_WDT_BASE, 0x30);
	s2r.sep_pg = ioremap_np(T6000_PMGR_SEP_PG, 4);
	s2r.cluster_ps = ioremap_np(T6000_PMGR_CLUSTER_PS, 4 * T6000_PMGR_CLUSTERS);
	s2r.pmgr_ps = ioremap_np(T6000_PMGR_PS_BASE, T6000_PMGR_PS_SIZE);
	s2r.east_ps = ioremap_np(T6000_PMGR_EAST_PS_BASE, T6000_PMGR_PS_SIZE);
	for (i = 0; i < T6000_ACCS; i++)
		s2r.acc[i] = ioremap_np(T6000_ACC_BASE + i * T6000_ACC_STRIDE, T6000_ACC_SIZE);
	s2r.clkgen = ioremap_np(T6000_CLKGEN_MUX, T6000_CLKGEN_SIZE);
	s2r.pll = ioremap_np(T6000_PLL0, 4);
	s2r.s2r_timer = ioremap_np(T6000_S2R_TIMER, 4);
	s2r.spmi_irq = ioremap_np(T6000_NUB_SPMI_IRQ_EN, 0x44);
	s2r.dcs = ioremap_np(T6000_DCS_PS, 4);
	s2r.afr_ps = ioremap_np(T6000_AFR_PS, 4);
	for (i = 0; i < SOCQ_ZERO_N; i++)
		s2r.zero[i] = ioremap_np(socq_zero_pa[i], 4);
	s2r.apcie_8014 = ioremap_np(T6000_APCIE_Q_8014, 4);
	s2r.disp_hs[0] = ioremap_np(0x28a000000UL, 0xc000);	/* dispext0 */
	s2r.disp_hs[1] = ioremap_np(0x38c000000UL, 0xc000);	/* disp0 */
	s2r.disp_hs[2] = ioremap_np(0x28d000000UL, 0xc000);	/* dispext1 */
	s2r.gfx_blk = ioremap_np(0x405000000UL, 0xc000);	/* GPU fabric gate */
	s2r.amcc[0] = ioremap_np(T6000_AMCC_BASE + 0 * T6000_AMCC_STRIDE, T6000_AMCC_MAP);
	s2r.amcc[1] = ioremap_np(T6000_AMCC_BASE + 1 * T6000_AMCC_STRIDE, T6000_AMCC_MAP);
	s2r.amcc[2] = ioremap_np(T6000_AMCC_BASE + 2 * T6000_AMCC_STRIDE, T6000_AMCC_MAP);
	s2r.amcc[3] = ioremap_np(T6000_AMCC_BASE + 3 * T6000_AMCC_STRIDE, T6000_AMCC_MAP);
	for (i = 0; i < T6000_AMCCS * T6000_DCS_PER_AMCC; i++)
		s2r.dcs_blk[i / T6000_DCS_PER_AMCC][i % T6000_DCS_PER_AMCC] =
			ioremap_np(T6000_AMCC_BASE + (i / T6000_DCS_PER_AMCC) * T6000_AMCC_STRIDE +
				   DCS_OFF(i % T6000_DCS_PER_AMCC), 0x1000);
	s2r.afr_gate = ioremap_np(0x28e5e4000UL, 4);		/* macOS 1 around GFX up */
	s2r.ptd = ioremap_np(T6000_PTD_BASE, T6000_PTD_SIZE);
	s2r.apcie_82bc = ioremap_np(T6000_APCIE_Q_82BC, 4);
	s2r.pmp_ps = ioremap_np(T6000_PMP_PS, 4);
	s2r.sep_ps = ioremap_np(T6000_SEP_PS, 4);
	if (!s2r_pmp_status)
		s2r_pmp_status = ioremap_np(T6000_PMP_STATUS, 4);
}

/*
 * The SEP driver puts the enclave to sleep for S2R and wakes it again after.
 * If that wake failed, it stays down until reboot: on a later sleep nothing
 * tells it to power gate, and a PMGR still waiting for that never sleeps. Then
 * wait only the first time.
 */
static bool s2r_sep_down;

/* Boot CPU alone with IRQs off, like the wake mask */
static void apple_s2r_sep_pg_clear(void)
{
	if (!s2r.sep_pg)
		return;
	if (s2r.sep_bound) {
		pr_debug("SEP has a driver, the PMGR keeps waiting for its power gate\n");
		return;
	}

	s2r.sep_pg_saved = readl(s2r.sep_pg);
	if (!(s2r.sep_pg_saved & PMGR_WAIT_SEP_PG))
		return;

	s2r.sep_pg_written = true;
	writel(s2r.sep_pg_saved & ~PMGR_WAIT_SEP_PG, s2r.sep_pg);
	s2r.sep_pg_readback = readl(s2r.sep_pg);
}

/* Whatever cpu_suspend() returned. After a real S2R the PMGR may come back reset */
static void apple_s2r_sep_pg_restore(void)
{
	if (!s2r.sep_pg_written)
		return;

	s2r.sep_pg_wake = readl(s2r.sep_pg);
	writel(s2r.sep_pg_saved, s2r.sep_pg);
	s2r.sep_pg_restored = readl(s2r.sep_pg);
}

static void apple_s2r_sep_pg_report(void)
{
	if (!s2r.sep_pg_written)
		return;

	pr_debug("SEP PG wait: %#x, wrote %#x, %#x after wake, restored %#x\n", s2r.sep_pg_saved,
		 s2r.sep_pg_readback, s2r.sep_pg_wake, s2r.sep_pg_restored);
	if (s2r.sep_pg_restored != s2r.sep_pg_saved)
		pr_err("SEP PG wait restore failed\n");
}

/* The blocks in a cluster, which have to be off before it goes down */
static bool apple_s2r_cluster_gate(unsigned int cluster)
{
	static const u64 ane0[] = { 0x28e080268, 0x28e0802c8, 0 };
	static const u64 afr[] = { 0x28e0801e8, 0x28e5803c0, 0x28e080268, 0x28e080270,
				   0x28e580390, 0x28e580398, 0x28e5803b0, 0 };
	/* disp0, dispext0 and dispext1 */
	static const u64 display[] = { 0x28e580350, 0x28e080280, 0x28e0802a8, 0 };
	static const u64 avemsr[] = { 0x28e080270, 0x28e580398, 0x28e5803b0, 0 };
	const u64 *ps;

	switch (cluster) {
	case CLUSTER_ANE0:
		ps = ane0;
		break;
	case CLUSTER_AFR:
		ps = afr;
		break;
	case CLUSTER_DISPLAY:
		ps = display;
		break;
	case CLUSTER_AVEMSR:
		ps = avemsr;
		break;
	default:
		return true;
	}

	for (; *ps; ps++) {
		void __iomem *reg = apple_s2r_ps(*ps);

		if (!reg || FIELD_GET(PS_ACTUAL, readl(reg)))
			return false;
	}

	return true;
}

/* AFR cluster word as it was before gfx_down took it to 0 */
static u32 s2r_afr_cl_pre;
static bool s2r_afr_cl_valid, s2r_afr_cl_kept, s2r_afr_cl_restored;

/* macOS enableCluster(false): only the CPU clusters are left to go down with their cores */
static void apple_s2r_cluster_write(unsigned int i)
{
	void __iomem *reg = s2r.cluster_ps + 4 * i;
	u32 val;

	s2r.cluster_saved[i] = readl(reg);
	writel(s2r.cluster_saved[i] & ~CLUSTER_PS_TARGET, reg);
	s2r.cluster_written |= BIT(i);
	if (!(CLUSTERS_CPU & BIT(i)) &&
	    readl_poll_timeout_atomic(reg, val, !(val & CLUSTER_PS_BUSY), 1, 10000))
		s2r.cluster_stuck |= BIT(i);
	s2r.cluster_readback[i] = readl(reg);
}

/* The boot CPU's own cluster, last when the boot CPU stays up to watch */
static void apple_s2r_cluster_eacc(void)
{
	if (!s2r.quiesce || !s2r.cluster_ps || (s2r.cluster_written & BIT(CLUSTER_EACC)))
		return;

	apple_s2r_cluster_write(CLUSTER_EACC);
}

static const u32 acc_regs[ACCR_N] = {
	[ACCR_PERF] = ACC_PERF_CTL,
	[ACCR_ENERGY] = ACC_ENERGY_CTL,
	[ACCR_AMX] = ACC_AMX_THRTL,
	[ACCR_LLC] = ACC_LLC_THRTL,
	[ACCR_PPT] = ACC_PPT_THRTL,
	[ACCR_DVFS] = ACC_DVFS_CMD,
};

static const char * const acc_reg_names[ACCR_N] = {
	[ACCR_PERF] = "perf",
	[ACCR_ENERGY] = "energy",
	[ACCR_AMX] = "amx_thrtl",
	[ACCR_LLC] = "llc_thrtl",
	[ACCR_PPT] = "ppt_thrtl",
	[ACCR_DVFS] = "dvfs",
};

/* 0x164, 0x16c, 0x168, 0x15c, 0x170 as macOS takes them; 0x160 stays */
static const u8 clkgen_mux_off[T6000_CLKGEN_MUXES] = { 0x08, 0x10, 0x0c, 0x00, 0x14 };

static u32 apple_s2r_socq_regs(void)
{
	u32 r = 0;

	if (s2r.socq_done & SOCQ_COUNTERS)
		r |= BIT(ACCR_PERF) | BIT(ACCR_ENERGY);
	if (s2r.socq_done & SOCQ_THROTTLERS)
		r |= BIT(ACCR_AMX) | BIT(ACCR_LLC) | BIT(ACCR_PPT);
	if (s2r.socq_done & SOCQ_APSC)
		r |= BIT(ACCR_DVFS);

	return r;
}

/* The same bits off in one register of each CPU complex */
static void apple_s2r_acc_clear(int r, u64 bits)
{
	int i;

	for (i = 0; i < T6000_ACCS; i++) {
		void __iomem *reg = s2r.acc[i] + acc_regs[r];

		s2r.acc_saved[i][r] = readq(reg);
		writeq(s2r.acc_saved[i][r] & ~bits, reg);
		s2r.acc_wrote[i][r] = readq(reg);
	}
}

/* quiesceACC and the feature loop, while every CPU complex is still up */
static void apple_s2r_quiesce_acc(void)
{
	u64 val;
	int i;

	if (!s2r.acc[0] || !s2r.acc[1] || !s2r.acc[2])
		return;

	apple_s2r_acc_clear(ACCR_PERF, ACC_PERF_EN);
	apple_s2r_acc_clear(ACCR_ENERGY, ACC_ENERGY_EN);
	s2r.socq_done |= SOCQ_COUNTERS;

	apple_s2r_acc_clear(ACCR_AMX, ACC_THRTL_EN);
	apple_s2r_acc_clear(ACCR_LLC, ACC_THRTL_EN);
	apple_s2r_acc_clear(ACCR_PPT, ACC_THRTL_EN);
	s2r.socq_done |= SOCQ_THROTTLERS;

	/* ACC0 and ACC1 pll relock */
	for (i = 0; i < 2; i++) {
		void __iomem *reg = s2r.acc[i] + ACC_DVFS_CMD;

		s2r.relock_saved[i] = readq(reg);
		writeq(ACC_DVFS_RELOCK, reg);
		if (readq_poll_timeout_atomic(reg, val, !(val & ACC_DVFS_APSC_BUSY), 1, 8000))
			s2r.socq_stuck |= SOCQ_RELOCK;
		s2r.relock_wrote[i] = val;
	}
	s2r.socq_done |= SOCQ_RELOCK;

	/* After cpufreq suspend put each complex on macOS's APSC value, twice as macOS does */
	for (i = 0; i < T6000_ACCS; i++) {
		void __iomem *reg = s2r.acc[i] + ACC_DVFS_CMD;
		int n;

		s2r.acc_saved[i][ACCR_DVFS] = readq(reg);
		for (n = 0; n < 2; n++) {
			writeq(ACC_DVFS_APSC_MACOS, reg);
			if (readq_poll_timeout_atomic(reg, val, !(val & ACC_DVFS_APSC_BUSY),
						      1, 8000))
				s2r.socq_stuck |= SOCQ_APSC;
		}
		s2r.acc_wrote[i][ACCR_DVFS] = val;
	}
	s2r.socq_done |= SOCQ_APSC;
}

/*
 * The tail of quiesceHW: the SoC clocks onto the reference clock, then PLL 0
 * in off mode 3, twice as macOS does around its media PS check. The PLL is
 * only touched once every mux has left it.
 */
static void apple_s2r_quiesce_clocks(void)
{
	u32 val;
	int i, n;

	if (!s2r.clkgen)
		return;

	for (i = 0; i < T6000_CLKGEN_MUXES; i++) {
		void __iomem *reg = s2r.clkgen + clkgen_mux_off[i];

		s2r.mux_saved[i] = readl(reg);
		writel((s2r.mux_saved[i] & ~CLKGEN_MUX_SEL) | CLKGEN_MUX_ON, reg);
		s2r.mux_n = i + 1;
		if (readl_poll_timeout_atomic(reg, val, !(val & CLKGEN_MUX_BUSY), 1, 8000)) {
			s2r.socq_stuck |= SOCQ_CLKGEN;
			return;
		}
		s2r.mux_wrote[i] = val;
	}
	s2r.socq_done |= SOCQ_CLKGEN;

	if (!s2r.pll)
		return;

	s2r.pll_saved = readl(s2r.pll);
	s2r.socq_done |= SOCQ_PLL;
	for (n = 0; n < 2; n++) {
		writel((readl(s2r.pll) & ~PLL_OFF_MODE) | FIELD_PREP(PLL_OFF_MODE, 3), s2r.pll);
		writel(readl(s2r.pll) | PLL_OFF_OK, s2r.pll);
		writel(readl(s2r.pll) & ~PLL_ON, s2r.pll);
		if (readl_poll_timeout_atomic(s2r.pll, val, !(val & PLL_BUSY), 1, 8000)) {
			s2r.socq_stuck |= SOCQ_PLL;
			break;
		}
	}
	s2r.pll_wrote = readl(s2r.pll);
}

static bool apple_s2r_dcs_set(u32 ps, u32 *out)
{
	u32 val = readl(s2r.dcs);

	writel((val & ~DCS_PS_TARGET) | FIELD_PREP(DCS_PS_TARGET, ps), s2r.dcs);
	return !readl_poll_timeout_atomic(s2r.dcs, *out, !(*out & DCS_PS_BUSY), 1, 8000);
}

/*
 * quiescePerfState, then the start of the T6000 tail: DCS 7 and 5, AFR PS 0.
 * The PMP owns DCS while it runs, so only once its PS is really down.
 */
static void apple_s2r_quiesce_dcs_afr(void)
{
	u32 val;

	if (s2r.dcs && s2r.pmp_ps) {
		s2r.pmp_ps_seen = readl(s2r.pmp_ps);
		if (!s2r.pmp_idled || FIELD_GET(PS_ACTUAL, s2r.pmp_ps_seen)) {
			s2r.socq_skipped |= SOCQ_DCS;
		} else {
			s2r.dcs_saved = readl(s2r.dcs);
			s2r.socq_done |= SOCQ_DCS;
			if (!apple_s2r_dcs_set(7, &s2r.dcs_mid) ||
			    !apple_s2r_dcs_set(5, &s2r.dcs_wrote))
				s2r.socq_stuck |= SOCQ_DCS;
		}
	}

	if (s2r.afr_ps) {
		s2r.afr_saved = readl(s2r.afr_ps);
		s2r.socq_done |= SOCQ_AFR;
		writel(0, s2r.afr_ps);
		if (readl_poll_timeout_atomic(s2r.afr_ps, val, !(val & PS_ACTUAL), 1, 8000))
			s2r.socq_stuck |= SOCQ_AFR;
		s2r.afr_wrote = val;
	}
}

/* restoreHW: AFR first, then DCS 7 and back to what the PMP had (macOS goes on to 9) */
static void apple_s2r_dcs_afr_restore(void)
{
	u32 val;

	if (s2r.socq_done & SOCQ_AFR) {
		s2r.afr_wake = readl(s2r.afr_ps);
		writel(s2r.afr_saved, s2r.afr_ps);
		readl_poll_timeout_atomic(s2r.afr_ps, val,
					  FIELD_GET(PS_ACTUAL, val) == FIELD_GET(PS_TARGET, val),
					  1, 8000);
		s2r.afr_restored = val;
	}
	if (s2r.socq_done & SOCQ_DCS) {
		s2r.dcs_wake = readl(s2r.dcs);
		apple_s2r_dcs_set(7, &val);
		apple_s2r_dcs_set(FIELD_GET(DCS_PS_TARGET, s2r.dcs_saved), &s2r.dcs_restored);
	}
}

/* PLL back on and locked before any mux goes back to it */
static void apple_s2r_clocks_restore(void)
{
	u32 val = 0;
	int i;

	if (s2r.socq_done & SOCQ_PLL) {
		s2r.pll_wake = readl(s2r.pll);
		writel(s2r.pll_wake | PLL_ON, s2r.pll);
		readl_poll_timeout_atomic(s2r.pll, val, !(val & PLL_BUSY), 1, 8000);
		writel(s2r.pll_saved, s2r.pll);
		readl_poll_timeout_atomic(s2r.pll, val, !(val & PLL_BUSY), 1, 8000);
		s2r.pll_restored = val;
		/* Better slow on the reference clock than on a PLL that never locked */
		if (val & PLL_BUSY)
			return;
	}

	for (i = s2r.mux_n - 1; i >= 0; i--) {
		void __iomem *reg = s2r.clkgen + clkgen_mux_off[i];

		s2r.mux_wake[i] = readl(reg);
		writel(s2r.mux_saved[i], reg);
		readl_poll_timeout_atomic(reg, val, !(val & CLKGEN_MUX_BUSY), 1, 8000);
		s2r.mux_restored[i] = val;
	}
}

/* Once the CPU complexes have power again */
static void apple_s2r_acc_restore(void)
{
	u32 regs = apple_s2r_socq_regs();
	u64 val;
	int i, r;

	for (i = 0; i < T6000_ACCS; i++) {
		for (r = ACCR_N - 1; r >= 0; r--) {
			void __iomem *reg = s2r.acc[i] + acc_regs[r];

			if (!(regs & BIT(r)))
				continue;
			/* Only the APSC bit: cpufreq owns the rest of DVFS_CMD */
			if (r == ACCR_DVFS) {
				val = readq(reg) & ~ACC_DVFS_APSC_OFF;
				writeq(val | (s2r.acc_saved[i][r] & ACC_DVFS_APSC_OFF), reg);
				readq_poll_timeout_atomic(reg, val, !(val & ACC_DVFS_APSC_BUSY),
							  1, 8000);
			} else {
				writeq(s2r.acc_saved[i][r], reg);
			}
			s2r.acc_restored[i][r] = readq(reg);
		}
	}
}

/*
 * quiesceHW, on the boot CPU alone while the parked CPUs still wait for the
 * release, as in macOS. A CPU cluster target is only a request: the cluster
 * goes down once its cores are all in their last WFI.
 *
 * The memory controller steps come first: DCS +0x10 bit 12 on as XNU sets it
 * at boot (written, never read), then each AMCC drained.
 */
static void apple_s2r_quiesce_mcc(void)
{
	u32 val;
	int a, i;

	for (a = 0; a < T6000_AMCCS; a++)
		if (!s2r.amcc[a]) {
			s2r.socq_skipped |= SOCQ_MCC;
			return;
		}

	for (a = 0; a < T6000_AMCCS; a++)
		for (i = 0; i < T6000_DCS_PER_AMCC; i++)
			if (s2r.dcs_blk[a][i])
				writel(DCS_CTL_MACOS, s2r.dcs_blk[a][i] + DCS_CTL);
	s2r.mcc_done |= MCC_DCS_LP;

	for (a = 0; a < T6000_AMCCS; a++)
		writel(1, s2r.amcc[a] + AMCC_LOCK);
	for (a = 0; a < T6000_AMCCS; a++)
		writel(0, s2r.amcc[a] + AMCC_DRAIN_CTL);
	for (a = 0; a < T6000_AMCCS; a++)
		writel(0, s2r.amcc[a] + AMCC_LOCK);
	for (a = 0; a < T6000_AMCCS; a++) {
		s2r.mcc_drain_cnt[a] = readl(s2r.amcc[a] + AMCC_DRAIN_CNT);
		if (readl_poll_timeout_atomic(s2r.amcc[a] + AMCC_DRAIN_CNT, val, !val, 1, 20000))
			s2r.socq_stuck |= SOCQ_MCC;
	}
	s2r.mcc_done |= MCC_DRAIN;
	s2r.socq_done |= SOCQ_MCC;
}

/* DCS +0x10 back to what iBoot left */
static void apple_s2r_mcc_restore(void)
{
	int a, i;

	if (s2r.mcc_done & MCC_DCS_LP)
		for (a = 0; a < T6000_AMCCS; a++)
			for (i = 0; i < T6000_DCS_PER_AMCC; i++)
				if (s2r.dcs_blk[a][i])
					writel(DCS_CTL_IBOOT, s2r.dcs_blk[a][i] + DCS_CTL);
}

static void apple_s2r_quiesce(void)
{
	int i;

	if (!s2r.quiesce)
		return;

	apple_s2r_quiesce_mcc();

	for (i = 0; i < SOCQ_ZERO_N; i++) {
		if (!s2r.zero[i])
			continue;
		s2r.zero_saved[i] = readl(s2r.zero[i]);
		writel(0, s2r.zero[i]);
	}
	s2r.socq_done |= SOCQ_ZEROS;
	/*
	 * No PS reads here: reading the three apcie PS words this late on the
	 * way down left the SoC stuck before sleep in 4 runs out of 4.
	 */
	if (s2r.apcie_8014 && s2r.apcie_82bc) {
		s2r.apcie_saved[0] = readl(s2r.apcie_8014);
		s2r.apcie_saved[1] = readl(s2r.apcie_82bc);
		writel(1, s2r.apcie_8014);
		writel(1, s2r.apcie_82bc);
		writel(2, s2r.apcie_82bc);
		writel(4, s2r.apcie_82bc);
		s2r.socq_done |= SOCQ_APCIE;
	}
	apple_s2r_quiesce_acc();
	apple_s2r_quiesce_dcs_afr();

	for (i = 0; s2r.cluster_ps && i < T6000_PMGR_CLUSTERS; i++) {
		if (!(CLUSTERS_VALID & BIT(i)))
			continue;
		if (!apple_s2r_cluster_gate(i)) {
			s2r.cluster_skipped |= BIT(i);
			continue;
		}
		apple_s2r_cluster_write(i);
	}

	apple_s2r_quiesce_clocks();
}

/* Whatever cpu_suspend() returned, before any parked CPU is brought back */
static void apple_s2r_quiesce_restore(void)
{
	unsigned int done = s2r.socq_done;
	u32 val;
	int i;

	apple_s2r_mcc_restore();
	apple_s2r_clocks_restore();

	for (i = 0; i < T6000_PMGR_CLUSTERS; i++) {
		void __iomem *reg = s2r.cluster_ps + 4 * i;

		if (!(s2r.cluster_written & BIT(i)))
			continue;
		s2r.cluster_wake[i] = readl(reg);
		if (i == CLUSTER_AFR)
			s2r_afr_cl_restored = true;
		if (i == CLUSTER_AFR && s2r_afr_cl_valid &&
		    !(s2r.cluster_saved[i] & CLUSTER_PS_TARGET)) {
			/*
			 * gfx_down took it to 0 first: back to what it was before
			 * that. Writing 0 leaves it busy at 0x30 for good, and the
			 * next S2R stalls.
			 */
			if ((s2r.cluster_wake[i] ^ s2r_afr_cl_pre) & CLUSTER_PS_TARGET)
				writel(s2r_afr_cl_pre, reg);
			s2r_afr_cl_kept = true;
		} else {
			writel(s2r.cluster_saved[i], reg);
		}
		readl_poll_timeout_atomic(reg, val, !(val & CLUSTER_PS_BUSY), 1, 10000);
		s2r.cluster_restored[i] = val;
	}

	apple_s2r_dcs_afr_restore();
	apple_s2r_acc_restore();

	for (i = 1; (done & SOCQ_RELOCK) && i >= 0; i--) {
		void __iomem *reg = s2r.acc[i] + ACC_DVFS_CMD;
		u64 v;

		writeq(s2r.relock_saved[i], reg);
		readq_poll_timeout_atomic(reg, v, !(v & ACC_DVFS_APSC_BUSY), 1, 8000);
	}
	/* After a real S2R the block comes back powered off and reset: nothing to restore */
	apcie_skipped = (done & SOCQ_APCIE) && !apple_s2r_apcie_on(apcie_ps_wake);
	if ((done & SOCQ_APCIE) && !apcie_skipped) {
		writel(s2r.apcie_saved[1], s2r.apcie_82bc);
		writel(s2r.apcie_saved[0], s2r.apcie_8014);
	}

	/* TVMs first back, then counters, then the error handlers */
	for (i = SOCQ_ZERO_N - 1; (done & SOCQ_ZEROS) && i >= 0; i--) {
		if (!s2r.zero[i])
			continue;
		s2r.zero_wake[i] = readl(s2r.zero[i]);
		writel(s2r.zero_saved[i], s2r.zero[i]);
	}
}

static void apple_s2r_quiesce_report(void)
{
	u32 regs;
	int i, r;

	if (!s2r.quiesce)
		return;

	if (s2r.cluster_skipped)
		pr_debug("clusters %#x left on: their blocks were still on\n", s2r.cluster_skipped);
	if (s2r.cluster_stuck)
		pr_warn("clusters %#x were still busy 10 ms after their target went to 0\n",
			s2r.cluster_stuck);
	if (s2r_afr_cl_kept)
		pr_debug("cluster PS 5: back to %#x from before gfx_down\n", s2r_afr_cl_pre);
	s2r_afr_cl_kept = false;
	s2r_afr_cl_valid = false;
	for (i = 0; i < T6000_PMGR_CLUSTERS; i++)
		if (s2r.cluster_written & BIT(i))
			pr_debug("cluster PS %d: %#x, wrote %#x, %#x after wake, restored %#x\n", i,
				 s2r.cluster_saved[i], s2r.cluster_readback[i], s2r.cluster_wake[i],
				 s2r.cluster_restored[i]);
	pr_debug("quiesce: did %#x, stuck %#x, skipped %#x\n", s2r.socq_done, s2r.socq_stuck,
		 s2r.socq_skipped);
	pr_debug("dcs: PMP PS %#x, %#x -> %#x -> %#x, %#x after wake, restored %#x\n",
		 s2r.pmp_ps_seen, s2r.dcs_saved, s2r.dcs_mid, s2r.dcs_wrote, s2r.dcs_wake,
		 s2r.dcs_restored);
	for (i = 0; (s2r.socq_done & SOCQ_MCC) && i < T6000_AMCCS; i++)
		pr_debug("amcc%d: drain %#x\n", i, s2r.mcc_drain_cnt[i]);
	if (s2r.socq_done & SOCQ_APCIE)
		pr_debug("apcie handshake: 8014 %#x, 82bc %#x before%s\n", s2r.apcie_saved[0],
			 s2r.apcie_saved[1],
			 apcie_skipped ? ", not restored: the block came back off" : "");
	if (s2r.socq_done & SOCQ_APCIE)
		pr_debug("apcie PS after wake: %#x %#x %#x\n", apcie_ps_wake[0],
			 apcie_ps_wake[1], apcie_ps_wake[2]);
	for (i = 0; (s2r.socq_done & SOCQ_RELOCK) && i < 2; i++)
		pr_debug("ACC%d relock: %#llx -> %#llx\n", i, s2r.relock_saved[i],
			 s2r.relock_wrote[i]);
	for (i = 0; (s2r.socq_done & SOCQ_ZEROS) && i < SOCQ_ZERO_N; i++)
		pr_debug("zero %#llx: %#x, %#x after wake\n", socq_zero_pa[i], s2r.zero_saved[i],
			 s2r.zero_wake[i]);
	if (s2r.socq_done & SOCQ_AFR)
		pr_debug("afr PS: %#x, wrote 0 -> %#x, %#x after wake, restored %#x\n",
			 s2r.afr_saved, s2r.afr_wrote, s2r.afr_wake, s2r.afr_restored);
	regs = apple_s2r_socq_regs();
	for (i = 0; i < T6000_ACCS; i++)
		for (r = 0; r < ACCR_N; r++)
			if (regs & BIT(r))
				pr_debug("ACC%d %-9s: %#018llx, wrote %#018llx, restored %#018llx\n",
					 i, acc_reg_names[r], s2r.acc_saved[i][r],
					 s2r.acc_wrote[i][r], s2r.acc_restored[i][r]);
	for (i = 0; i < s2r.mux_n; i++)
		pr_debug("ClkGen mux 0x%llx: %#010x, wrote %#010x, %#010x after wake, restored %#010x\n",
			 T6000_CLKGEN_MUX + clkgen_mux_off[i], s2r.mux_saved[i], s2r.mux_wrote[i],
			 s2r.mux_wake[i], s2r.mux_restored[i]);
	if (s2r.socq_done & SOCQ_PLL)
		pr_debug("PLL 0: %#010x, wrote %#010x, %#010x after wake, restored %#010x\n",
			 s2r.pll_saved, s2r.pll_wrote, s2r.pll_wake, s2r.pll_restored);
	if (s2r.socq_stuck)
		pr_warn("quiesce steps %#x did not finish within 8 ms\n", s2r.socq_stuck);
}

/*
 * Only touches WD1 when the dead-man armed it for this sleep: apple_wdt
 * stops it in device suspend and owns it again from device resume.
 */
static void apple_s2r_wdt_mark(void)
{
	u32 ticks = S2R_WDT_WAKE_S * WDT_TICKS_PER_S;

	if (!s2r.wdt || !(readl(s2r.wdt + WDT_WD1_CTL) & WDT_CTL_RESET_EN))
		return;

	writel(0, s2r.wdt + WDT_WD1_CUR);
	writel(ticks, s2r.wdt + WDT_WD1_BITE);
}

/*
 * The PMGR AP wake mask while asleep (restored after wake): what macOS 13.7.8
 * writes to 0x292004008..0x292004020 at boot (RE13)
 */
static const u32 t6000_ap_wake_macos[PMGR_AP_WAKE_WORDS] = {
	0xf3ffafff, 0xffa1f6ff, 0xfbdfffff, 0x3fffffff, 0xffffffc0, 0xffffffff, 0x00001fff,
};

static void apple_s2r_pmgr_read(u32 *mask, u32 *status)
{
	int i;

	for (i = 0; i < PMGR_AP_WAKE_WORDS; i++) {
		if (mask)
			mask[i] = readl(s2r.pmgr_wake + PMGR_AP_WAKE_MASK + 4 * i);
		if (status)
			status[i] = readl(s2r.pmgr_wake + PMGR_AP_WAKE_STATUS + 4 * i);
	}
}

static void apple_s2r_pmgr_write(const u32 *mask)
{
	int i;

	for (i = 0; i < PMGR_AP_WAKE_WORDS; i++)
		writel(mask[i], s2r.pmgr_wake + PMGR_AP_WAKE_MASK + 4 * i);
}

static void apple_s2r_pmgr_log(const char *when)
{
	u32 mask[PMGR_AP_WAKE_WORDS], status[PMGR_AP_WAKE_WORDS];
	char m[72], s[72];

	if (!s2r.pmgr_wake)
		return;

	apple_s2r_pmgr_read(mask, status);
	apple_s2r_words(m, sizeof(m), mask);
	apple_s2r_words(s, sizeof(s), status);
	pr_debug("PMGR %s: mask %s status %s\n", when, m, s);
}

/* Runs on the boot CPU alone with IRQs off, right before it powers down */
static void apple_s2r_pmgr_mask_set(void)
{
	if (!s2r.pmgr_wake)
		return;

	memcpy(s2r.pmgr_wanted, t6000_ap_wake_macos, sizeof(s2r.pmgr_wanted));

	apple_s2r_pmgr_read(s2r.pmgr_saved, NULL);
	s2r.pmgr_written = true;
	apple_s2r_pmgr_write(s2r.pmgr_wanted);
	/* Also makes sure the writes landed before the core goes down */
	apple_s2r_pmgr_read(s2r.pmgr_readback, NULL);
}

/* Right after wake, whatever cpu_suspend() returned: what woke it, and the old mask back */
static void apple_s2r_pmgr_wake(void)
{
	if (!s2r.pmgr_wake)
		return;

	apple_s2r_pmgr_read(s2r.pmgr_wake_mask, s2r.pmgr_wake_status);
	s2r.pmgr_woke = true;

	if (s2r.pmgr_written) {
		apple_s2r_pmgr_write(s2r.pmgr_saved);
		apple_s2r_pmgr_read(s2r.pmgr_restored, NULL);
	}
}

static void apple_s2r_pmgr_report(void)
{
	char a[72], b[72], ids[256];

	if (!s2r.pmgr_woke)
		return;

	if (s2r.pmgr_written) {
		apple_s2r_words(a, sizeof(a), s2r.pmgr_wanted);
		if (memcmp(s2r.pmgr_readback, s2r.pmgr_wanted, sizeof(s2r.pmgr_wanted))) {
			apple_s2r_words(b, sizeof(b), s2r.pmgr_readback);
			pr_warn("PMGR mask written: %s, read back %s\n", a, b);
		} else {
			pr_debug("PMGR mask written: %s, read back the same\n", a);
		}
	}

	apple_s2r_words(a, sizeof(a), s2r.pmgr_wake_mask);
	apple_s2r_words(b, sizeof(b), s2r.pmgr_wake_status);
	pr_debug("PMGR after wake: mask %s status %s\n", a, b);
	apple_s2r_bits(ids, sizeof(ids), s2r.pmgr_wake_status, PMGR_AP_WAKE_WORDS,
		       t6000_ap_wake_sources, ARRAY_SIZE(t6000_ap_wake_sources));
	pr_debug("PMGR wake status:%s\n", ids);

	if (s2r.pmgr_written) {
		apple_s2r_words(a, sizeof(a), s2r.pmgr_saved);
		if (memcmp(s2r.pmgr_restored, s2r.pmgr_saved, sizeof(s2r.pmgr_saved))) {
			apple_s2r_words(b, sizeof(b), s2r.pmgr_restored);
			pr_err("PMGR mask restore failed: read back %s, saved %s\n", b, a);
		} else {
			pr_debug("PMGR mask restored: %s\n", a);
		}
	}
}

/* SIO ASC CPU control block; only touched once sio_cpu reports it is on */
#define PS_LATE_SIO_ASC 0x39bc00000UL
#define PS_LATE_SIO_CPU_PS 0x28e580188UL

/*
 * SIO blocks macOS touches last before GO: ASC CPU control, the mailbox
 * control words and the two SIO DARTs (stream enables, TCRs). Only written
 * while sio_cpu reports it is on.
 */
static bool apple_s2r_ps_late_sio_pa(unsigned long pa)
{
	return (pa >= PS_LATE_SIO_ASC && pa < PS_LATE_SIO_ASC + 0x100) ||
	       (pa >= 0x39bc08100UL && pa < 0x39bc08120UL) ||
	       (pa >= 0x39b004000UL && pa < 0x39b004200UL) ||
	       (pa >= 0x39b008000UL && pa < 0x39b008200UL);
}

/*
 * Raw PS values written right before the S2R timer, after every other CPU is
 * parked, to leave the PMGR the way macOS has it at GO, without polling:
 * address, value (e.g. sio_cpu 0x28e580188 = 0xf0002ff); address 0 waits that
 * many microseconds.
 */
static const unsigned long ps_late[][2] = {
	{ 0x28e580160, 0x1f0000ff }, { 0x28e580180, 0x1f0000ff }, { 0x0, 0x7d0 },
	{ 0x2922800d0, 0x300 }, { 0x2922800e0, 0x300 }, { 0x2922800d8, 0x300 },
	{ 0x292280088, 0x300 }, { 0x292280098, 0x300 }, { 0x292280090, 0x300 },
	{ 0x2922800a0, 0x300 }, { 0x28e080330, 0x300 }, { 0x28e080340, 0x300 },
	{ 0x28e080350, 0x300 }, { 0x28e080360, 0x300 }, { 0x28e080238, 0x300 },
	{ 0x28e0802f8, 0x300 }, { 0x28e080300, 0x300 }, { 0x28e580250, 0x200 },
	{ 0x28e580258, 0x200 }, { 0x28e580260, 0x200 }, { 0x28e580268, 0x200 },
	{ 0x28e580270, 0x200 }, { 0x292280078, 0x200 }, { 0x28e080220, 0x1000030f },
	{ 0x28e580160, 0x1000030f }, { 0x28e580178, 0x1000030f }, { 0x28e080320, 0x1000030f },
	{ 0x28e080138, 0x1400024f }, { 0x28e080140, 0x1400024f }, { 0x28e0802e8, 0x1f0003ff },
	{ 0x28e0802f0, 0x1f0003ff }, { 0x28e080148, 0xff }, { 0x28e080120, 0xff },
	{ 0x28e080130, 0xff }, { 0x28e580188, 0xf0002ff }, { 0x28e580190, 0x2ff },
	{ 0x28e5801e8, 0xff }, { 0x28e580218, 0x1f0000ff }, { 0x28e580220, 0x1f0000ff },
	{ 0x28e580228, 0x1f0000ff }, { 0x28e580238, 0xff }, { 0x28e580248, 0xff },
	{ 0x28e580278, 0x2ff }, { 0x28e5802d0, 0x3ff }, { 0x28e5803b8, 0x1f0003ff },
	{ 0x28e038104, 0x80100000 }, { 0x28e038108, 0x80100000 }, { 0x28e03810c, 0x80100000 },
	{ 0x28e038114, 0x8a100100 }, { 0x28e038120, 0x85100100 }, { 0x28e038124, 0x85100100 },
	{ 0x28e038128, 0x87100100 }, { 0x28e03812c, 0x87100100 }, { 0x28e038130, 0x87100100 },
	{ 0x28e038134, 0x87100100 }, { 0x28e038138, 0x87100100 }, { 0x28e03813c, 0x87100100 },
	{ 0x28e038148, 0x80100100 }, { 0x28e03814c, 0x85100100 }, { 0x28e038160, 0x85100100 },
	{ 0x28e038180, 0x87100000 }, { 0x28e038184, 0x87100000 }, { 0x28e038188, 0x87100000 },
	{ 0x28e03818c, 0x87100100 }, { 0x292240000, 0x80100010 }, { 0x28e20c000, 0x333 },
	{ 0x0, 0x7d0 }, { 0x28e580188, 0xf00000f }, { 0x39bc00044, 0x0 },
	{ 0x39bc08114, 0x0 }, { 0x39b0040fc, 0x0 }, { 0x39b0080fc, 0x0 },
	{ 0x28e080138, 0x0 }, { 0x28e080320, 0x0 }, { 0x28e038098, 0x0 },
	{ 0x28e03809c, 0x0 }, { 0x28e0380a0, 0x0 }, { 0x28e0380a4, 0x0 },
	{ 0x0, 0x64 },
};

#define PS_LATE_N ARRAY_SIZE(ps_late)

/* After a sleep, WD1 goes back as it was before it, once the devices are back */
static bool wdt_saved;
static u32 wdt_saved_ctl, wdt_saved_bite;

/* The wait after the ps_late writes, before the S2R timer (the hv takes ~100 ms there) */
#define S2R_PS_LATE_MS 200

static void __iomem *ps_late_reg[PS_LATE_N];
static unsigned int ps_late_mapped;

static void apple_s2r_ps_late_unmap(void)
{
	unsigned int i;

	for (i = 0; i < ps_late_mapped; i++)
		if (ps_late_reg[i])
			iounmap(ps_late_reg[i]);
	ps_late_mapped = 0;
}

static void apple_s2r_ps_late_map(void)
{
	unsigned int i;

	apple_s2r_ps_late_unmap();
	for (i = 0; i < PS_LATE_N; i++) {
		unsigned long pa = ps_late[i][0];

		ps_late_reg[i] = ((pa >= 0x28e000000UL && pa < 0x293000000UL) ||
				  apple_s2r_ps_late_sio_pa(pa)) ? ioremap_np(pa & ~3UL, 4) : NULL;
	}
	ps_late_mapped = i;
}

/*
 * After a deep sleep, the PMGR registers ps_late wrote go back the way Linux
 * had them before any device resumes: Linux still thinks those power domains
 * are on.
 */
static u32 ps_late_saved[PS_LATE_N];
static unsigned long ps_late_is_ps[BITS_TO_LONGS(PS_LATE_N)];
static unsigned long ps_late_has_saved[BITS_TO_LONGS(PS_LATE_N)];
static bool ps_late_applied;

/* PMGR registers only, and none that iBoot armed or that must never be touched */
static bool apple_s2r_ps_late_restore_pa_ok(unsigned long pa)
{
	if (pa < 0x28e000000UL || pa >= 0x293000000UL)
		return false;
	if ((pa >= 0x28e200000UL && pa < 0x28e208000UL) ||
	    (pa >= 0x28e941000UL && pa < 0x28e942000UL) ||
	    (pa >= 0x28e59c000UL && pa < 0x28e59d000UL) ||
	    (pa >= 0x28e099000UL && pa < 0x28e099078UL) ||
	    (pa >= 0x292000200UL && pa < 0x292000204UL) ||
	    (pa >= 0x2922b0000UL && pa < 0x2922b0100UL) ||
	    (pa >= 0x2922c0000UL && pa < 0x2922c0314UL) ||
	    (pa >= 0x2922dc21cUL && pa < 0x2922dc22cUL) ||
	    (pa >= 0x2925c0000UL && pa < 0x2925d4000UL) ||
	    (pa >= 0x292f00000UL && pa < 0x292f70000UL))
		return false;
	return true;
}

static bool apple_s2r_pa_is_ps(unsigned long pa)
{
	struct device_node *np, *parent;
	struct resource res;
	bool found = false;
	u32 off;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (of_property_read_u32(np, "reg", &off))
			continue;
		parent = of_get_parent(np);
		if (parent && !of_address_to_resource(parent, 0, &res) && res.start + off == pa)
			found = true;
		of_node_put(parent);
		if (found) {
			of_node_put(np);
			break;
		}
	}
	return found;
}

/* Awake, from begin(): what Linux has in every PMGR register ps_late will write */
static void apple_s2r_ps_late_save(void)
{
	unsigned int i;

	bitmap_zero(ps_late_is_ps, PS_LATE_N);
	bitmap_zero(ps_late_has_saved, PS_LATE_N);
	ps_late_applied = false;
	for (i = 0; i < ps_late_mapped; i++) {
		unsigned long pa = ps_late[i][0];

		if (!pa || !ps_late_reg[i] || !apple_s2r_ps_late_restore_pa_ok(pa))
			continue;
		ps_late_saved[i] = readl(ps_late_reg[i]);
		__set_bit(i, ps_late_has_saved);
		if (apple_s2r_pa_is_ps(pa))
			__set_bit(i, ps_late_is_ps);
	}
}

/*
 * After a deep sleep, before any device resumes. ps_late powered domains off
 * the way macOS does right before GO, children before parents; walk it
 * backwards so parents come back first. Each address once, to what Linux had.
 */
static void apple_s2r_ps_late_put_back(void)
{
	unsigned int i, j, n = 0, slow = 0;
	unsigned long done[BITS_TO_LONGS(PS_LATE_N)];

	if (!ps_late_applied)
		return;
	ps_late_applied = false;
	bitmap_zero(done, PS_LATE_N);
	for (i = ps_late_mapped; i-- > 0;) {
		unsigned long pa = ps_late[i][0];
		bool again = false;
		u32 v, t;

		if (!test_bit(i, ps_late_has_saved))
			continue;
		for (j = i + 1; j < ps_late_mapped; j++)
			if (ps_late[j][0] == pa && test_bit(j, done))
				again = true;
		if (again)
			continue;
		__set_bit(i, done);
		v = ps_late_saved[i];
		/*
		 * A power state raised while its parent is still off never comes
		 * up (afnc1_lw2 under afnc1_ls): the ones Linux had on go through
		 * apple_s2r_psall_restore(), parents first.
		 */
		if (test_bit(i, ps_late_is_ps) && FIELD_GET(PS_ACTUAL, v))
			continue;
		if (!test_bit(i, ps_late_is_ps)) {
			writel(v, ps_late_reg[i]);
			n++;
			continue;
		}
		/* The was-gated bits are write-one-to-clear, the actual state read only */
		writel(v & ~(BIT(9) | BIT(8) | PS_ACTUAL), ps_late_reg[i]);
		for (t = 0; t < 10000; t++) {
			if (FIELD_GET(PS_ACTUAL, readl(ps_late_reg[i])) == FIELD_GET(PS_TARGET, v))
				break;
			udelay(10);
		}
		if (t == 10000) {
			slow++;
			pr_warn("ps_late restore: %#lx target %lx, still %#x\n", pa,
				FIELD_GET(PS_TARGET, v), readl(ps_late_reg[i]));
		}
		n++;
	}
	pr_debug("ps_late restore: %u registers put back, %u power states late\n", n, slow);
}

/*
 * ps_all: after a deep sleep, every PMGR domain that was on before it goes back
 * on, parents first, before any device resumes (never SEP, AFC or AFI). The
 * wake walks the saved power states up to this many times, 10 ms apart, while
 * some are still not back: a fabric parent can take longer than one poll.
 */
#define S2R_PSALL_PASSES 30

#define S2R_PSALL_MAX 256
#define S2R_PSALL_BASES 8
static struct {
	struct device_node *np;
	void __iomem *reg;
	const char *name;
	unsigned long pa;
	u32 saved;
} s2r_psall[S2R_PSALL_MAX];
static unsigned int s2r_psall_n;
static struct {
	resource_size_t start;
	void __iomem *base;
} s2r_psall_base[S2R_PSALL_BASES];
static unsigned int s2r_psall_nbase;
static bool s2r_psall_armed;
static unsigned int s2r_psall_fixed, s2r_psall_stuck, s2r_psall_pass;

static void __iomem *apple_s2r_psall_map(struct device_node *parent, resource_size_t *start)
{
	struct resource res;
	unsigned int b;

	if (of_address_to_resource(parent, 0, &res))
		return NULL;
	*start = res.start;
	for (b = 0; b < s2r_psall_nbase; b++)
		if (s2r_psall_base[b].start == res.start)
			return s2r_psall_base[b].base;
	if (s2r_psall_nbase == S2R_PSALL_BASES)
		return NULL;
	s2r_psall_base[b].start = res.start;
	s2r_psall_base[b].base = ioremap_np(res.start, resource_size(&res));
	if (s2r_psall_base[b].base)
		s2r_psall_nbase++;
	return s2r_psall_base[b].base;
}

static void apple_s2r_psall_init(void)
{
	struct device_node *np, *parent;
	resource_size_t start;
	void __iomem *base;
	const char *label;
	u32 off;

	if (s2r_psall_n)
		return;
	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (s2r_psall_n == S2R_PSALL_MAX) {
			of_node_put(np);
			break;
		}
		label = NULL;
		of_property_read_string(np, "label", &label);
		if (!label || !strcmp(label, "sep") || !strcmp(label, "afc") ||
		    !strcmp(label, "afi"))
			continue;
		if (of_property_read_u32(np, "reg", &off))
			continue;
		parent = of_get_parent(np);
		base = parent ? apple_s2r_psall_map(parent, &start) : NULL;
		of_node_put(parent);
		if (!base || !apple_s2r_ps_late_restore_pa_ok(start + off))
			continue;
		s2r_psall[s2r_psall_n].np = of_node_get(np);
		s2r_psall[s2r_psall_n].reg = base + off;
		s2r_psall[s2r_psall_n].name = label;
		s2r_psall[s2r_psall_n].pa = start + off;
		s2r_psall_n++;
	}
	pr_debug("ps_all: %u power states to watch over a deep sleep\n", s2r_psall_n);
}

/*
 * The DCS power states right before GO in the first deep sleep of this boot,
 * which apple_s2r_dcs_restore() puts back right before a later GO.
 */
static u32 s2r_go_ref[S2R_PSALL_MAX];
static bool s2r_go_ref_taken;

static u32 apple_s2r_go_mask(u32 a, u32 b)
{
	u32 m = PS_TARGET | PS_ACTUAL | BIT(10) | GENMASK(19, 16) | PS_AUTO_ENABLE;

	if (a & b & PS_AUTO_ENABLE)
		m &= ~PS_ACTUAL;
	return m;
}

#define S2R_DCS_LOG 8
static u16 s2r_dcs_idx[S2R_DCS_LOG];
static u32 s2r_dcs_was[S2R_DCS_LOG], s2r_dcs_now[S2R_DCS_LOG];
static unsigned int s2r_dcs_n, s2r_dcs_events;
static unsigned int s2r_dcs_first;

/*
 * The wake engines wait for each DCS to drop to 0 on the way back. One that
 * goes to sleep in another state holds them there until WDT2 resets the SoC.
 */
static void apple_s2r_dcs_restore(void)
{
	bool any = false;
	unsigned int i;
	u32 v, ref, d;

	for (i = 0; i < s2r_psall_n; i++) {
		if (strncmp(s2r_psall[i].name, "dcs_", 4))
			continue;
		ref = s2r_go_ref[i];
		v = readl(s2r_psall[i].reg);
		d = (v ^ ref) & apple_s2r_go_mask(v, ref);
		if (!d || (ref & GENMASK(19, 16)))
			continue;
		if (!any)
			s2r_dcs_events++;
		any = true;
		writel((v & ~(PS_TARGET | PS_AUTO_ENABLE | GENMASK(19, 16) | BIT(31) |
			      BIT(9) | BIT(8))) |
		       (ref & (PS_TARGET | PS_AUTO_ENABLE | GENMASK(19, 16))), s2r_psall[i].reg);
		if (s2r_dcs_n < S2R_DCS_LOG) {
			udelay(100);
			s2r_dcs_idx[s2r_dcs_n] = i;
			s2r_dcs_was[s2r_dcs_n] = v;
			s2r_dcs_now[s2r_dcs_n++] = readl(s2r_psall[i].reg);
		}
	}
}

/* Right before GO: the first deep sleep takes the reference, later ones put a changed DCS back */
static void apple_s2r_go_check(void)
{
	unsigned int i;
	u32 v;

	if (!s2r_psall_n)
		return;
	if (!s2r_go_ref_taken) {
		for (i = 0; i < s2r_psall_n; i++) {
			if (strncmp(s2r_psall[i].name, "dcs_", 4))
				continue;
			v = readl(s2r_psall[i].reg);
			/*
			 * A floor already there on the first sleep must not end up in
			 * the reference.
			 */
			if (v & GENMASK(19, 16)) {
				writel(v & ~(GENMASK(19, 16) | BIT(31) | BIT(9) | BIT(8)),
				       s2r_psall[i].reg);
				udelay(100);
				s2r_dcs_first++;
				v = readl(s2r_psall[i].reg);
			}
			s2r_go_ref[i] = v;
		}
		s2r_go_ref_taken = true;
		return;
	}
	apple_s2r_dcs_restore();
}

static void apple_s2r_dcs_report(void)
{
	unsigned int i;

	if (s2r_dcs_first) {
		pr_debug("dcs_first: %u floors cleared on the first sleep\n", s2r_dcs_first);
		s2r_dcs_first = 0;
	}

	for (i = 0; i < s2r_dcs_n; i++)
		pr_debug("dcs_change %u: %s was %#010x at GO, first sleep %#010x, now %#010x\n",
			 s2r_dcs_events, s2r_psall[s2r_dcs_idx[i]].name, s2r_dcs_was[i],
			 s2r_go_ref[s2r_dcs_idx[i]], s2r_dcs_now[i]);
	s2r_dcs_n = 0;
}

static void apple_s2r_psall_save(void)
{
	unsigned int i;

	s2r_psall_armed = false;
	apple_s2r_psall_init();
	for (i = 0; i < s2r_psall_n; i++)
		s2r_psall[i].saved = readl(s2r_psall[i].reg);
}

static int apple_s2r_psall_find(struct device_node *np)
{
	unsigned int i;

	for (i = 0; i < s2r_psall_n; i++)
		if (s2r_psall[i].np == np)
			return i;
	return -1;
}

static void apple_s2r_psall_on(unsigned int i, unsigned int depth, unsigned long *seen)
{
	struct of_phandle_iterator it;
	u32 saved = s2r_psall[i].saved, cur;
	int err, j;
	unsigned int t;

	if (test_and_set_bit(i, seen) || depth > 8)
		return;
	/*
	 * The NUB PMGR keeps its state through the sleep. These are auto-PM
	 * domains, so a saved actual state only says a transfer was running
	 * when the snapshot was taken. Raising one for that would cost it
	 * AUTO_ENABLE for the rest of the boot.
	 */
	if (s2r_psall[i].pa >= T6000_NUB_PS)
		return;
	/* Linux had it off: leave it to genpd */
	if (!FIELD_GET(PS_ACTUAL, saved))
		return;
	of_for_each_phandle(&it, err, s2r_psall[i].np, "power-domains", "#power-domain-cells", 0) {
		j = apple_s2r_psall_find(it.node);
		if (j >= 0)
			apple_s2r_psall_on(j, depth + 1, seen);
	}
	cur = readl(s2r_psall[i].reg);
	/*
	 * The minimum state (apple,min-state, the DCP CPUs) is lost with the
	 * power: put it back first.
	 */
	if ((cur ^ saved) & GENMASK(19, 16)) {
		writel((cur & ~(BIT(12) | BIT(11) | BIT(10) | BIT(9) | BIT(8) | GENMASK(19, 16))) |
		       (saved & GENMASK(19, 16)), s2r_psall[i].reg);
		pr_debug("ps_all: %#llx min state back to %lu (was %#x)\n", (u64)s2r_psall[i].pa,
			 FIELD_GET(GENMASK(19, 16), saved), cur);
		cur = readl(s2r_psall[i].reg);
	}
	if (FIELD_GET(PS_TARGET, cur) == FIELD_GET(PS_TARGET, saved) &&
	    FIELD_GET(PS_ACTUAL, cur) >= FIELD_GET(PS_ACTUAL, saved))
		return;
	cur &= ~(BIT(12) | BIT(11) | BIT(10) | BIT(9) | BIT(8) | BIT(28) | PS_TARGET |
		 GENMASK(27, 24));
	cur |= saved & GENMASK(27, 24);
	if (s2r_psall_pass && !FIELD_GET(PS_ACTUAL, readl(s2r_psall[i].reg))) {
		writel(cur & ~PS_ACTUAL, s2r_psall[i].reg);
		udelay(100);
	}
	writel(cur | (saved & PS_TARGET), s2r_psall[i].reg);
	for (t = 0; t < 2000 && FIELD_GET(PS_ACTUAL, readl(s2r_psall[i].reg)) <
				  FIELD_GET(PS_ACTUAL, saved); t++)
		udelay(10);
	cur = readl(s2r_psall[i].reg) &
	      ~(BIT(12) | BIT(11) | BIT(10) | BIT(9) | BIT(8) | PS_ACTUAL);
	/*
	 * After the first pass, a power state still off comes up without
	 * AUTO_ENABLE: with it the PMGR keeps an idle fabric domain (afnc1_lw2)
	 * off even when a DART behind it is accessed.
	 */
	if ((saved & BIT(28)) && !s2r_psall_pass)
		writel(cur | BIT(28), s2r_psall[i].reg);
	cur = readl(s2r_psall[i].reg);
	if ((cur & BIT(31)) && !(saved & BIT(31)) && FIELD_GET(PS_ACTUAL, cur)) {
		writel(cur & ~(BIT(31) | BIT(11) | BIT(9) | BIT(8) | PS_ACTUAL), s2r_psall[i].reg);
		udelay(1);
		cur = readl(s2r_psall[i].reg);
	}
	if (FIELD_GET(PS_ACTUAL, cur) >= FIELD_GET(PS_ACTUAL, saved)) {
		s2r_psall_fixed++;
		pr_debug("ps_all: %s back on (%#x, was %#x)\n", s2r_psall[i].name, cur, saved);
	} else {
		s2r_psall_stuck++;
		pr_warn("ps_all: %s stays off (%#x, was %#x)\n", s2r_psall[i].name, cur, saved);
	}
}

static void apple_s2r_psall_restore(void)
{
	unsigned long seen[BITS_TO_LONGS(S2R_PSALL_MAX)];
	unsigned int i, pass;

	s2r_psall_stuck = 0;
	s2r_psall_fixed = 0;
	if (!s2r_psall_armed)
		return;
	s2r_psall_armed = false;
	for (pass = 0; pass < S2R_PSALL_PASSES; pass++) {
		if (pass)
			mdelay(10);
		s2r_psall_pass = pass;
		s2r_psall_stuck = 0;
		s2r_psall_fixed = 0;
		bitmap_zero(seen, S2R_PSALL_MAX);
		for (i = 0; i < s2r_psall_n; i++)
			apple_s2r_psall_on(i, 0, seen);
		pr_debug("ps_all: pass %u: %u power states back on, %u stay off\n", pass,
			 s2r_psall_fixed, s2r_psall_stuck);
		if (!s2r_psall_stuck)
			break;
	}
}

static void apple_s2r_ps_late(void)
{
	unsigned int i;

	ps_late_applied = !!bitmap_weight(ps_late_has_saved, PS_LATE_N);
	for (i = 0; i < ps_late_mapped; i++) {
		if (!ps_late[i][0]) {
			/* address 0: wait this many microseconds */
			udelay(min_t(unsigned long, ps_late[i][1], 20000));
			continue;
		}
		if (!ps_late_reg[i])
			continue;
		if (apple_s2r_ps_late_sio_pa(ps_late[i][0])) {
			void __iomem *ps = NULL;
			unsigned int j;

			for (j = 0; j < ps_late_mapped; j++)
				if (ps_late[j][0] == PS_LATE_SIO_CPU_PS && ps_late_reg[j])
					ps = ps_late_reg[j];
			if (ps) {
				unsigned int t;

				/* the PS needs a moment after its target is raised */
				for (t = 0; t < 2000 && ((readl(ps) >> 4) & 0xf) != 0xf; t++)
					udelay(1);
			}
			if (!ps || ((readl(ps) >> 4) & 0xf) != 0xf)
				continue;
		}
		writel((u32)ps_late[i][1], ps_late_reg[i]);
	}
	dsb(sy);
	if (ps_late_mapped)
		mdelay(S2R_PS_LATE_MS);
}

/* The SoC lost power in the sleep that is ending: the m1n1 resume count moved */
static bool s2r_power_cut;
/* From prepare() until the SoC is down or the sleep is given up: wakeup events are held */
static bool s2r_going_down;
/* A wakeup event came while they were held */
static bool s2r_wake_held;
/* The SMC has slpa: from here only the power cut of the sleep itself leads back */
static bool s2r_no_return;
/* Display blocks whose coprocessor the display driver stopped for this sleep */
static unsigned long s2r_disp_parked;
/* Display blocks that went down since, which is what starts a stopped one again */
static unsigned long s2r_disp_cycled;
/* The GPU firmware did not start after a sleep that was given up */
static bool s2r_gpu_dead;

/* Wakeup events count again. One that came meanwhile makes the wake a full one. */
static void apple_s2r_release_wakeups(void)
{
	if (!READ_ONCE(s2r_going_down))
		return;

	WRITE_ONCE(s2r_going_down, false);
	pm_wakeup_hold(false);
	if (pm_wakeup_pending())
		WRITE_ONCE(s2r_wake_held, true);
}

/*
 * A sleep given up after slpa leaves the SMC and the PMP waiting for a power
 * cut that never comes. The NVMe, the PCIe ports and the other CPUs do not
 * come back from that, so restart while the disk is still in order rather
 * than run on without one.
 */
static void apple_s2r_check_no_return(void)
{
	if (!READ_ONCE(s2r_no_return))
		return;

	pr_emerg("deep sleep was given up after the point of no return, restarting\n");
	/* In case the restart needs the SMC */
	if (s2r.wdt) {
		writel(0, s2r.wdt + WDT_WD1_CTL);
		writel(0, s2r.wdt + WDT_WD1_CUR);
		writel(5 * WDT_TICKS_PER_S, s2r.wdt + WDT_WD1_BITE);
		writel(WDT_CTL_RESET_EN, s2r.wdt + WDT_WD1_CTL);
	}
	emergency_restart();
}

static int apple_s2r_begin(suspend_state_t state)
{
	struct platform_device *pdev;
	struct device_node *np;
	int ret;

	s2r.smc = NULL;
	s2r.smc_pdev = NULL;
	s2r.smc_announced = false;
	s2r.slpa_sent = false;
	s2r.smc_ap_idle_sent = false;
	WRITE_ONCE(s2r_power_cut, false);
	WRITE_ONCE(s2r_wake_held, false);
	WRITE_ONCE(s2r_no_return, false);
	s2r_disp_parked = 0;
	s2r_disp_cycled = 0;

	/* Also called for s2idle, which never gets to finish() and its waka */
	if (state != PM_SUSPEND_MEM)
		return 0;

	apple_s2r_ps_late_map();
	apple_s2r_ps_late_save();
	apple_s2r_psall_save();

	s2r.again = 0;
	apple_s2r_pmgr_map();

	/* Before apple_wdt stops it, so this is what user space asked for */
	if (s2r.wdt) {
		wdt_saved_ctl = readl(s2r.wdt + WDT_WD1_CTL);
		wdt_saved_bite = readl(s2r.wdt + WDT_WD1_BITE);
		wdt_saved = true;
	}

	np = of_find_compatible_node(NULL, NULL, "apple,sep");
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	s2r.sep_bound = pdev && device_is_bound(&pdev->dev);
	if (pdev)
		put_device(&pdev->dev);
	/* apple_sep woke the enclave since: DEV_DISABLE is clear again and it sleeps for real */
	if (s2r.sep_bound && s2r_sep_down && s2r.sep_ps &&
	    !(readl(s2r.sep_ps) & S2R_PS_DEV_DISABLE)) {
		pr_debug("SEP is back since the last sleep, the PMGR waits for its power gate again\n");
		s2r_sep_down = false;
	}
	if (s2r.sep_bound && s2r_sep_down) {
		pr_info("SEP is still down from the last sleep, the PMGR does not wait for it\n");
		s2r.sep_bound = false;
	}

	/* suspend_again() needs the SMC events to tell a dark wake from a full one */
	np = of_find_compatible_node(NULL, NULL, "apple,smc");
	s2r.smc_pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (s2r.smc_pdev)
		s2r.smc = platform_get_drvdata(s2r.smc_pdev);
	if (s2r.smc && !s2r.smc_nb_registered) {
		s2r.smc_nb.notifier_call = apple_s2r_smc_event;
		if (!blocking_notifier_chain_register(&s2r.smc->event_handlers, &s2r.smc_nb))
			s2r.smc_nb_registered = true;
	}

	/* end() runs after a failed begin() too, and drops the SMC */
	if (!s2r.smc) {
		pr_err("no SMC, not suspending\n");
		return -ENODEV;
	}
	ret = apple_s2r_smc_write(S2R_SMC_SLPW);
	/* A timed out write may still have reached the SMC, end() then owes it a waka */
	if (!ret || ret == -ETIMEDOUT)
		s2r.smc_announced = true;
	if (ret)
		return ret;
	pr_debug("sent slpw\n");

	/* macOS tells the PMP right after slpw, long before its own tail writes 1 */
	s2r.ptd_noticed = false;
	if (s2r.ptd) {
		s2r.ptd_sleep_saved = readl(s2r.ptd + PTD_SLEEP);
		writel(4, s2r.ptd + PTD_SLEEP);
		s2r.ptd_noticed = true;
		pr_debug("ptd_notice: %#llx %#x -> 4\n", T6000_PTD_BASE + PTD_SLEEP,
			 s2r.ptd_sleep_saved);
	}

	return 0;
}

/*
 * The PMP device requests gfx_down() and disp_down() dropped, and whether a
 * sleep that is given up still has the two to undo, see apple_s2r_disp_undo()
 */
static u64 s2r_ptd_dropped;
static bool s2r_disp_undo_due;

/*
 * The GPU fabric port as macOS leaves it for S2R: AFR and GFX up, the gate
 * block set up, PTD GFX request raised and dropped, the +0x8014/+0x82bc
 * handshake, GFX and AFR down. Linux otherwise reaches GO with GFX 0xf000b00.
 */
static void apple_s2r_gfx_down(void)
{
	void __iomem *gfx = apple_s2r_ps(0x28e5803c0);
	void __iomem *blk = s2r.gfx_blk;
	unsigned int j;
	u64 q;
	u32 v;

	s2r.gfx_stuck = 0;
	if (!gfx || !blk || !s2r.afr_ps || !s2r.ptd)
		return;
	s2r.gfx_seen[0] = readl(gfx);
	s2r.gfx_seen[1] = readl(s2r.afr_ps);
	/* macOS powers the AFR cluster (word 5) up first: 0xf, 0x2f, then 0x28e5e4000 = 1 */
	if (s2r.cluster_ps) {
		s2r_afr_cl_pre = readl(s2r.cluster_ps + 4 * 5);
		s2r_afr_cl_valid = true;
		s2r_afr_cl_restored = false;
		writel(0xf, s2r.cluster_ps + 4 * 5);
		/* reads back 0x2f once up (hv read trace 1/11 19:45) */
		if (readl_poll_timeout_atomic(s2r.cluster_ps + 4 * 5, v, v & 0x20, 1, 50000))
			s2r.gfx_stuck |= BIT(6);
		writel(0x2f, s2r.cluster_ps + 4 * 5);
	}
	if (s2r.afr_gate)
		writel(1, s2r.afr_gate);
	writel(0xf040000, s2r.afr_ps);
	writel(0xf04000f, s2r.afr_ps);
	writel(0x1f04000f, s2r.afr_ps);
	if (readl_poll_timeout_atomic(s2r.afr_ps, v, FIELD_GET(PS_ACTUAL, v), 1, 50000))
		s2r.gfx_stuck |= BIT(0);
	writel(0xf000000, gfx);
	writel(0xf00000f, gfx);
	writel(0x1f00000f, gfx);
	/*
	 * GFX reaches 4 and the PMP drops it to auto/min 0 unless the PTD GFX
	 * request is up (hv read trace 1/11 19:45), so raise that first.
	 */
	q = readq(s2r.ptd + PTD_PS_REQ_RD);
	s2r_ptd_dropped |= q & 0x200;
	writeq(q | 0x200, s2r.ptd + PTD_PS_REQ_WR);
	if (readq_poll_timeout_atomic(s2r.ptd + PTD_PS_ACK_RD, q, q & 0x200, 10, 200000))
		s2r.gfx_stuck |= BIT(2);
	if (readl_poll_timeout_atomic(gfx, v, FIELD_GET(PS_ACTUAL, v), 1, 50000)) {
		/* The gate block is in the GFX domain, never touch it unpowered */
		s2r.gfx_stuck |= BIT(1);
	} else {
		for (j = 0; j < ARRAY_SIZE(gfx_init); j++)
			writel(gfx_init[j][1], blk + gfx_init[j][0]);
	}
	q = readq(s2r.ptd + PTD_PS_REQ_RD);
	writeq(q & ~0x200ULL, s2r.ptd + PTD_PS_REQ_WR);
	if (readq_poll_timeout_atomic(s2r.ptd + PTD_PS_ACK_RD, q, !(q & 0x200), 10, 200000))
		s2r.gfx_stuck |= BIT(3);
	if (!(s2r.gfx_stuck & BIT(1)) && FIELD_GET(PS_ACTUAL, readl(gfx))) {
		writel(1, blk + 0x8014);
		writel(1, blk + 0x82bc);
		writel(2, blk + 0x82bc);
		writel(4, blk + 0x82bc);
	} else {
		s2r.gfx_stuck |= BIT(7);
	}
	writel(0xf0000ff, gfx);
	writel(0xf0003f0, gfx);
	if (readl_poll_timeout_atomic(gfx, v, !FIELD_GET(PS_ACTUAL, v), 1, 10000))
		s2r.gfx_stuck |= BIT(4);
	s2r.gfx_seen[2] = v;
	for (j = 0; j < ARRAY_SIZE(gfx_afr_down); j++)
		writel(gfx_afr_down[j], s2r.afr_ps);
	if (readl_poll_timeout_atomic(s2r.afr_ps, v, !FIELD_GET(PS_ACTUAL, v), 1, 10000))
		s2r.gfx_stuck |= BIT(5);
	s2r.gfx_seen[3] = v;
	if (s2r.afr_gate)
		writel(0, s2r.afr_gate);
	if (s2r.cluster_ps)
		writel(0, s2r.cluster_ps + 4 * 5);
	pr_debug("gfx_down: stuck %#x, gfx %#x -> %#x, afr %#x -> %#x\n", s2r.gfx_stuck,
		 s2r.gfx_seen[0], s2r.gfx_seen[2], s2r.gfx_seen[1], s2r.gfx_seen[3]);
}

/*
 * Display blocks down in macOS's order (disp0, dispext0, dispext1) while the
 * PMP still runs, each first dropped from the PMP SOC-DEV-PS-REQ (bits 12, 13,
 * 14) and acked, as macOS steps 0x28e3d07c0 0x7000 -> 0x6000 -> 0x4000 -> 0
 * (hv trace 1/11 18:05). The quiesce can then take the DISPLAY cluster down
 * as well.
 */
/* The cpu0 and fe power states of dispext0, disp0 and dispext1 */
static const u64 s2r_disp_ps[6] = { 0x28e080280, 0x28e080258, 0x28e580350,
				    0x28e580328, 0x28e0802a8, 0x28e080260 };
#define S2R_DISP_ALL	GENMASK(2, 0)

static void apple_s2r_disp_down(unsigned long blocks, bool gfx)
{
	/*
	 * macOS order and values per block: cpu0 0xf0400ff, 0xf0403f0, the
	 * fabric handshake +0x8014 = 1, +0x82bc = 1, 2, 4, then fe 0x3f0.
	 * The dispext0 handshake sets 0x2922c0044 bit 2, which the PMGR S2R
	 * engine 1 waits for after GO (hv trace 1/11).
	 */
	static const u8 order_early[6] = { 2, 3, 0, 1, 4, 5 };
	static const u64 ptd_bit[3] = { 0x2000, 0x1000, 0x4000 };
	unsigned int i, k;
	u32 v;
	u64 q;

	s2r_ptd_dropped = 0;
	s2r_disp_cycled |= blocks;
	if (gfx)
		apple_s2r_gfx_down();
	s2r.disp_stuck = 0;
	for (k = 0; k < ARRAY_SIZE(s2r_disp_ps); k++) {
		void __iomem *reg, *fe;
		void __iomem *hs;

		i = order_early[k];
		if (!(blocks & BIT(i / 2)))
			continue;
		reg = apple_s2r_ps(s2r_disp_ps[i]);
		hs = s2r.disp_hs[i / 2];

		if (!reg)
			continue;
		/*
		 * The fabric block is only there while its fe is powered: a write
		 * to it otherwise is an SError.
		 */
		fe = apple_s2r_ps(s2r_disp_ps[i | 1]);
		if (hs && (!fe || !FIELD_GET(PS_ACTUAL, readl(fe))))
			hs = NULL;
		if (!(i & 1) && hs) {
			const u32 (*t)[2] = i == 2 ? disp_init_38c : disp_init_28a;
			unsigned int n = i == 2 ? ARRAY_SIZE(disp_init_38c) :
						  ARRAY_SIZE(disp_init_28a);
			unsigned int j;

			for (j = 0; j < n; j++)
				writel(t[j][1], hs + t[j][0]);
		}
		if (!(i & 1) && s2r.ptd) {
			q = readq(s2r.ptd + PTD_PS_REQ_RD);
			s2r_ptd_dropped |= q & ptd_bit[i / 2];
			writeq(q & ~ptd_bit[i / 2], s2r.ptd + PTD_PS_REQ_WR);
			if (readq_poll_timeout_atomic(s2r.ptd + PTD_PS_ACK_RD, q,
						      !(q & ptd_bit[i / 2]), 10, 200000))
				s2r.disp_stuck |= BIT(8 + i / 2);
		}
		if (!(i & 1)) {
			writel(0xf0400ff, reg);
			writel(0xf0403f0, reg);
			if (readl_poll_timeout_atomic(reg, v, !FIELD_GET(PS_ACTUAL, v), 1, 10000))
				s2r.disp_stuck |= BIT(i);
			s2r.disp_read[i] = readl(reg);
			continue;
		}
		if (hs) {
			writel(1, hs + 0x8014);
			writel(1, hs + 0x82bc);
			writel(2, hs + 0x82bc);
			writel(4, hs + 0x82bc);
		}
		writel(0x3f0, reg);
		if (readl_poll_timeout_atomic(reg, v, !FIELD_GET(PS_ACTUAL, v), 1, 10000))
			s2r.disp_stuck |= BIT(i);
		s2r.disp_read[i] = readl(reg);
	}
	pr_debug("disp_down: stuck %#x, ps %#x %#x %#x %#x %#x %#x\n", s2r.disp_stuck,
		 s2r.disp_read[0], s2r.disp_read[1], s2r.disp_read[2], s2r.disp_read[3],
		 s2r.disp_read[4], s2r.disp_read[5]);
}

/*
 * A sleep given up after disp_down() and before the SoC went down: a display
 * block that did not power down, an SMC write that failed, a CPU that did not
 * go offline, a wakeup event, or the finisher giving up before the quiesce.
 * wake() and finish() put back what enter() and a real sleep change, so undo
 * the rest first. A running PMP keeps a block down until its device request
 * is up again, a stopped one gets its requests back when it starts. The AFR
 * cluster word goes back as in quiesce_restore(), and the ps_all restore in
 * wake() then raises every power state to its level at begin().
 */
static void apple_s2r_disp_undo(void)
{
	u64 q;
	u32 v;

	s2r_disp_undo_due = false;
	if (s2r.ptd && s2r_ptd_dropped && !s2r.pmp_idled) {
		q = readq(s2r.ptd + PTD_PS_REQ_RD);
		writeq(q | s2r_ptd_dropped, s2r.ptd + PTD_PS_REQ_WR);
		if (readq_poll_timeout_atomic(s2r.ptd + PTD_PS_ACK_RD, q,
					      (q & s2r_ptd_dropped) == s2r_ptd_dropped, 10, 200000))
			pr_warn("PMP did not take device requests %#llx back\n", s2r_ptd_dropped);
	}
	if (s2r.cluster_ps && s2r_afr_cl_valid && !s2r_afr_cl_restored) {
		void __iomem *reg = s2r.cluster_ps + 4 * CLUSTER_AFR;

		if ((readl(reg) ^ s2r_afr_cl_pre) & CLUSTER_PS_TARGET)
			writel(s2r_afr_cl_pre, reg);
		readl_poll_timeout_atomic(reg, v, !(v & CLUSTER_PS_BUSY), 1, 10000);
		s2r_afr_cl_valid = false;
	}
	s2r_psall_armed = s2r_psall_n;
	pr_info("sleep given up, powering the displays back up\n");
}

/* Which display block @np sits in, by the power states it hangs off */
static void apple_s2r_display_block(struct device_node *np, unsigned int depth)
{
	struct of_phandle_iterator it;
	unsigned int k;
	int err, i;

	of_for_each_phandle(&it, err, np, "power-domains", "#power-domain-cells", 0) {
		i = apple_s2r_psall_find(it.node);
		for (k = 0; i >= 0 && k < ARRAY_SIZE(s2r_disp_ps); k++)
			if (s2r_psall[i].pa == s2r_disp_ps[k])
				set_bit(k / 2, &s2r_disp_parked);
		/* A coprocessor's own domain is its PMP report entry, one step above */
		if (i < 0 && depth < 2)
			apple_s2r_display_block(it.node, depth + 1);
	}
}

/**
 * apple_s2r_display_parked() - a display coprocessor was stopped for deep sleep
 * @np: the coprocessor's device node
 *
 * A stopped coprocessor only starts again once its block lost power. Should
 * the sleep be given up before the blocks go down, apple_s2r_finish() takes
 * the ones named here down and up, ahead of the display driver's resume.
 */
void apple_s2r_display_parked(struct device_node *np)
{
	apple_s2r_display_block(np, 0);
}
EXPORT_SYMBOL_GPL(apple_s2r_display_parked);

/*
 * A sleep given up after the display driver stopped its coprocessors and
 * before their blocks went down: take those blocks down and up, so that the
 * coprocessors start again in the driver's resume.
 */
static void apple_s2r_restart_displays(void)
{
	unsigned long blocks = s2r_disp_parked & ~s2r_disp_cycled;
	unsigned long seen[BITS_TO_LONGS(S2R_PSALL_MAX)];
	unsigned int i, k;

	if (READ_ONCE(s2r_power_cut) || !blocks)
		return;

	apple_s2r_disp_down(blocks, false);
	apple_s2r_disp_undo();
	/* Only these blocks' power states, the rest is where the devices have it */
	s2r_psall_armed = false;
	s2r_psall_pass = 0;
	bitmap_zero(seen, S2R_PSALL_MAX);
	for (k = 0; k < ARRAY_SIZE(s2r_disp_ps); k++)
		for (i = 0; (blocks & BIT(k / 2)) && i < s2r_psall_n; i++)
			if (s2r_psall[i].pa == s2r_disp_ps[k])
				apple_s2r_psall_on(i, 0, seen);
}

static u64 s2r_dvfs_saved[4];

/*
 * macOS, between the last device power-down and the PMP's IOP sleep: the last
 * PTD writes, SOC-DEV-PS-REQ 0, the DVFS words idle and the sleep word 1
 */
static void apple_s2r_pmp_tail(void)
{
	u64 v;
	int i, n;

	s2r.ptd_written = false;
	if (!s2r.ptd)
		return;
	s2r.ptd_req_saved = readq(s2r.ptd + PTD_PS_REQ_RD);
	writeq(0, s2r.ptd + PTD_PS_REQ_WR);
	s2r.ptd_ack_ret = readq_poll_timeout(s2r.ptd + PTD_PS_ACK_RD, v, !(u32)v, 1000, 200000);
	s2r.ptd_ack_seen = v;
	for (i = 0; i < 4; i++)
		s2r_dvfs_saved[i] = readq(s2r.ptd + PTD_DVFS_RD(i));
	for (n = 0; n < 2; n++)
		for (i = 0; i < 4; i++)
			writeq(PTD_DVFS_IDLE, s2r.ptd + PTD_DVFS_WR(i));
	writel(1, s2r.ptd + PTD_SLEEP);
	for (i = 0; i < 3; i++)
		writeq(PTD_DVFS_IDLE, s2r.ptd + PTD_DVFS_WR(i));
	s2r.ptd_written = true;
}

/*
 * Idle and stop the PMP once the devices are suspended and slpa went out, and
 * restart it before they resume, as ApplePMPv2's sleep action does
 */
static void apple_s2r_pmp_set(bool sleep)
{
	int ret;

	if (!sleep && !s2r.pmp_idled)
		return;
	if (sleep)
		apple_s2r_pmp_tail();
	ret = apple_s2r_pmp_op(sleep ? APPLE_S2R_PMP_STOP : APPLE_S2R_PMP_START);
	if (!sleep && s2r.ptd_written) {
		/*
		 * The PMP report driver writes its device requests (displays...)
		 * back when the PMP starts: only add the saved ones, never clear.
		 */
		u64 cur = readq(s2r.ptd + PTD_PS_REQ_RD);
		u32 sleep_word;

		if (s2r.ptd_req_saved & ~cur)
			writeq(cur | s2r.ptd_req_saved, s2r.ptd + PTD_PS_REQ_WR);
		sleep_word = readl(s2r.ptd + PTD_SLEEP);
		pr_debug("pmp_tail: PS-REQ %#llx -> 0, ACK %#llx (%d), REQ now %#llx, sleep word %#x\n",
			 s2r.ptd_req_saved, s2r.ptd_ack_seen, s2r.ptd_ack_ret, cur, sleep_word);
		/*
		 * Otherwise the PMP keeps the fabric clocks at their idle source
		 * and the apcie DART is unreachable
		 */
		if (!ret) {
			int i;

			for (i = 0; i < 4; i++)
				writeq(s2r_dvfs_saved[i], s2r.ptd + PTD_DVFS_WR(i));
			/* give the PMP a moment to move the clocks */
			mdelay(10);
			pr_debug("pmp_tail: DVFS words back to %#llx %#llx %#llx %#llx\n",
				 s2r_dvfs_saved[0], s2r_dvfs_saved[1], s2r_dvfs_saved[2],
				 s2r_dvfs_saved[3]);
		}
	}
	/* A partial idle still needs the wake, which skips a running PMP */
	s2r.pmp_idled = sleep && ret != -ENODEV;
}

/*
 * The AOP AFK endpoints 0x20-0x2b get a queue stop before the AP power 0x201
 * and a queue start after the AP power 0x20, as in macOS
 * (AFKMailboxSharedMemoryEndpoint::_setQueueState)
 */
#define S2R_AOP_AFK_EP		0x20
#define S2R_AOP_AFK_EPS		12
#define S2R_AOP_AFK_QUEUE_STOP	0xa4ULL
#define S2R_AOP_AFK_QUEUE_START	0xa3ULL

/* Set by the AOP module, which may come and go */
static DEFINE_MUTEX(s2r_aop_lock);
static struct apple_rtkit *s2r_aop_rtk;
static bool s2r_aop_slept;
/* The endpoints whose queues were stopped, started again on wake */
static u16 s2r_aop_stopped;
static_assert(S2R_AOP_AFK_EPS <= 16);

void apple_s2r_set_aop_rtkit(struct apple_rtkit *rtk)
{
	guard(mutex)(&s2r_aop_lock);
	s2r_aop_rtk = rtk;
	s2r_aop_slept = false;
	s2r_aop_stopped = 0;
}
EXPORT_SYMBOL_GPL(apple_s2r_set_aop_rtkit);

/* Sends @op to the endpoints in @eps, returns those that took it */
static u16 apple_s2r_aop_afk_queues(u64 op, u16 eps)
{
	unsigned int i;
	u16 sent = 0;
	int ret;

	for (i = 0; i < S2R_AOP_AFK_EPS; i++) {
		if (!(eps & BIT(i)))
			continue;
		ret = apple_rtkit_send_message(s2r_aop_rtk, S2R_AOP_AFK_EP + i,
					       op << 48, NULL, false);
		if (!ret)
			sent |= BIT(i);
		pr_debug("AOP AFK ep %#x %#llx: %d\n", S2R_AOP_AFK_EP + i, op, ret);
	}
	return sent;
}

/*
 * Tell the AOP the AP goes to sleep (RTKit AP power 0x201) before the PMP idles
 * and that it is on (0x20) after the PMP restarts, as macOS
 * RTBuddy::_signalApStateGated does for an IOP that stays up through sleep.
 * The AFK queues stop before the 0x201 and start after the 0x20; the ones
 * that stopped start again on wake even when the 0x201 failed.
 */
static void apple_s2r_aop_ap_power(bool on)
{
	int ret;

	guard(mutex)(&s2r_aop_lock);
	if (on && !s2r_aop_slept && !s2r_aop_stopped)
		return;
	if (!s2r_aop_rtk) {
		pr_info("no AOP for the AP power state\n");
		return;
	}
	if (!on) {
		s2r_aop_stopped = apple_s2r_aop_afk_queues(S2R_AOP_AFK_QUEUE_STOP,
							   GENMASK(S2R_AOP_AFK_EPS - 1, 0));
		msleep(50);
		ret = apple_rtkit_set_ap_power(s2r_aop_rtk, false);
		s2r_aop_slept = !ret;
		pr_debug("AOP AP power 0x201: %d\n", ret);
		return;
	}

	/* An ON only after an acked 0x201 */
	if (s2r_aop_slept) {
		ret = apple_rtkit_set_ap_power(s2r_aop_rtk, true);
		s2r_aop_slept = false;
		pr_debug("AOP AP power 0x20: %d\n", ret);
	}
	/* The stopped queues stay stopped until told otherwise */
	apple_s2r_aop_afk_queues(S2R_AOP_AFK_QUEUE_START, s2r_aop_stopped);
	s2r_aop_stopped = 0;
}

static DEFINE_MUTEX(s2r_gpu_lock);
static struct apple_rtkit *s2r_gpu_rtk;
static bool s2r_gpu_slept;
static bool s2r_gpu_parked;
static void __iomem *s2r_gpu_asc;

#define S2R_GPU_ASC_CPU_CONTROL	0x44
#define S2R_GPU_ASC_CPU_STATUS	0x48
#define S2R_GPU_ASC_RUN		BIT(4)

static const struct of_device_id s2r_agx_ids[] = {
	{ .compatible = "apple,agx-g13x" },
	{ .compatible = "apple,agx-g13s" },
	{ }
};

/* The GPU ASC is up whenever its firmware answers: map it once, keep it */
static void __iomem *apple_s2r_gpu_asc(void)
{
	struct device_node *np;

	if (s2r_gpu_asc)
		return s2r_gpu_asc;
	np = of_find_matching_node(NULL, s2r_agx_ids);
	/* reg 0 is the ASC, its mailbox at +0x8000 */
	s2r_gpu_asc = np ? of_iomap(np, 0) : NULL;
	of_node_put(np);
	return s2r_gpu_asc;
}

static void apple_s2r_gpu_park(void)
{
	void __iomem *asc = apple_s2r_gpu_asc();
	u32 ctrl = 0, st = 0;
	int ret, iop = -ENODEV;

	if (!asc) {
		pr_err("GPU park: no GPU ASC\n");
		return;
	}
	if (s2r_gpu_dead) {
		/* Nothing to tell it: the power loss ahead is what starts it again */
		ret = 0;
		iop = 0;
	} else {
		ret = apple_rtkit_set_ap_power(s2r_gpu_rtk, false);
		s2r_gpu_slept = !ret;
		if (!ret)
			iop = apple_rtkit_idle_iop(s2r_gpu_rtk);
	}
	if (!ret && !iop) {
		apple_rtkit_detach_keep(s2r_gpu_rtk);
		ctrl = readl_relaxed(asc + S2R_GPU_ASC_CPU_CONTROL);
		st = readl_relaxed(asc + S2R_GPU_ASC_CPU_STATUS);
		writel_relaxed(ctrl & ~S2R_GPU_ASC_RUN, asc + S2R_GPU_ASC_CPU_CONTROL);
		s2r_gpu_parked = true;
	}
	pr_debug("GPU park: AP IDLE %d, IOP IDLE %d, cpu control %#x status %#x\n",
		 ret, iop, ctrl, st);
}

static void apple_s2r_gpu_unpark(void)
{
	void __iomem *asc = s2r_gpu_asc;
	u32 ctrl, st, now;
	int ret;

	s2r_gpu_parked = false;
	s2r_gpu_slept = false;
	ctrl = readl_relaxed(asc + S2R_GPU_ASC_CPU_CONTROL);
	st = readl_relaxed(asc + S2R_GPU_ASC_CPU_STATUS);
	ret = apple_rtkit_reinit_keep(s2r_gpu_rtk);
	if (!ret) {
		writel_relaxed(ctrl | S2R_GPU_ASC_RUN, asc + S2R_GPU_ASC_CPU_CONTROL);
		ret = apple_rtkit_wake_keep_ap(s2r_gpu_rtk);
	}
	if (!ret)
		ret = apple_rtkit_restart_started_eps(s2r_gpu_rtk);
	if (!ret)
		ret = apple_rtkit_ap_power_timeout(s2r_gpu_rtk, true, 2000);
	now = readl_relaxed(asc + S2R_GPU_ASC_CPU_STATUS);
	s2r_gpu_dead = ret;
	if (ret)
		pr_err("the GPU firmware did not start again (%d)%s\n", ret,
		       READ_ONCE(s2r_power_cut) ? "" :
		       ": the sleep was given up, the next sleep or a reboot brings it back");
	pr_debug("GPU unpark: %d (cpu control was %#x status %#x, status now %#x)\n",
		 ret, ctrl, st, now);
}

void apple_s2r_set_gpu_rtkit(struct apple_rtkit *rtk)
{
	guard(mutex)(&s2r_gpu_lock);
	s2r_gpu_rtk = rtk;
	s2r_gpu_slept = false;
}
EXPORT_SYMBOL_GPL(apple_s2r_set_gpu_rtkit);

/*
 * Park the GPU firmware as the DCPs: AP and IOP IDLE, clear RUN on its ASC; on
 * wake set RUN, IOP INIT, restart its endpoints, AP ON
 */
static void apple_s2r_gpu_ap_power(bool on)
{
	int ret;

	guard(mutex)(&s2r_gpu_lock);
	if (on && s2r_gpu_parked) {
		apple_s2r_gpu_unpark();
		return;
	}
	if (on && !s2r_gpu_slept)
		return;
	if (!s2r_gpu_rtk) {
		pr_info("no GPU firmware for the AP power state\n");
		return;
	}
	if (!on) {
		apple_s2r_gpu_park();
		return;
	}
	ret = apple_rtkit_set_ap_power(s2r_gpu_rtk, on);
	s2r_gpu_slept = !on && !ret;
	pr_debug("GPU AP power %s: %d\n", on ? "0x20" : "0x201", ret);
}

static int apple_s2r_prepare(void)
{
	s2r.attempts_at_prepare = s2r.attempts;
	s2r.last_ret = 0;
	s2r.smc_wake_reason = 0;
	s2r.counter_before = 0;
	s2r.counter_after = 0;
	s2r.resumes_before = 0;
	s2r.resumes_after = 0;
	s2r.wake_isr = 0;
	s2r.wake_ipi_sr = 0;
	s2r.wake_pmcr0 = 0;
	s2r.pmgr_written = false;
	s2r.pmgr_woke = false;
	s2r.sep_pg_written = false;
	/*
	 * The GPU firmware stops below and the display ones in the late phase,
	 * and none of them starts again without the power cut. A wakeup event
	 * from here on is kept for after the sleep.
	 */
	WRITE_ONCE(s2r_going_down, true);
	pm_wakeup_hold(true);
	/*
	 * Here and no later: the GPU's power domain is off once the noirq phase
	 * is through, and its mailbox with it. The firmware does not start
	 * again without a power loss (GFX-ASC has no power state of its own).
	 */
	apple_s2r_gpu_ap_power(false);
	apple_s2r_aop_ap_power(false);

	return 0;
}

static void apple_s2r_smc_gpio(bool off);

/* Runs after dpm_suspend_noirq(), the last point where the SMC still answers */
static int apple_s2r_prepare_late(void)
{
	int ret;

	apple_s2r_pmgr_log("before sleep");

	/* Read only: macOS polls the actual level and never writes the SEP PS */
	if (s2r.sep_ps) {
		u32 first = readl(s2r.sep_ps), v = first;
		unsigned int us = 0;

		while (FIELD_GET(PS_ACTUAL, v) && us < S2R_SEP_WAIT_MS * 1000) {
			udelay(100);
			us += 100;
			v = readl(s2r.sep_ps);
		}
		pr_debug("sep PS: %#x, then %#x after %u us\n", first, v, us);
		if (FIELD_GET(PS_ACTUAL, v)) {
			pr_err("the SEP did not power-gate, not sleeping\n");
			return -EBUSY;
		}
	}

	/* macOS: the displays go down through the PMP before gP1a and slpa */
	s2r_disp_undo_due = true;
	apple_s2r_disp_down(S2R_DISP_ALL, true);
	if (s2r.disp_stuck) {
		/* By disp_hs[]: the pairs of disp_down()'s power states */
		static const char * const names[] = { "dispext0", "disp0", "dispext1" };
		unsigned int d;

		for (d = 0; d < ARRAY_SIZE(names) - 1; d++)
			if (s2r.disp_stuck & (BIT(2 * d) | BIT(2 * d + 1) | BIT(8 + d)))
				break;
		/* Never sleep around a display block that is still up */
		pr_err("%s did not power down (%#x), not sleeping\n", names[d], s2r.disp_stuck);
		return -EBUSY;
	}

	if (s2r.smc)
		apple_s2r_smc_gpio(true);

	/*
	 * AppleSMC's last sleep action, once everything else is idle. The
	 * SMC mailbox IRQ is NO_SUSPEND and the reply completes in its
	 * handler. pm_test never powers the AP down, so no slpa then.
	 */
	if (s2r.smc_announced) {
		if (apple_s2r_pm_test()) {
			pr_info("pm_test is on, not sending slpa\n");
		} else {
			/*
			 * macOS writes these between slpw and slpa. A failed
			 * write may still have reached the SMC.
			 */
			static const u8 pmvc[4] = { 0x00, 0x01, 0x00, 0x00 };
			u8 wktp = 2;

			ret = apple_s2r_smc_write_key(S2R_SMC_PMVC, pmvc, sizeof(pmvc));
			if (!ret)
				ret = apple_s2r_smc_write_key(S2R_SMC_WKTP, &wktp, 1);
			if (ret)
				return ret;
			pr_debug("wrote pmVC and WKTP\n");

			apple_s2r_baac_mark();
			s2r.slpa_sent = true;
			WRITE_ONCE(s2r_no_return, true);
			ret = apple_s2r_smc_write(S2R_SMC_SLPA);
			if (ret)
				return ret;
			pr_debug("sent slpa after the noirq phase\n");

			/*
			 * Tell the SMC RTKit the AP is idle, and ON first on wake, as
			 * AppleSMC's sleep/wake action 300 does. A late ack still
			 * flips the state.
			 */
			s2r.smc_ap_idle_sent = true;
			ret = apple_rtkit_set_ap_power(s2r.smc->rtk, false);
			if (ret) {
				pr_err("SMC AP power 0x201 failed: %d\n", ret);
				return ret;
			}
			pr_debug("SMC AP power 0x201 acked\n");
		}
	}

	/* macOS stops the PMP (AP 0x201, then IOP 0x201) only after slpa */
	apple_s2r_pmp_set(true);

	/* No CPU is parked at this point: clear the word cpu_die() waits on */
	smp_store_release(&s2r.release, S2R_RELEASE_NONE);
	WRITE_ONCE(s2r.deep_round, false);
	WRITE_ONCE(s2r.sleeping, true);
	return 0;
}

/* Runs through cpu_suspend(), whose argument it does not use */
static int apple_s2r_finisher(unsigned long arg)
{
	struct s2r_cpu *slot = s2r_cpus[smp_processor_id()].slot;
	unsigned int me = smp_processor_id(), i;

	/*
	 * syscore_suspend() only looks for wakeup events. An armed IRQ that
	 * came in since IRQs were masked would make the WFI return
	 * right after the secondaries powered down for deep sleep. Give up
	 * while they can still take the plain power down path.
	 */
	if (read_sysreg(isr_el1) & (PSR_I_BIT | PSR_F_BIT))
		return -EBUSY;

	/* m1n1 sets the internal PCIe controller up again on the wake path */
	WRITE_ONCE(s2r.mbox->restore, S2R_RESTORE_APCIE);
	WRITE_ONCE(s2r.mbox->restored, 0);
	WRITE_ONCE(slot->entry, __pa_symbol(cpu_resume));
	apple_s2r_mbox_clean();

	/* enter() puts back whatever this wrote, also when giving up here */
	apple_s2r_quiesce();
	if (read_sysreg(isr_el1) & (PSR_I_BIT | PSR_F_BIT))
		return -EBUSY;

	apple_s2r_go_check();

	/* From here on the SoC may start S2R on its own, there is no giving up */
	WRITE_ONCE(s2r.stops_written, false);
	/*
	 * The S2R timer is the last MMIO write, as in macOS: the PMGR stop of
	 * every parked core (quiesceHW step 13) and the EACC cluster target go
	 * before it. A core then stays in WFI until the SoC sleeps.
	 */
	for_each_possible_cpu(i) {
		struct apple_s2r_cpu *c = &s2r_cpus[i];

		if (i == me || !c->slot || !c->pmgr || READ_ONCE(c->state) != S2R_CPU_PARKED)
			continue;
		writel(c->slot->start_val0, c->pmgr + PMGR_CPU_STOP);
	}
	WRITE_ONCE(s2r.stops_written, true);
	apple_s2r_cluster_eacc();
	dsb(sy);

	/* Same dead-man before the raw writes, so a fabric hang in them still resets */
	if (s2r.wdt) {
		writel(0, s2r.wdt + WDT_WD1_CTL);
		writel(0, s2r.wdt + WDT_WD1_CUR);
		writel(S2R_WDT_GO_ARM_S * WDT_TICKS_PER_S, s2r.wdt + WDT_WD1_BITE);
		writel(WDT_CTL_RESET_EN, s2r.wdt + WDT_WD1_CTL);
		dsb(sy);
	}
	apple_s2r_ps_late();
	/* Linux never enables SPMI vectors, macOS has the PMU one on for the RTC alarm */
	if (s2r.spmi_irq) {
		writel(S2R_SPMI_VEC, s2r.spmi_irq + 0x40);
		writel(readl(s2r.spmi_irq) | S2R_SPMI_VEC, s2r.spmi_irq);
		dsb(sy);
	}
	/* Dead-man from here on: it keeps counting in S2R and m1n1 restarts it on a wake */
	if (s2r.wdt) {
		writel(0, s2r.wdt + WDT_WD1_CTL);
		writel(0, s2r.wdt + WDT_WD1_CUR);
		writel(S2R_WDT_GO_ARM_S * WDT_TICKS_PER_S, s2r.wdt + WDT_WD1_BITE);
		writel(WDT_CTL_RESET_EN, s2r.wdt + WDT_WD1_CTL);
		dsb(sy);
	}
	/* macOS startS2RTimer(), right before the CPUs go down */
	if (s2r.s2r_timer)
		writel(1, s2r.s2r_timer);

	WRITE_ONCE(s2r.deep_round, true);
	/* The parked CPUs go down for S2R now, and read stops_written after this */
	smp_store_release(&s2r.release, S2R_RELEASE_SLEEP);
	dsb(sy);
	sev();

	apple_s2r_cluster_eacc();

	return apple_s2r_cpu_sleep(apple_s2r_cyc_ovrd(true), apple_s2r_acc_ovrd_deep(), true);
}

static int apple_s2r_enter(suspend_state_t state)
{
	unsigned int cpu = smp_processor_id();
	struct apple_s2r_cpu *c = &s2r_cpus[cpu];
	u64 acc = read_sysreg_s(SYS_IMP_APL_ACC_OVRD);
	u64 cyc = read_sysreg_s(SYS_IMP_APL_CYC_OVRD);
	int ret, i;

	if (apple_s2r_mpidr(cpu) != s2r.boot_mpidr || !c->slot)
		return -EINVAL;

	s2r.attempts++;

	/* It may still be running, and the SoC cannot sleep around it */
	for_each_possible_cpu(i) {
		if (READ_ONCE(s2r_cpus[i].state) == S2R_CPU_STUCK) {
			pr_err("CPU%u is in an unknown state, not sleeping\n", i);
			s2r.last_ret = -EBUSY;
			return -EBUSY;
		}
	}

	apple_s2r_mbox_inval();
	s2r.resumes_before = READ_ONCE(s2r.mbox->resumes);

	/*
	 * The parked CPUs power down once the finisher releases them. Count
	 * their resets from before that, so that cpu_boot() sees one that
	 * happened during sleep or on wake.
	 */
	for_each_possible_cpu(i)
		if (READ_ONCE(s2r_cpus[i].state) == S2R_CPU_PARKED)
			s2r_cpus[i].resets_at_die = READ_ONCE(s2r_cpus[i].slot->resets);

	WRITE_ONCE(s2r.smc_events, 0);
	if (state == PM_SUSPEND_MEM)
		apple_s2r_pmgr_mask_set();
	/* Unless the SEP has a driver, the PMGR sleeps without waiting for its power gate */
	if (state == PM_SUSPEND_MEM && !apple_s2r_pm_test())
		apple_s2r_sep_pg_clear();
	s2r.quiesce = state == PM_SUSPEND_MEM && !apple_s2r_pm_test();
	s2r.cluster_written = 0;
	s2r.cluster_skipped = 0;
	s2r.cluster_stuck = 0;
	s2r.socq_done = 0;
	s2r.socq_stuck = 0;
	s2r.socq_skipped = 0;
	s2r.mux_n = 0;

	s2r.counter_before = __arch_counter_get_cntpct();

	s2r_psall_armed = s2r_psall_n;
	ret = cpu_suspend(1, apple_s2r_finisher);
	/* Back up, or never down */
	apple_s2r_release_wakeups();
	/* After a real sleep wake() and finish() bring the displays back */
	if (!ret)
		s2r_disp_undo_due = false;

	apple_s2r_wdt_mark();
	apple_s2r_quiesce_restore();
	apple_s2r_sep_pg_restore();
	s2r.counter_after = __arch_counter_get_cntpct();
	/* What woke us, if it was not an IRQ through the AIC */
	s2r.wake_isr = read_sysreg(isr_el1);
	s2r.wake_ipi_sr = read_sysreg_s(SYS_IMP_APL_IPI_SR_EL1);
	s2r.wake_pmcr0 = read_sysreg_s(SYS_IMP_APL_PMCR0_EL1);
	/* Let a parked CPU still waiting go down before the CPUs are brought back */
	smp_store_release(&s2r.release, S2R_RELEASE_OFF);
	dsb(sy);
	sev();
	write_sysreg_s(acc, SYS_IMP_APL_ACC_OVRD);
	write_sysreg_s(cyc, SYS_IMP_APL_CYC_OVRD);
	isb();

	apple_s2r_pmgr_wake();

	apple_s2r_mbox_inval();
	s2r.resumes_after = READ_ONCE(s2r.mbox->resumes);
	if (s2r.resumes_after != s2r.resumes_before)
		WRITE_ONCE(s2r_power_cut, true);
	/* Back through the resume entry: the SoC was down */
	if (!ret || READ_ONCE(s2r_power_cut))
		WRITE_ONCE(s2r_no_return, false);
	apple_s2r_check_no_return();
	s2r_restored = READ_ONCE(s2r.mbox->restored);
	WRITE_ONCE(s2r.mbox->restore, 0);
	WRITE_ONCE(c->slot->entry, 0);
	apple_s2r_mbox_clean();

	/* The parked CPUs went down with us */
	for_each_possible_cpu(i)
		if (READ_ONCE(s2r_cpus[i].state) == S2R_CPU_PARKED)
			WRITE_ONCE(s2r_cpus[i].state, S2R_CPU_OFF);

	s2r.last_ret = ret;

	return ret;
}

/* Before any SMC key access on wake, an idle AP gets every key refused */
static void apple_s2r_smc_ap_on(void)
{
	int i, ret = 0;

	if (!s2r.smc_ap_idle_sent)
		return;

	for (i = 0; i < 3; i++) {
		ret = apple_rtkit_set_ap_power(s2r.smc->rtk, true);
		if (!ret) {
			s2r.smc_ap_idle_sent = false;
			pr_debug("SMC AP power ON acked\n");
			return;
		}
	}
	pr_err("SMC AP power ON failed: %d\n", ret);
}

static void apple_s2r_wake(void)
{
	/* Given up between slpa and enter() */
	apple_s2r_check_no_return();
	apple_s2r_smc_ap_on();
	apple_s2r_ps_late_put_back();
	if (s2r_disp_undo_due)
		apple_s2r_disp_undo();
	apple_s2r_psall_restore();

	/* After the CPUs are back, see suspend_enter() */
	WRITE_ONCE(s2r.deep_round, false);
	WRITE_ONCE(s2r.sleeping, false);
	/* No CPU is parked any more: clear the word for the next round */
	smp_store_release(&s2r.release, S2R_RELEASE_NONE);
}

/* SMC GPIO keys gPxx: macOS writes 0 to power a pin off, gpio-macsmc writes CMD_OUTPUT | 1 */
static unsigned long s2r_gpio_lowered;

static void apple_s2r_smc_gpio(bool off)
{
	unsigned long pins = off ? S2R_SMC_GPIO_OFF : s2r_gpio_lowered;
	unsigned int i;

	if (!off && !s2r_gpio_lowered)
		return;
	for_each_set_bit(i, &pins, BITS_PER_LONG) {
		smc_key key = __SMC_KEY('g', 'P', hex_asc_hi(i), hex_asc_lo(i));
		__le32 v = cpu_to_le32(off ? 0 : (1 << 24) | 1);

		if (apple_s2r_smc_write_key(key, &v, sizeof(v)))
			continue;
		if (off)
			__set_bit(i, &s2r_gpio_lowered);
		pr_debug("SMC %p4ch = %s\n", &key, off ? "0 (off)" : "on");
	}
	if (!off)
		s2r_gpio_lowered = 0;
}

/*
 * The SMC wakes the AP for every event it has to send. For the charger, or
 * the lid closing, macOS handles the event in a dark wake and sleeps again
 * with the display off. After slpa this sleeps again the same way when the
 * SMC woke the AP only for the charger or a plug (MBSW charger, acattach,
 * acdetach, USB-C_plug, USB2_plug) or for the lid while it is closed, with
 * a pause between waka and the slpw of the dark wake.
 */
#define S2R_DARK_WAKE_GAP_MS	300
#define S2R_MBSW_LID		BIT_ULL(8)
/*
 * charger, acattach, acdetach, and USB-C_plug and USB2_plug, which the
 * charger's port also raises
 */
#define S2R_MBSW_CHARGER	(BIT_ULL(9) | BIT_ULL(40) | BIT_ULL(41) | BIT_ULL(42) | BIT_ULL(43))
static u64 s2r_wake_mbsw;
static bool s2r_wake_mbsw_ok;

static bool apple_s2r_dark_wake(void)
{
	unsigned int t;
	u8 lid = 0;

	if (!s2r_wake_mbsw_ok || !s2r_wake_mbsw ||
	    (s2r_wake_mbsw & ~(S2R_MBSW_CHARGER | S2R_MBSW_LID)))
		return false;
	if (!(s2r_wake_mbsw & S2R_MBSW_LID)) {
		bool ac = apple_s2r_ac_read(), was_ac = s2r_ac_online;

		/*
		 * Power arriving with the lid closed is someone docking the machine:
		 * it has no other way to wake, as external keyboards are off in sleep.
		 * The plug bits also come with unplugging, so go by the AC state.
		 */
		apple_s2r_smc_read(SMC_KEY(MSLD), &lid, sizeof(lid));
		/* The charger's contract can come a moment after the plug event */
		for (t = 0; !ac && !was_ac && lid && t < 30; t++) {
			msleep(100);
			ac = apple_s2r_ac_read();
		}
		s2r_ac_online = ac;
		pr_debug("dark wake: ac %d, was %d, lid closed %d\n", ac, was_ac, lid);
		return !(ac && !was_ac && lid);
	}
	/* Opening the lid wakes for real, closing it does not */
	return !apple_s2r_smc_read(SMC_KEY(MSLD), &lid, sizeof(lid)) && lid;
}

static void apple_s2r_mbsw_log(u64 mbsw)
{
	u32 words[2] = { lower_32_bits(mbsw), upper_32_bits(mbsw) };
	char bits[160];

	apple_s2r_bits(bits, sizeof(bits), words, ARRAY_SIZE(words), apple_s2r_mbsw_bits,
		       ARRAY_SIZE(apple_s2r_mbsw_bits));
	pr_debug("SMC MBSW %016llx:%s\n", mbsw, bits);
}

static void apple_s2r_smc_wake(void)
{
	__le32 mbse;
	__le64 mbsw;
	int ret;

	s2r_wake_mbsw_ok = false;
	apple_s2r_smc_ap_on();
	apple_s2r_smc_gpio(false);

	if (!s2r.smc_announced)
		return;

	apple_s2r_baac_report();
	ret = apple_s2r_smc_read(SMC_KEY(MBSE), &mbse, sizeof(mbse));
	if (!ret) {
		s2r.smc_wake_reason = le32_to_cpu(mbse);
		pr_debug("SMC MBSE after wake: %p4ch\n", &s2r.smc_wake_reason);
	} else {
		pr_warn("SMC MBSE read failed: %d\n", ret);
	}

	/* What the SMC saw while asleep. macOS reads it on wake, here it is only logged. */
	if (ret != -ETIMEDOUT) {
		ret = apple_s2r_smc_read(SMC_KEY(MBSW), &mbsw, sizeof(mbsw));
		if (ret) {
			pr_warn("SMC MBSW read failed: %d\n", ret);
		} else {
			s2r_wake_mbsw = le64_to_cpu(mbsw);
			s2r_wake_mbsw_ok = true;
			apple_s2r_mbsw_log(s2r_wake_mbsw);
		}
	}

	if (!apple_s2r_smc_write(S2R_SMC_WAKA))
		pr_debug("sent waka\n");
	s2r.smc_announced = false;
}

static void apple_s2r_finish(void)
{
	apple_s2r_release_wakeups();
	apple_s2r_pmp_set(false);
	apple_s2r_aop_ap_power(true);
	apple_s2r_gpu_ap_power(true);

	apple_s2r_restart_displays();
	if (s2r.attempts == s2r.attempts_at_prepare) {
		pr_info("enter not reached (pm_test or a wakeup event)\n");
		apple_s2r_smc_wake();
		return;
	}

	pr_debug("wake state: isr %#llx ipi_sr %#llx pmcr0 %#llx\n",
		 s2r.wake_isr, s2r.wake_ipi_sr, s2r.wake_pmcr0);
	if (s2r_restored & S2R_RESTORED_DONE)
		pr_debug("m1n1 restore after wake: %d\n", (int)lower_32_bits(s2r_restored));
	if (s2r.sep_bound && s2r.resumes_after != s2r.resumes_before)
		s2r_sep_down = true;
	pr_debug("woke: ret %d, m1n1 resumes %llu -> %llu, counter %llu -> %llu (%llu ms)\n",
		 s2r.last_ret, s2r.resumes_before, s2r.resumes_after, s2r.counter_before,
		 s2r.counter_after,
		 div_u64(s2r.counter_after - s2r.counter_before, arch_timer_get_cntfrq() / 1000));

	apple_s2r_pmgr_report();
	apple_s2r_sep_pg_report();
	apple_s2r_dcs_report();
	apple_s2r_quiesce_report();
	apple_s2r_smc_wake();
}

/*
 * Runs after a round that enter() came back from without error and without
 * a wakeup event pending before it, once devices are back from resume_early
 * and IRQs are on. Sleep again unless a driver made this wake a wakeup
 * event: the SMC wakes the AP for every event it sends, and macsmc-input
 * only reports the power button and opening the lid as one.
 */
static bool apple_s2r_suspend_again(void)
{
	void (*flush_rx)(struct apple_rtkit *rtk);

	/* pm_test rounds end up here too, without having slept */
	if (s2r.attempts == s2r.attempts_at_prepare || s2r.last_ret || apple_s2r_pm_test())
		return false;

	if (READ_ONCE(s2r_wake_held)) {
		pr_info("full wake: a wakeup event on the way down\n");
		return false;
	}

	if (!s2r.smc || !s2r.smc->rtk) {
		pr_info("full wake: no SMC to ask why it woke\n");
		return false;
	}

	/* Let the SMC events that came with the wake reach their drivers */
	flush_rx = symbol_get(apple_rtkit_flush_rx);
	if (!flush_rx) {
		pr_info("full wake: cannot flush the SMC events\n");
		return false;
	}
	flush_rx(s2r.smc->rtk);
	symbol_put(apple_rtkit_flush_rx);

	/* suspend_enter() only looked before enter() */
	if (pm_wakeup_pending()) {
		pr_info("full wake: wakeup event\n");
		return false;
	}

	/*
	 * macOS only sends slpw and slpa again after a full dark wake. Whether
	 * the SMC takes waka, slpw and slpa back to back while the devices
	 * stay suspended is unknown, and an SMC that thought the AP was asleep
	 * when it was not hung the machine for good. Wake fully after slpa
	 * until a round trip like that has been tried on its own.
	 */
	if (s2r.slpa_sent && !apple_s2r_dark_wake()) {
		pr_info("full wake: slpa went out\n");
		return false;
	}

	if (s2r.again >= S2R_AGAIN_MAX) {
		pr_info("full wake: went back to sleep %u times, the most it does\n", s2r.again);
		return false;
	}

	/*
	 * Dark wake: finish() sent waka, so announce the next sleep the way
	 * begin() did. prepare_late() sends slpa again.
	 */
	if (s2r.slpa_sent) {
		int ret;

		msleep(S2R_DARK_WAKE_GAP_MS);
		ret = apple_s2r_smc_write(S2R_SMC_SLPW);
		/* A timed out write may still have reached the SMC, end() then owes it a waka */
		if (!ret || ret == -ETIMEDOUT)
			s2r.smc_announced = true;
		if (ret) {
			pr_info("full wake: slpw for the dark wake failed: %d\n", ret);
			return false;
		}
		s2r.slpa_sent = false;
		/* The SEP went down in the last round and stays down: same as begin() */
		if (s2r.sep_bound && s2r_sep_down) {
			pr_info("SEP is down since the last round, the PMGR does not wait for it\n");
			s2r.sep_bound = false;
		}
		pr_debug("dark wake (MBSW %016llx), sent slpw again\n", s2r_wake_mbsw);
	}

	s2r.again++;
	pr_info("re-sleep %u of %u: no wakeup event after %llu ms (isr %#llx, %u SMC events, last %#x)\n",
		s2r.again, S2R_AGAIN_MAX,
		div_u64(s2r.counter_after - s2r.counter_before, arch_timer_get_cntfrq() / 1000),
		s2r.wake_isr, READ_ONCE(s2r.smc_events), READ_ONCE(s2r.smc_last_event));

	return true;
}

static void apple_s2r_end(void)
{
	/* Suspend failed before finish(), the SMC still thinks the AP is going down */
	apple_s2r_smc_wake();

	if (wdt_saved) {
		writel(0, s2r.wdt + WDT_WD1_CTL);
		writel(0, s2r.wdt + WDT_WD1_CUR);
		writel(wdt_saved_bite, s2r.wdt + WDT_WD1_BITE);
		writel(wdt_saved_ctl, s2r.wdt + WDT_WD1_CTL);
		wdt_saved = false;
	}

	if (s2r.ptd_noticed) {
		writel(s2r.ptd_sleep_saved, s2r.ptd + PTD_SLEEP);
		s2r.ptd_noticed = false;
	}

	if (s2r.smc_nb_registered)
		blocking_notifier_chain_unregister(&s2r.smc->event_handlers, &s2r.smc_nb);
	s2r.smc_nb_registered = false;
	if (s2r.smc_pdev)
		put_device(&s2r.smc_pdev->dev);
	s2r.smc_pdev = NULL;
	s2r.smc = NULL;
	WRITE_ONCE(s2r.sleeping, false);
	apple_s2r_release_wakeups();
	apple_s2r_ps_late_unmap();
}

static const struct platform_suspend_ops apple_s2r_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.begin = apple_s2r_begin,
	.prepare = apple_s2r_prepare,
	.prepare_late = apple_s2r_prepare_late,
	.enter = apple_s2r_enter,
	.wake = apple_s2r_wake,
	.finish = apple_s2r_finish,
	.suspend_again = apple_s2r_suspend_again,
	.end = apple_s2r_end,
};

static int apple_s2r_set_enable(const char *val, const struct kernel_param *kp)
{
	bool enable;
	int ret;

	ret = kstrtobool(val, &enable);
	if (ret)
		return ret;

	if (enable && !s2r.mbox)
		return -ENODEV;

	if (enable && !s2r_enabled) {
		suspend_set_ops(&apple_s2r_suspend_ops);
		pr_info("deep sleep available, select it in /sys/power/mem_sleep\n");
	}
	s2r_enabled |= enable;

	return 0;
}

static const struct kernel_param_ops apple_s2r_enable_ops = {
	.set = apple_s2r_set_enable,
	.get = param_get_bool,
};

module_param_cb(enable, &apple_s2r_enable_ops, &s2r_enabled, 0644);
MODULE_PARM_DESC(enable, "Offer suspend-to-RAM as deep sleep (one way)");

/*
 * For drivers that do more for deep sleep than for s2idle. Once the suspend
 * has a target state, whether that is deep sleep. Before that, from the
 * PM_SUSPEND_PREPARE notifiers on, whether deep sleep is what "mem" selects.
 */
bool apple_s2r_deep_sleep(void)
{
	if (!READ_ONCE(s2r_enabled))
		return false;
	if (pm_suspend_target_state != PM_SUSPEND_ON)
		return pm_suspend_target_state == PM_SUSPEND_MEM;
	return READ_ONCE(mem_sleep_current) == PM_SUSPEND_MEM;
}
EXPORT_SYMBOL_GPL(apple_s2r_deep_sleep);

/**
 * apple_s2r_power_cut() - whether the SoC lost power in the sleep that is ending
 *
 * False when a deep sleep was given up on the way down. The devices then
 * still hold their state, and a restore meant for a power loss is not due.
 *
 * Return: true once the SoC went down and came back.
 */
bool apple_s2r_power_cut(void)
{
	return READ_ONCE(s2r_power_cut);
}
EXPORT_SYMBOL_GPL(apple_s2r_power_cut);

static int __init apple_s2r_cpuhp_init(void)
{
	if (!s2r.mbox)
		return 0;

	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "soc/apple-s2r:online",
				  apple_s2r_cpu_online, apple_s2r_cpu_down_prep);

	return 0;
}
late_initcall(apple_s2r_cpuhp_init);

/*
 * Setup
 */

static void apple_s2r_read_acc(void *unused)
{
	s2r_cpus[smp_processor_id()].acc_ovrd = read_sysreg_s(SYS_IMP_APL_ACC_OVRD);
}

static int __init apple_s2r_cpus_acc(void)
{
	if (s2r.mbox)
		on_each_cpu(apple_s2r_read_acc, NULL, 1);
	return 0;
}
late_initcall(apple_s2r_cpus_acc);

static int __init apple_s2r_map_cpu(unsigned int cpu)
{
	struct apple_s2r_cpu *c = &s2r_cpus[cpu];
	u64 mpidr = apple_s2r_mpidr(cpu);
	int i;

	for (i = 0; i < s2r.mbox->ncpus && i < S2R_MAX_CPUS; i++)
		if (s2r.mbox->cpu[i].mpidr == mpidr)
			break;
	if (i >= s2r.mbox->ncpus || i >= S2R_MAX_CPUS)
		return -ENOENT;

	c->slot = &s2r.mbox->cpu[i];
	c->impl = ioremap_np(c->slot->impl_reg, CPU_IMPL_PWRSTATE + 8);
	c->pmgr = ioremap_np(c->slot->start_reg0 - 4, 8);
	c->start1 = ioremap_np(c->slot->start_reg1, 4);
	if (!c->impl || !c->pmgr || !c->start1) {
		c->slot = NULL;
		return -ENOMEM;
	}
	c->state = S2R_CPU_ONLINE;

	/* Resume and hotplug only work when a reset lands in this m1n1 */
	if ((readq_relaxed(c->impl + CPU_IMPL_RVBAR) & CPU_IMPL_RVBAR_ADDR) != s2r.mbox->rvbar)
		pr_warn("CPU%u: RVBAR 0x%llx is not m1n1's 0x%llx\n", cpu,
			readq_relaxed(c->impl + CPU_IMPL_RVBAR), s2r.mbox->rvbar);

	return 0;
}

static int __init apple_s2r_init(void)
{
	struct device_node *chosen;
	unsigned int cpu;
	u64 addr;

	if (!of_machine_is_compatible("apple,arm-platform"))
		return 0;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return 0;
	if (of_property_read_u64(chosen, "asahi,s2r-mailbox", &addr)) {
		of_node_put(chosen);
		return 0;
	}
	of_node_put(chosen);

	if (!of_machine_is_compatible("apple,t6000")) {
		pr_info("only supported on t6000 (M1 Pro), no deep sleep on this machine\n");
		return 0;
	}

	s2r.mbox = memremap(addr, sizeof(*s2r.mbox), MEMREMAP_WB);
	if (!s2r.mbox) {
		pr_err("cannot map the m1n1 mailbox at 0x%llx\n", addr);
		return -ENOMEM;
	}
	apple_s2r_mbox_inval();
	if (s2r.mbox->magic != S2R_MAGIC || s2r.mbox->version != S2R_VERSION) {
		pr_err("unknown m1n1 mailbox (magic 0x%llx version %u)\n", s2r.mbox->magic,
		       s2r.mbox->version);
		memunmap(s2r.mbox);
		s2r.mbox = NULL;
		return -EINVAL;
	}
	s2r.boot_mpidr = s2r.mbox->boot_mpidr;

	for_each_possible_cpu(cpu)
		if (apple_s2r_map_cpu(cpu))
			pr_warn("CPU%u (0x%llx) is not in the m1n1 mailbox\n", cpu,
				apple_s2r_mpidr(cpu));

	spin_table_set_hotplug_ops(&apple_s2r_hotplug_ops);
	pr_info("m1n1 mailbox at 0x%llx, %u CPUs, CPU hotplug through m1n1\n", addr,
		s2r.mbox->ncpus);

	return 0;
}
early_initcall(apple_s2r_init);
