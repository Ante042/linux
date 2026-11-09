// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * System sleep for the SEP driver: the notifier only forwards "about to sleep"
 * and "back from sleep" to Rust, which ends any Touch ID capture before the
 * system suspends (see pm.rs), and the enclave's power domains are kept on
 * across sleep.
 */
#include <linux/bitfield.h>
#include <linux/compiler.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/s2r.h>
#include <linux/pm_domain.h>
#include <linux/suspend.h>

#include "shim.h"

static void (*sep_pm_event)(bool entering);
static int (*sep_pm_sleep)(void);
static int (*sep_pm_wake)(void);

/*
 * For deep sleep (S2R) the enclave gets SLEEP (type 2) before the devices
 * suspend, as AppleSEPManager does. After every resume an enclave that went
 * to sleep is powered back on and sepOS resumed from RAM through the SEPROM,
 * so it can sleep again; one that did not sleep is left alone.
 */
static int sep_pm_notify(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	void (*event)(bool entering) = READ_ONCE(sep_pm_event);

	if (!event)
		return NOTIFY_DONE;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		event(true);
		if (apple_s2r_deep_sleep()) {
			int (*sleep)(void) = READ_ONCE(sep_pm_sleep);
			int ret = sleep ? sleep() : -ENODEV;

			if (ret && ret != -EALREADY)
				return notifier_from_errno(ret);
		}
		return NOTIFY_OK;
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		sep_diag_sleep(true);
		event(true);
		return NOTIFY_OK;
	case PM_POST_SUSPEND: {
		int (*wake)(void) = READ_ONCE(sep_pm_wake);
		int ret = wake ? wake() : -ENODEV;

		if (ret && ret != -EALREADY)
			pr_err("apple-sep: enclave wake failed: %d\n", ret);
		event(false);
		return NOTIFY_OK;
	}
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		event(false);
		sep_diag_sleep(false);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block sep_pm_nb = {
	.notifier_call = sep_pm_notify,
};

/*
 * After SLEEP the enclave gates its own power island, and its DART with it.
 * Stop the DART driver touching it, or its system suspend reads the TCRs of
 * a powered-off block and the bus hangs: macOS takes the mapper inactive.
 */
int sep_pm_gate_dart(void *dev)
{
	struct device *d = dev;
	struct device_node *np;
	struct platform_device *pdev;
	int ret;

	np = of_parse_phandle(d->of_node, "iommus", 0);
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	ret = apple_dart_quiesce_commands(&pdev->dev);
	dev_dbg(d, "SLEEP: DART %s gated (%d)\n", dev_name(&pdev->dev), ret);
	put_device(&pdev->dev);
	return ret;
}

#define SEP_PS_TARGET		GENMASK(3, 0)
#define SEP_PS_ACTUAL		GENMASK(7, 4)
#define SEP_PS_FLAGS		(BIT(8) | BIT(9))
#define SEP_PS_DEV_DISABLE	BIT(10)
#define SEP_PS_MIN		GENMASK(19, 16)

/* The enclave's own power state: its only domain, labelled "sep". */
static int sep_pm_ps(struct device *d, struct regmap **map, u32 *off)
{
	struct device_node *ps, *pmgr;
	const char *label;
	int ret;

	ps = of_parse_phandle(d->of_node, "power-domains", 0);
	if (!ps)
		return -ENODEV;
	if (of_property_read_string(ps, "label", &label) || strcmp(label, "sep")) {
		of_node_put(ps);
		return -ENODEV;
	}
	ret = of_property_read_u32_index(ps, "reg", 0, off);
	pmgr = of_get_parent(ps);
	of_node_put(ps);
	if (ret) {
		of_node_put(pmgr);
		return ret;
	}
	*map = syscon_node_to_regmap(pmgr);
	of_node_put(pmgr);
	return PTR_ERR_OR_ZERO(*map);
}

/*
 * AppleT6000PMGR::restoreHW raises the minimum state of the enclave's power
 * state to on and waits up to 8 ms for the island. The target is left as it
 * is. A suspend-to-RAM also leaves DEV_DISABLE set, which is cleared as a
 * reset deassert does, before anything reads the mailbox or the DART.
 * The minimum stays raised until sep_pm_power_release().
 */
int sep_pm_power_on(void *dev)
{
	struct device *d = dev;
	struct regmap *map;
	u32 off, before, val = 0, mid;
	int ret;

	ret = sep_pm_ps(d, &map, &off);
	if (ret)
		return ret;
	ret = regmap_read(map, off, &before);
	if (ret)
		return ret;

	regmap_update_bits(map, off, SEP_PS_FLAGS | SEP_PS_MIN, SEP_PS_MIN);
	ret = regmap_read_poll_timeout(map, off, val,
				       FIELD_GET(SEP_PS_ACTUAL, val) == 0xf, 100, 20000);
	mid = val;
	if (ret && (val & SEP_PS_DEV_DISABLE)) {
		regmap_update_bits(map, off, SEP_PS_FLAGS | SEP_PS_DEV_DISABLE, 0);
		ret = regmap_read_poll_timeout(map, off, val,
					       FIELD_GET(SEP_PS_ACTUAL, val) == 0xf, 100, 20000);
	}
	if (!ret && (val & SEP_PS_DEV_DISABLE)) {
		regmap_update_bits(map, off, SEP_PS_FLAGS | SEP_PS_DEV_DISABLE, 0);
		regmap_read(map, off, &val);
	}
	dev_dbg(d, "WAKE: SEP PS %#x -> %#x -> %#x (%d)\n", before, mid, val, ret);
	if (!ret && (FIELD_GET(SEP_PS_ACTUAL, val) != 0xf || (val & SEP_PS_DEV_DISABLE)))
		ret = -EIO;
	if (ret)
		sep_pm_power_release(dev);
	return ret;
}

void sep_pm_power_release(void *dev)
{
	struct device *d = dev;
	struct regmap *map;
	u32 off, val = 0;

	if (sep_pm_ps(d, &map, &off))
		return;
	regmap_update_bits(map, off, SEP_PS_FLAGS | SEP_PS_MIN, 0);
	regmap_read(map, off, &val);
	dev_dbg(d, "WAKE: SEP PS minimum dropped: %#x\n", val);
}

static struct device *sep_pm_dart(struct device *d)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_parse_phandle(d->of_node, "iommus", 0);
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	return pdev ? &pdev->dev : NULL;
}

