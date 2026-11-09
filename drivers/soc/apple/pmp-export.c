// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <linux/soc/apple/pmp.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/s2r.h>

/* Join removal and serialize the firmware's untagged power reply stream. */
static DEFINE_MUTEX(pmp_lock);
static const void *pmp_data;
static struct device *pmp_device;

#define ASC_CPU_CONFIG		0x40
#define ASC_CPU_CONTROL		0x44
#define ASC_CPU_CONTROL_RUN	BIT(4)
#define ASC_CPU_STATUS		0x48
#define ASC_CPU_STATUS_IDLE	GENMASK(1, 0)
/* In the mailbox: bit 0 enables the IOP outbox (AppleASCWrapV4::_enableOutbox) */
#define MBOX_A2I_CONTROL	0x110
#define MBOX_I2A_CONTROL	0x114
#define MBOX_OUTBOX_ENABLE	BIT(0)

/* T6000 PMP reset vector, reg[1] of the macOS pmp node; bit 0 locks it */
#define T6000_PMP_IORVBAR	0x28e850000ULL
#define IORVBAR_LOCK		BIT_ULL(0)

/*
 * The APF filter of dart-pmp, reg[1] of its macOS node. All PMP traffic
 * through the DART passes it (TCR bit 12 would bypass it). m1n1 fills it
 * from filter-data-instance-0; it loses that with the power cut, and the
 * T6000 PPL dartRestore writes the entries back before any TCR.
 */
#define T6000_PMP_DAPF		0x28e304000ULL
#define DAPF_ENTRIES		16	/* n-apfs-instance-0 */
#define DAPF_STRIDE		0x40
#define DAPF_WORDS		6	/* control, 0x4, start lo/hi, end lo/hi */
/* Every ADT entry sets these (r0_lo in m1n1); the unused ones read clear */
#define DAPF_CTRL_SET		GENMASK(1, 0)

/* PMGR PS fields as macOS ApplePMGR::_setPSLevel and _setPSAutoMinLevel use them */
#define PS_TARGET		GENMASK(3, 0)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_WAS_GATED		GENMASK(9, 8)	/* write one to clear */
#define PS_DEV_DISABLE		BIT(10)
#define PS_BUSY			BIT(11)
#define PS_AUTO_MIN		GENMASK(19, 16)
#define PS_AUTO_ENABLE		BIT(28)
#define PS_BIT30		BIT(30)
#define PS_ACTIVE		0xf
/* 0x2ee00 ticks of 24 MHz */
#define PS_TIMEOUT_US		8000

static void __iomem *pmp_asc;

/* What the SRAM holds, to tell an image from a bus that reads back nothing */
struct pmp_sram_sum {
	u32 crc, word0, zero, ones;
};

struct pmp_ps {
	const char *name;
	struct regmap *map;
	u32 reg;
	u32 saved;
};

/* The T6000 power cycle for system sleep, as macOS RTBuddy does it */
static struct {
	void __iomem *mbox_ctl;
	void __iomem *outbox;
	void __iomem *iorvbar;
	void __iomem *dapf;
	u32 dapf_saved[DAPF_ENTRIES][DAPF_WORDS];
	int dapf_n;
	void __iomem *sram;
	size_t sram_size;
	u32 *sram_copy;
	bool sram_saved;
	struct pmp_sram_sum saved_sum;
	u64 iorvbar_saved;
	struct pmp_ps ps_pmp, ps_sram;
	struct device *dart;
	int dart_irq;
	struct device *mbox;
	int mbox_irq[2];
	/* Idled for sleep: nothing may talk to it until it boots again */
	bool stopped;
	/* Powered down, with its mailbox, DART, IORVBAR and report region */
	bool down;
} pmp_pc;

static u32 pmp_ps_read(struct pmp_ps *ps)
{
	u32 v = 0;

	regmap_read(ps->map, ps->reg, &v);
	return v;
}

static int pmp_ps_wait(struct pmp_ps *ps, u32 mask, u32 val)
{
	u32 v;
	int ret;

	ret = regmap_read_poll_timeout_atomic(ps->map, ps->reg, v, (v & mask) == val,
					      1, PS_TIMEOUT_US);
	if (ret)
		dev_warn(pmp_device, "PS %s: %#x, waited for %#x in %#x\n", ps->name, v,
			 val, mask);
	return ret;
}

