// SPDX-License-Identifier: GPL-2.0
/*
 * s2rwakefix: what a t6000 deep sleep (s2r.c) loses and nothing puts back, restored on the way up.
 *
 * 1. AUTO_ENABLE (bit 28) on the pms_bridge and afi power-state words. Without it the fabric (AFI)
 *    never idles after a wake.
 * 2. The tunables of the GPU's AXI-to-fabric bridge (0x405000000, macOS bridge 7, table from the
 *    macOS AppleT6000PMGR kext). 12 of the 21 are gone after the power cut, and with them the PMP
 *    and PMS never idle after a wake.
 *
 * 3. Stale minimum-state floors. s2r.c snapshots every power-state word when the sleep begins and puts the
 *    min-state field back after the wake. The PMP raises that field on the memory controllers and the
 *    fabric (amcc0, amcc2, afnc0_ioa) only while it is awake; a snapshot that catches it awake makes s2r
 *    write the floor back with nobody left to drop it, the PMP never idles again, and every later
 *    snapshot repeats it. Right after the wake these words read min 0: put that back.
 *
 * All run from this device's resume callbacks: after s2r's .wake (PMGR words restored) and before
 * its .finish (PMP, AOP and GPU firmware restart). The bridge sits in the GFX power domain and is
 * never touched unless GFX reads actual f, and never after the GPU firmware runs again.
 *
 * Nothing is touched unless the wake shows the power cut (pms_bridge or afi without AUTO_ENABLE): after a
 * suspend that was given up the PMP is still running, and a floor on amcc0 may be one it is standing on.
 *
 * gfx=0 leaves the bridge alone, verbose=1 logs what each phase sees (and reads the bridge at suspend).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/bits.h>
#include <linux/bitfield.h>

#define PS_ACTUAL	GENMASK(7, 4)
#define PS_WAS_GATED	GENMASK(9, 8)
#define PS_AUTO_ENABLE	BIT(28)

#define PMS_BRIDGE_PS	0x28e080100ULL
#define AFI_PS		0x28e0801d8ULL
#define GFX_PS		0x28e5803c0ULL
#define AFR_PS		0x28e0801e8ULL
#define CLUSTER5_PS	0x28e098014ULL
#define GFX_BRIDGE	0x405000000ULL

static const u32 bridge[][3] = {	/* offset, mask, value; the gate enable (0x400) last */
	{ 0x714, 0x03ff001f, 0x02000000 },
	{ 0x718, 0x0fff0fff, 0x00400080 },
	{ 0x71c, 0x0fff0fff, 0x00400080 },
	{ 0x720, 0x0fff0fff, 0x00400080 },
	{ 0x724, 0x0fff0fff, 0x00400080 },
	{ 0x728, 0x0fff0fff, 0x00400080 },
	{ 0x72c, 0x0fff0fff, 0x00400080 },
	{ 0x730, 0x0fff0fff, 0x00400080 },
	{ 0x734, 0x0fff0fff, 0x00400080 },
	{ 0x000, 0x00000003, 0x00000001 },
	{ 0x010, 0x0fff0000, 0x000d0000 },
	{ 0x014, 0x0000003f, 0x00000001 },
	{ 0x018, 0x0000003f, 0x00000001 },
	{ 0x01c, 0x0000003f, 0x00000003 },
	{ 0x020, 0x0000003f, 0x00000003 },
	{ 0x024, 0x0000003f, 0x00000003 },
	{ 0x028, 0x0000003f, 0x00000003 },
	{ 0x02c, 0x0000003f, 0x00000003 },
	{ 0x600, 0x01ffffff, 0x01ffffff },
	{ 0x800, 0x00000100, 0x00000100 },
	{ 0x400, 0x400103ff, 0x40010001 },
};

static bool bit28 = true;
module_param(bit28, bool, 0644);
MODULE_PARM_DESC(bit28, "put AUTO_ENABLE back on pms_bridge and afi in resume_noirq");
static int gfx = 1;
module_param(gfx, int, 0644);
MODULE_PARM_DESC(gfx, "0 = log only, 1 = write the GPU bridge tunables in the first phase where GFX is on");
static bool floors = true;
module_param(floors, bool, 0644);
MODULE_PARM_DESC(floors, "drop min-state floors s2r put back on amcc*/afnc0_* in resume_noirq");
static bool verbose;
module_param(verbose, bool, 0644);

static void __iomem *r_pmsb, *r_afi, *r_gfx, *r_afr, *r_cl5, *r_blk;
static struct platform_device *pdev;
static bool gfx_done, cold;
static u32 wrote, left;

#define PS_MIN		GENMASK(19, 16)
#define PS_FLAGS	GENMASK(12, 8)

static struct {
	void __iomem *reg;
	const char *name;
} floor_ps[48];
static unsigned int n_floor_ps;