int sep_pm_save_dart(void *dev)
{
	struct device *dart = sep_pm_dart(dev);
	int ret;

	if (!dart)
		return -ENODEV;
	ret = apple_dart_save_island_state(dart);
	put_device(dart);
	return ret;
}

int sep_pm_restore_dart(void *dev)
{
	struct device *dart = sep_pm_dart(dev);
	int ret;

	if (!dart)
		return -ENODEV;
	ret = apple_dart_restore_island_state(dart);
	put_device(dart);
	return ret;
}

int sep_pm_register(void (*event)(bool entering), int (*sleep)(void),
		    int (*wake)(void))
{
	int ret;

	WRITE_ONCE(sep_pm_wake, wake);
	WRITE_ONCE(sep_pm_sleep, sleep);
	WRITE_ONCE(sep_pm_event, event);
	ret = register_pm_notifier(&sep_pm_nb);
	if (ret) {
		WRITE_ONCE(sep_pm_event, NULL);
		WRITE_ONCE(sep_pm_sleep, NULL);
		WRITE_ONCE(sep_pm_wake, NULL);
	}
	return ret;
}

/* Returns only once no notifier call is still running. */
void sep_pm_unregister(void)
{
	unregister_pm_notifier(&sep_pm_nb);
	WRITE_ONCE(sep_pm_event, NULL);
	WRITE_ONCE(sep_pm_sleep, NULL);
	WRITE_ONCE(sep_pm_wake, NULL);
}

static void sep_pm_release_domains(void *data)
{
	struct dev_pm_domain_list *list = data;
	u32 i;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], false);
}

/*
 * The enclave runs from boot to shutdown, and on some SoCs it needs power
 * domains besides its own: once one of them is switched off, it stops
 * reading its mailbox for good. A node that lists more than one domain gets
 * all of them attached here, powered on through device links for as long as
 * the driver is bound, and taken out of system sleep, which would otherwise
 * switch a domain off in the noirq phase once its other users had suspended.
 * A node with a single domain is attached by the platform core as before.
 */
int sep_pm_keep_domains(void *ptr)
{
	struct device *dev = ptr;
	struct dev_pm_domain_attach_data data = {
		.pd_flags = PD_FLAG_DEV_LINK_ON,
	};
	struct dev_pm_domain_list *list;
	int count, ret;
	u32 i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0 && count != -ENOENT)
		return count;
	if (count <= 1)
		return 0;

	ret = devm_pm_domain_attach_list(dev, &data, &list);
	if (ret < 0)
		return ret;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], true);

	return devm_add_action_or_reset(dev, sep_pm_release_domains, list);
}