/* AppleT6000PMGR::writeReg32 waits out BUSY after each write of a lower target */
static int pmp_ps_write(struct pmp_ps *ps, u32 val)
{
	regmap_write(ps->map, ps->reg, val);
	if (FIELD_GET(PS_TARGET, val) == PS_ACTIVE)
		return 0;
	return pmp_ps_wait(ps, PS_BUSY, 0);
}

/* ApplePMGR::_setPSAutoMinLevel */
static void pmp_ps_set_auto_min(struct pmp_ps *ps, u32 level)
{
	u32 r = pmp_ps_read(ps);

	pmp_ps_write(ps, (r & ~(PS_AUTO_MIN | PS_WAS_GATED)) |
		     FIELD_PREP(PS_AUTO_MIN, level));
}

/* ApplePMGR::_setPSLevel */
static int pmp_ps_set_level(struct pmp_ps *ps, u32 level, bool auto_enable)
{
	u32 r = pmp_ps_read(ps), w;
	int ret;

	if (FIELD_GET(PS_TARGET, r) == level && !!(r & PS_AUTO_ENABLE) == auto_enable &&
	    !(r & (PS_BIT30 | PS_DEV_DISABLE)))
		return 0;
	if (r & PS_AUTO_ENABLE) {
		/* macOS leaves auto PM only from the active state */
		if (FIELD_GET(PS_TARGET, r) != PS_ACTIVE)
			return -EINVAL;
		w = r & ~(PS_AUTO_ENABLE | PS_WAS_GATED);
		pmp_ps_write(ps, w);
		pmp_ps_wait(ps, PS_ACTUAL, FIELD_PREP(PS_ACTUAL, PS_ACTIVE));
	} else {
		w = r & ~PS_WAS_GATED;
	}
	w = (w & ~PS_TARGET) | level;
	pmp_ps_write(ps, level == PS_ACTIVE ? w : w | PS_WAS_GATED);
	ret = pmp_ps_wait(ps, PS_ACTUAL, FIELD_PREP(PS_ACTUAL, level));
	if (auto_enable) {
		w |= PS_AUTO_ENABLE;
		pmp_ps_write(ps, w);
	}
	if (w & PS_BIT30) {
		w &= ~PS_BIT30;
		pmp_ps_write(ps, w);
	}
	if (w & PS_DEV_DISABLE) {
		w &= ~PS_DEV_DISABLE;
		pmp_ps_write(ps, w);
	}
	return ret;
}

/* As the PMGR powers a device up: its auto-min level, then active */
static int pmp_ps_power_up(struct pmp_ps *ps)
{
	pmp_ps_set_auto_min(ps, FIELD_GET(PS_AUTO_MIN, ps->saved));
	return pmp_ps_set_level(ps, PS_ACTIVE, ps->saved & PS_AUTO_ENABLE);
}

static void pmp_pc_mbox_irqs(bool enable)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pmp_pc.mbox_irq); i++) {
		if (enable)
			enable_irq(pmp_pc.mbox_irq[i]);
		else
			disable_irq(pmp_pc.mbox_irq[i]);
	}
}

/* AppleT6000DART save and restore around the power cut, as its runtime PM */
static int pmp_pc_dart(bool up)
{
	struct device *dart = pmp_pc.dart;
	const struct dev_pm_ops *pm = dart->driver ? dart->driver->pm : NULL;
	int ret = -ENODEV;

	if (!up) {
		disable_irq(pmp_pc.dart_irq);
		if (pm && pm->runtime_suspend)
			ret = pm->runtime_suspend(dart);
		return ret;
	}
	if (pm && pm->runtime_resume)
		ret = pm->runtime_resume(dart);
	enable_irq(pmp_pc.dart_irq);
	return ret;
}

/* The filters m1n1 wrote at boot, which are the ADT ones macOS writes */
static void pmp_pc_dapf_save(void)
{
	int i, j;

	for (i = 0; i < DAPF_ENTRIES; i++) {
		void __iomem *e = pmp_pc.dapf + i * DAPF_STRIDE;

		if (!(readl(e) & DAPF_CTRL_SET))
			break;
		for (j = 0; j < DAPF_WORDS; j++)
			pmp_pc.dapf_saved[i][j] = readl(e + 4 * j);
	}
	pmp_pc.dapf_n = i;
}