static void floors_find(void)
{
	struct device_node *np, *parent;
	struct resource res;
	const char *label;
	u32 off;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (n_floor_ps == ARRAY_SIZE(floor_ps)) {
			of_node_put(np);
			break;
		}
		if (of_property_read_string(np, "label", &label))
			continue;
		/* the words seen with a stale floor, and their siblings: memory controllers and fabric 0 */
		if (strncmp(label, "amcc", 4) && strncmp(label, "afnc0_", 6))
			continue;
		/* a floor the device tree asks for is not ours to drop */
		if (of_property_present(np, "apple,min-state"))
			continue;
		if (of_property_read_u32(np, "reg", &off))
			continue;
		parent = of_get_parent(np);
		if (!parent)
			continue;
		if (!of_address_to_resource(parent, 0, &res)) {
			floor_ps[n_floor_ps].reg = ioremap_np(res.start + off, 4);
			floor_ps[n_floor_ps].name = label;
			if (floor_ps[n_floor_ps].reg)
				n_floor_ps++;
		}
		of_node_put(parent);
	}
}

/* The PMP is still stopped here and the words read min 0 until s2r's restore a moment ago */
static void floors_drop(void)
{
	char buf[200];
	unsigned int i, n = 0, hit = 0;
	u32 v;

	buf[0] = 0;
	for (i = 0; i < n_floor_ps; i++) {
		v = readl(floor_ps[i].reg);
		if (!FIELD_GET(PS_MIN, v) || (v & 0xf) != 0xf || !(v & PS_AUTO_ENABLE))
			continue;
		hit++;
		n += scnprintf(buf + n, sizeof(buf) - n, " %s %#x", floor_ps[i].name, v);
		if (floors)
			writel(v & ~(PS_FLAGS | PS_MIN), floor_ps[i].reg);
	}
	if (hit)
		pr_info("s2rwakefix: %u stale min-state floors %s:%s\n", hit, floors ? "dropped" : "seen", buf);
}

static void bit28_one(const char *name, void __iomem *reg)
{
	u32 v = readl(reg), n = v;

	if (bit28 && !(v & PS_AUTO_ENABLE)) {
		/* as the experiment that survived: actual and the gated flags are not written back */
		writel((v & ~(GENMASK(12, 8) | PS_ACTUAL)) | PS_AUTO_ENABLE, reg);
		udelay(100);
		n = readl(reg);
	}
	if (verbose)
		pr_info("s2rwakefix: %s %#x -> %#x\n", name, v, n);
}

static void gfx_phase(const char *when, bool may_write)
{
	u32 g = readl(r_gfx), v, bad = 0;
	bool write = gfx == 1 && may_write && !gfx_done;
	bool held = false;
	char buf[400];
	int i, n = 0;

	buf[0] = 0;
	if (verbose)
		pr_info("s2rwakefix: %s GFX %#x AFR %#x cluster5 %#x\n", when, g, readl(r_afr), readl(r_cl5));
	/* The bridge is in the GFX domain: never touch it unless GFX is on with target f */
	if (FIELD_GET(PS_ACTUAL, g) != 0xf || (g & 0xf) != 0xf)
		return;
	if (g & PS_AUTO_ENABLE) {
		/*
		 * With AUTO_ENABLE the block may drop under us, at the latest once the gate enable
		 * is in. Hold it on for the writes, as the PMGR does: auto PM is only left from
		 * the active state and put back after. Looking alone is not worth that.
		 */
		if (!write)
			return;
		writel(g & ~(PS_AUTO_ENABLE | PS_WAS_GATED), r_gfx);
		udelay(200);
		if (FIELD_GET(PS_ACTUAL, readl(r_gfx)) != 0xf) {
			pr_warn("s2rwakefix: %s GFX dropped (%#x), bridge left alone\n", when, readl(r_gfx));
			writel((readl(r_gfx) & ~PS_WAS_GATED) | PS_AUTO_ENABLE, r_gfx);
			return;
		}
		held = true;
	}
	for (i = 0; i < ARRAY_SIZE(bridge); i++) {
		v = readl(r_blk + bridge[i][0]);
		if ((v & bridge[i][1]) == bridge[i][2])
			continue;
		bad++;
		n += scnprintf(buf + n, sizeof(buf) - n, " %#x=%#x", bridge[i][0], v);
		if (write)
			writel((v & ~bridge[i][1]) | bridge[i][2], r_blk + bridge[i][0]);
	}
	if (write) {
		gfx_done = true;
		wrote = bad;
		left = 0;
		for (i = 0; i < ARRAY_SIZE(bridge); i++)
			if ((readl(r_blk + bridge[i][0]) & bridge[i][1]) != bridge[i][2])
				left++;
	}
	if (held) {
		writel((readl(r_gfx) & ~PS_WAS_GATED) | PS_AUTO_ENABLE, r_gfx);
		udelay(200);
	}
	if (write && (left || verbose))
		pr_info("s2rwakefix: %s GPU bridge: wrote %u, %u did not stick, GFX %#x -> %#x\n", when,
			wrote, left, g, readl(r_gfx));
	if (verbose)
		pr_info("s2rwakefix: %s GPU bridge was %u/%zu right;%s\n", when,
			(u32)ARRAY_SIZE(bridge) - bad, ARRAY_SIZE(bridge), buf);
}

/* How the bridge looked at the end of the awake period: GFX is on here every time. No writes to the bridge. */
static void bridge_census(const char *when)
{
	u32 g = readl(r_gfx), v, bad = 0;
	bool held = false;
	char buf[400];
	int i, n = 0;

	buf[0] = 0;
	if (FIELD_GET(PS_ACTUAL, g) != 0xf || (g & 0xf) != 0xf) {
		pr_info("s2rwakefix: %s census: GFX %#x not on\n", when, g);
		return;
	}
	if (g & PS_AUTO_ENABLE) {
		writel(g & ~(PS_AUTO_ENABLE | PS_WAS_GATED), r_gfx);
		udelay(200);
		if (FIELD_GET(PS_ACTUAL, readl(r_gfx)) != 0xf) {
			pr_info("s2rwakefix: %s census: GFX dropped (%#x)\n", when, readl(r_gfx));
			writel((readl(r_gfx) & ~PS_WAS_GATED) | PS_AUTO_ENABLE, r_gfx);
			return;
		}
		held = true;
	}
	for (i = 0; i < ARRAY_SIZE(bridge); i++) {
		v = readl(r_blk + bridge[i][0]);
		if ((v & bridge[i][1]) == bridge[i][2])
			continue;
		bad++;
		n += scnprintf(buf + n, sizeof(buf) - n, " %#x=%#x", bridge[i][0], v);
	}
	if (held)
		writel((readl(r_gfx) & ~PS_WAS_GATED) | PS_AUTO_ENABLE, r_gfx);
	pr_info("s2rwakefix: %s census: bridge %u/%zu right;%s\n", when,
		(u32)ARRAY_SIZE(bridge) - bad, ARRAY_SIZE(bridge), buf);
}

static int fix_suspend_noirq(struct device *dev)
{
	gfx_done = false;
	cold = false;
	if (verbose) {
		pr_info("s2rwakefix: suspend pms_bridge %#x afi %#x GFX %#x\n", readl(r_pmsb), readl(r_afi), readl(r_gfx));
		bridge_census("suspend");
	}
	return 0;
}

static int fix_resume_noirq(struct device *dev)
{
	cold = !(readl(r_pmsb) & PS_AUTO_ENABLE) || !(readl(r_afi) & PS_AUTO_ENABLE);
	if (!cold) {
		if (verbose)
			pr_info("s2rwakefix: no power cut seen (pms_bridge %#x afi %#x): nothing touched\n",
				readl(r_pmsb), readl(r_afi));
		return 0;
	}
	floors_drop();
	bit28_one("pms_bridge", r_pmsb);
	bit28_one("afi", r_afi);
	gfx_phase("noirq", true);
	return 0;
}

static int fix_resume_early(struct device *dev)
{
	if (cold)
		gfx_phase("early", true);
	return 0;
}

static int fix_resume(struct device *dev)
{
	/* the GPU firmware runs again here: look, never write */
	if (cold)
		gfx_phase("resume", false);
	return 0;
}

static const struct dev_pm_ops fix_pm = {
	.suspend_noirq = fix_suspend_noirq,
	.resume_noirq = fix_resume_noirq,
	.resume_early = fix_resume_early,
	.resume = fix_resume,
};

static struct platform_driver fix_driver = {
	.driver = { .name = "s2rwakefix", .pm = &fix_pm },
};

static void fix_unmap(void)
{
	void __iomem **p[] = { &r_pmsb, &r_afi, &r_gfx, &r_afr, &r_cl5, &r_blk };
	int i;

	for (i = 0; i < ARRAY_SIZE(p); i++)
		if (*p[i])
			iounmap(*p[i]);
	for (i = 0; i < n_floor_ps; i++)
		iounmap(floor_ps[i].reg);
	n_floor_ps = 0;
}

static int __init fix_init(void)
{
	int ret;

	if (!of_machine_is_compatible("apple,t6000") && !of_machine_is_compatible("apple,t6001"))
		return -ENODEV;
	r_pmsb = ioremap_np(PMS_BRIDGE_PS, 4);
	r_afi = ioremap_np(AFI_PS, 4);
	r_gfx = ioremap_np(GFX_PS, 4);
	r_afr = ioremap_np(AFR_PS, 4);
	r_cl5 = ioremap_np(CLUSTER5_PS, 4);
	r_blk = ioremap_np(GFX_BRIDGE, 0x1000);
	if (!r_pmsb || !r_afi || !r_gfx || !r_afr || !r_cl5 || !r_blk) {
		fix_unmap();
		return -ENOMEM;
	}
	floors_find();
	ret = platform_driver_register(&fix_driver);
	if (ret) {
		fix_unmap();
		return ret;
	}
	pdev = platform_device_register_simple("s2rwakefix", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&fix_driver);
		fix_unmap();
		return PTR_ERR(pdev);
	}
	pr_info("s2rwakefix: loaded (bit28 %d, gfx %d, floors %d on %u words)\n", bit28, gfx, floors, n_floor_ps);
	return 0;
}

static void __exit fix_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&fix_driver);
	fix_unmap();
}

module_init(fix_init);
module_exit(fix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("t6000 deep sleep: AUTO_ENABLE on pms_bridge/afi and the GPU bridge tunables back after a wake");