/*
 * The APF loop of the PPL dartRestore, unlocked: plain stores of 0x4,
 * 0x8, 0xc, 0x10 and 0x14, then the control word. Returns how many
 * entries read back differently afterwards.
 */
static int pmp_pc_dapf_restore(void)
{
	int i, j, bad = 0;

	for (i = 0; i < pmp_pc.dapf_n; i++) {
		void __iomem *e = pmp_pc.dapf + i * DAPF_STRIDE;

		for (j = 1; j < DAPF_WORDS; j++)
			writel(pmp_pc.dapf_saved[i][j], e + 4 * j);
		writel(pmp_pc.dapf_saved[i][0], e);
	}
	for (i = 0; i < pmp_pc.dapf_n; i++)
		for (j = 0; j < DAPF_WORDS; j++)
			if (readl(pmp_pc.dapf + i * DAPF_STRIDE + 4 * j) !=
			    pmp_pc.dapf_saved[i][j]) {
				bad++;
				break;
			}
	return bad;
}

/* Reads only: the copy, or the SRAM itself when copy is NULL */
static void pmp_pc_sram_sum(const u32 *copy, struct pmp_sram_sum *s)
{
	size_t i, n = pmp_pc.sram_size / 4;
	u32 v;

	memset(s, 0, sizeof(*s));
	s->crc = ~0U;
	for (i = 0; i < n; i++) {
		v = copy ? copy[i] : readl_relaxed(pmp_pc.sram + 4 * i);
		if (!i)
			s->word0 = v;
		s->zero += !v;
		s->ones += v == ~0U;
		s->crc = crc32_le(s->crc, (u8 *)&v, sizeof(v));
	}
}

static void pmp_pc_sram_log(const char *when, const struct pmp_sram_sum *s)
{
	dev_dbg(pmp_device, "SRAM %s: crc %08x, word 0 %#x, %u zero, %u ones\n",
		when, s->crc, s->word0, s->zero, s->ones);
}

/* RTBuddy saveSRAM and restoreSRAM: ascending 32-bit words */
static void pmp_pc_sram(bool save)
{
	size_t i, n = pmp_pc.sram_size / 4;

	if (save) {
		for (i = 0; i < n; i++)
			pmp_pc.sram_copy[i] = readl_relaxed(pmp_pc.sram + 4 * i);
	} else {
		for (i = 0; i < n; i++)
			writel_relaxed(pmp_pc.sram_copy[i], pmp_pc.sram + 4 * i);
		/* The whole image is written before the PMP is powered up and reads it */
		wmb();
	}
	pmp_pc.sram_saved = save;
}

/* Reads only: how CPU status moves in the first 2 ms after RUN */
static void pmp_pc_status_trace(void)
{
	u32 seen[8], at[8], v;
	int i, n = 0, us;
	char buf[160] = "";
	int len = 0;

	for (us = 0; us <= 2000 && n < ARRAY_SIZE(seen); us += 10) {
		v = readl(pmp_asc + ASC_CPU_STATUS);
		if (!n || v != seen[n - 1]) {
			seen[n] = v;
			at[n++] = us;
		}
		udelay(10);
	}
	for (i = 0; i < n; i++)
		len += scnprintf(buf + len, sizeof(buf) - len, " %#x@%uus", seen[i], at[i]);
	dev_dbg(pmp_device, "CPU status after RUN:%s\n", buf);
}

/* RTBuddy::_setManagedStateGated for sleep */
static int pmp_pc_stop(void)
{
	struct pmp_sram_sum halted;
	u32 st, ps_pmp, ps_sram, cfg, ps_pmp_now, ps_sram_now;
	int ret, ret_pmp, ret_sram;

	if (!pmp_pc.sram)
		return -EOPNOTSUPP;
	if (pmp_pc.stopped)
		return -EALREADY;
	if (!pmp_pc.sram_copy) {
		pmp_pc.sram_copy = vmalloc(pmp_pc.sram_size);
		if (!pmp_pc.sram_copy)
			return -ENOMEM;
	}

	/* The report region goes down with PMS_SRAM, the requests wait in a shadow */
	apple_pmp_report_gate(true);
	ret = apple_pmp_set_sleep(pmp_data, APPLE_PMP_IDLE);
	if (ret) {
		apple_pmp_report_gate(false);
		return ret;
	}
	pmp_pc.stopped = true;
	pmp_pc_mbox_irqs(false);

	/* stopCPU */
	writel(readl(pmp_pc.outbox) & ~MBOX_OUTBOX_ENABLE, pmp_pc.outbox);
	ret = readl_poll_timeout(pmp_asc + ASC_CPU_STATUS, st, st & ASC_CPU_STATUS_IDLE,
				 1000, 10000);
	if (ret) {
		/* macOS panics here. Without the power cut a restart cannot work. */
		dev_err(pmp_device, "CPU never idle (%#x), not powering it down\n", st);
		return ret;
	}
	writel(readl(pmp_asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN,
	       pmp_asc + ASC_CPU_CONTROL);
	ret = pmp_pc_dart(false);
	pmp_pc.iorvbar_saved = readq(pmp_pc.iorvbar);
	ps_pmp = pmp_ps_read(&pmp_pc.ps_pmp);
	pmp_pc.ps_pmp.saved = ps_pmp;
	ps_sram = pmp_ps_read(&pmp_pc.ps_sram);
	pmp_pc.ps_sram.saved = ps_sram;
	/* Stopped but still powered: the image as it should be saved */
	cfg = readl(pmp_asc + ASC_CPU_CONFIG);
	pmp_pc_sram_sum(NULL, &halted);
	pmp_pc.down = true;
	ret_pmp = pmp_ps_set_level(&pmp_pc.ps_pmp, 0, false);

	/* With the PMP off and PMS_SRAM still on, then PMS_SRAM off */
	pmp_pc_sram(true);
	ret_sram = pmp_ps_set_level(&pmp_pc.ps_sram, 0, false);

	ps_pmp_now = pmp_ps_read(&pmp_pc.ps_pmp);
	ps_sram_now = pmp_ps_read(&pmp_pc.ps_sram);
	dev_dbg(pmp_device,
		"down for sleep: CPU status %#x, config %#x, DART %d, IORVBAR %#llx, PS pmp %#x -> %#x (%d), pms_sram %#x -> %#x (%d)\n",
		st, cfg, ret, pmp_pc.iorvbar_saved, ps_pmp, ps_pmp_now, ret_pmp,
		ps_sram, ps_sram_now, ret_sram);
	pmp_pc_sram_log("halted, powered", &halted);
	pmp_pc_sram_sum(pmp_pc.sram_copy, &pmp_pc.saved_sum);
	pmp_pc_sram_log("saved with the PMP off", &pmp_pc.saved_sum);
	return 0;
}

/* The RTBuddy wake path, through startCPUWithOptions and the HELLO */
static int pmp_pc_start(void)
{
	struct pmp_sram_sum back;
	int ret_sram, ret_pmp, ret_dart, ret, dapf_bad;
	bool wrote = false;
	u32 dapf0, cpu_config, cpu_control, cpu_status, ps_pmp_now, ps_sram_now;
	u64 rvbar;

	if (!pmp_pc.stopped)
		return 0;
	if (!pmp_pc.down) {
		/* Its CPU never stopped, so it cannot boot again; undo the rest */
		writel(readl(pmp_pc.outbox) | MBOX_OUTBOX_ENABLE, pmp_pc.outbox);
		pmp_pc_mbox_irqs(true);
		pmp_pc.stopped = false;
		apple_pmp_report_gate(false);
		return -EIO;
	}

	/* enableSRAM, restoreSRAM */
	ret_sram = pmp_ps_power_up(&pmp_pc.ps_sram);
	if (pmp_pc.sram_saved)
		pmp_pc_sram(false);
	pmp_pc_sram_sum(NULL, &back);

	/* startCPUWithOptions: power, DART, firmware, interrupts, outbox, RUN */
	ret_pmp = pmp_ps_power_up(&pmp_pc.ps_pmp);
	pmp_pc.down = false;
	pmp_pc_sram_log("restored, read with the PMP off", &back);
	pmp_pc_sram_sum(NULL, &back);
	pmp_pc_sram_log("with the PMP on", &back);
	cpu_config = readl(pmp_asc + ASC_CPU_CONFIG);
	cpu_control = readl(pmp_asc + ASC_CPU_CONTROL);
	cpu_status = readl(pmp_asc + ASC_CPU_STATUS);
	dev_dbg(pmp_device, "CPU after power up: config %#x, control %#x, status %#x\n",
		cpu_config, cpu_control, cpu_status);
	/* In dartRestore the APF entries go in ahead of the TCRs */
	dapf0 = readl(pmp_pc.dapf);
	dapf_bad = pmp_pc_dapf_restore();
	ret_dart = pmp_pc_dart(true);
	/*
	 * iBoot locked it to the SRAM, and macOS writes it only when unlocked.
	 * Should it come back reset, put back what iBoot left there.
	 */
	rvbar = readq(pmp_pc.iorvbar);
	if (!(rvbar & IORVBAR_LOCK)) {
		writeq(pmp_pc.iorvbar_saved, pmp_pc.iorvbar);
		wrote = true;
	}
	ps_pmp_now = pmp_ps_read(&pmp_pc.ps_pmp);
	ps_sram_now = pmp_ps_read(&pmp_pc.ps_sram);
	dev_dbg(pmp_device,
		"up after sleep: PS pmp %#x (%d), pms_sram %#x (%d), DART %d, IORVBAR %#llx%s, DAPF %d entries (control 0 was %#x, %d differ)\n",
		ps_pmp_now, ret_pmp, ps_sram_now, ret_sram,
		ret_dart, rvbar, wrote ? ", lost and written again" : "",
		pmp_pc.dapf_n, dapf0, dapf_bad);

	/* A fresh RTKit state before RUN, so the HELLO cannot be lost */
	ret = apple_pmp_set_sleep(pmp_data, APPLE_PMP_REINIT);
	pmp_pc_mbox_irqs(true);
	if (!ret) {
		writel(readl(pmp_pc.outbox) | MBOX_OUTBOX_ENABLE, pmp_pc.outbox);
		writel(readl(pmp_asc + ASC_CPU_CONTROL) | ASC_CPU_CONTROL_RUN,
		       pmp_asc + ASC_CPU_CONTROL);
		pmp_pc_status_trace();
		ret = apple_pmp_set_sleep(pmp_data, APPLE_PMP_BOOT);
	}
	if (ret)
		dev_err(pmp_device,
			"no boot after sleep: CPU control %#x, status %#x, mailbox A2I %#x, I2A %#x\n",
			readl(pmp_asc + ASC_CPU_CONTROL), readl(pmp_asc + ASC_CPU_STATUS),
			readl(pmp_pc.mbox_ctl), readl(pmp_pc.outbox));
	pmp_pc.stopped = false;
	apple_pmp_report_gate(false);
	if (!ret)
		dev_dbg(pmp_device, "restarted after sleep\n");
	return ret;
}

static int apple_pmp_s2r_sleep(int op)
{
	guard(mutex)(&pmp_lock);
	if (!pmp_data || !pmp_asc)
		return -ENODEV;
	return op == APPLE_S2R_PMP_STOP ? pmp_pc_stop() : pmp_pc_start();
}

static struct device *pmp_pc_find_device(struct device_node *np, const char *prop)
{
	struct device_node *node = of_parse_phandle(np, prop, 0);
	struct platform_device *pdev = node ? of_find_device_by_node(node) : NULL;

	of_node_put(node);
	return pdev ? &pdev->dev : NULL;
}

static int pmp_pc_find_ps(struct device_node *ps_np, struct pmp_ps *ps)
{
	struct device_node *parent = of_get_parent(ps_np);

	ps->map = parent ? syscon_node_to_regmap(parent) : ERR_PTR(-ENODEV);
	of_node_put(parent);
	if (IS_ERR(ps->map))
		return PTR_ERR(ps->map);
	return of_property_read_u32(ps_np, "reg", &ps->reg);
}

/* Only T6000 has the power cycle; elsewhere stop returns -EOPNOTSUPP */
static int pmp_pc_init(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *ps_np, *parent, *child;
	struct resource *res;
	int ret = -ENODEV;

	if (!IS_ENABLED(CONFIG_APPLE_S2R) ||
	    !of_device_is_compatible(dev->of_node, "apple,t6000-pmp-v2") ||
	    !of_machine_is_compatible("apple,t6000"))
		return 0;

	pmp_pc.ps_pmp.name = "pmp";
	pmp_pc.ps_sram.name = "pms_sram";
	ps_np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!ps_np)
		return -ENODEV;
	ret = pmp_pc_find_ps(ps_np, &pmp_pc.ps_pmp);
	parent = of_get_parent(ps_np);
	of_node_put(ps_np);
	if (ret) {
		of_node_put(parent);
		return ret;
	}
	ret = -ENODEV;
	for_each_child_of_node(parent, child) {
		const char *label;

		if (!of_property_read_string(child, "label", &label) &&
		    !strcmp(label, "pms_sram")) {
			ret = pmp_pc_find_ps(child, &pmp_pc.ps_sram);
			of_node_put(child);
			break;
		}
	}
	of_node_put(parent);
	if (ret)
		return ret;

	pmp_pc.dart = pmp_pc_find_device(dev->of_node, "iommus");
	pmp_pc.mbox = pmp_pc_find_device(dev->of_node, "mboxes");
	if (!pmp_pc.dart || !pmp_pc.mbox)
		return -ENODEV;
	pmp_pc.dart_irq = platform_get_irq(to_platform_device(pmp_pc.dart), 0);
	pmp_pc.mbox_irq[0] = platform_get_irq_byname(to_platform_device(pmp_pc.mbox),
						     "recv-not-empty");
	pmp_pc.mbox_irq[1] = platform_get_irq_byname(to_platform_device(pmp_pc.mbox),
						     "send-empty");
	if (pmp_pc.dart_irq < 0 || pmp_pc.mbox_irq[0] < 0 || pmp_pc.mbox_irq[1] < 0)
		return -ENODEV;

	res = platform_get_resource(to_platform_device(pmp_pc.mbox), IORESOURCE_MEM, 0);
	pmp_pc.mbox_ctl = res ? ioremap_np(res->start + MBOX_A2I_CONTROL, 8) : NULL;
	pmp_pc.outbox = pmp_pc.mbox_ctl ? pmp_pc.mbox_ctl + 4 : NULL;
	pmp_pc.iorvbar = ioremap_np(T6000_PMP_IORVBAR, 8);
	pmp_pc.dapf = ioremap_np(T6000_PMP_DAPF, DAPF_ENTRIES * DAPF_STRIDE);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmp");
	if (!pmp_pc.outbox || !pmp_pc.iorvbar || !pmp_pc.dapf || !res)
		return -ENODEV;
	/* Without the filters the PMP could not reach anything after the cut */
	pmp_pc_dapf_save();
	if (!pmp_pc.dapf_n)
		return -ENOENT;
	pmp_pc.sram_size = resource_size(res);
	/* Set last: it tells stop that everything above is there */
	pmp_pc.sram = ioremap_np(res->start, pmp_pc.sram_size);
	return pmp_pc.sram ? 0 : -ENOMEM;
}

static void pmp_pc_exit(void)
{
	if (pmp_pc.sram)
		iounmap(pmp_pc.sram);
	if (pmp_pc.dapf)
		iounmap(pmp_pc.dapf);
	if (pmp_pc.iorvbar)
		iounmap(pmp_pc.iorvbar);
	if (pmp_pc.mbox_ctl)
		iounmap(pmp_pc.mbox_ctl);
	put_device(pmp_pc.dart);
	put_device(pmp_pc.mbox);
	vfree(pmp_pc.sram_copy);
	memset(&pmp_pc, 0, sizeof(pmp_pc));
}

int apple_pmp_register(struct device *dev, const void *data)
{
	struct resource *res;
	int ret;

	scoped_guard(mutex, &pmp_lock) {
		if (pmp_data)
			return -EBUSY;
		pmp_data = data;
		pmp_device = dev;
		/* The Rust driver holds the region; this only drives the CPU */
		res = platform_get_resource_byname(to_platform_device(dev),
						   IORESOURCE_MEM, "asc");
		pmp_asc = res ? ioremap_np(res->start, 0x100) : NULL;
		ret = pmp_pc_init(dev);
		if (ret) {
			dev_warn(dev, "no power cycle for system sleep: %d\n", ret);
			pmp_pc_exit();
		} else if (pmp_pc.sram) {
			struct pmp_sram_sum live;

			dev_info(dev, "power cycle for system sleep, %d DAPF entries\n",
				 pmp_pc.dapf_n);
			pmp_pc_sram_sum(NULL, &live);
			pmp_pc_sram_log("at probe, running", &live);
		}
	}
	/* Outside pmp_lock: s2r calls the hook under its own lock */
	apple_s2r_set_pmp_hook(apple_pmp_s2r_sleep);
	return 0;
}

void apple_pmp_unregister(const void *data)
{
	scoped_guard(mutex, &pmp_lock) {
		if (pmp_data != data)
			return;
		pmp_data = NULL;
		pmp_device = NULL;
		if (pmp_asc)
			iounmap(pmp_asc);
		pmp_asc = NULL;
		pmp_pc_exit();
	}
	/* Waits out a hook call already running; later ones see no pmp_data */
	apple_s2r_set_pmp_hook(NULL);
}

int apple_pmp_link_device(struct device *consumer)
{
	struct device_node *node;
	struct device *supplier = NULL;
	struct device_link *link;

	node = of_parse_phandle(consumer->of_node, "apple,pmp", 0);
	if (!node)
		return -EINVAL;
	scoped_guard(mutex, &pmp_lock) {
		if (pmp_device && pmp_device->of_node == node && device_is_bound(pmp_device))
			supplier = get_device(pmp_device);
	}
	of_node_put(node);
	if (!supplier)
		return -EPROBE_DEFER;
	link = device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(supplier);
	return link ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(apple_pmp_link_device);

int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	bool report_gfx;
	int ret;

	if ((command != 0x0e && command != 0x0f) || device_id > 15 || enabled > 1)
		return -EINVAL;
	guard(mutex)(&pmp_lock);
	if (!pmp_data)
		return -ENODEV;
	if (pmp_pc.stopped)
		return -EAGAIN;
	/* The matched native report must be ready; never use an elapsed-time fallback. */
	ret = apple_pmp_report_wait_supplier_ready(pmp_device, 5000);
	if (ret)
		return ret;
	/* T6030 needs both the RTKit power vote and the PTD bandwidth report. */
	report_gfx = IS_ENABLED(CONFIG_APPLE_T6030_GPU_PMP_LINK) &&
		     command == 0x0f && device_id == 5 &&
		     of_machine_is_compatible("apple,t6030") &&
		     of_device_is_compatible(pmp_device->of_node, "apple,t6030-pmp-v2");
	if (report_gfx) {
		ret = apple_pmp_report_check_t6030_gfx(pmp_device);
		if (ret)
			return ret;
	}
	if (report_gfx && !enabled) {
		ret = apple_pmp_report_set_t6030_gfx(pmp_device, false);
		if (ret) {
			/* Clearing was written before the ACK timed out. Restore it. */
			apple_pmp_report_set_t6030_gfx(pmp_device, true);
			return ret;
		}
	}
	ret = apple_pmp_send_power_command(pmp_data, command, device_id, enabled);
	/* Power is unknown after a lost reply; assert the report conservatively. */
	if (report_gfx && !enabled && ret)
		apple_pmp_report_set_t6030_gfx(pmp_device, true);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_set_device_power);

/* Reassert after startup, including a report left set by an earlier failure. */
int apple_pmp_t6030_gpu_ready(void)
{
	int ret;

	guard(mutex)(&pmp_lock);
	if (!pmp_data)
		return -ENODEV;
	if (pmp_pc.stopped)
		return -EAGAIN;
	ret = apple_pmp_report_set_t6030_gfx(pmp_device, false);
	if (ret)
		return ret;
	return apple_pmp_report_set_t6030_gfx(pmp_device, true);
}
EXPORT_SYMBOL_GPL(apple_pmp_t6030_gpu_ready);
