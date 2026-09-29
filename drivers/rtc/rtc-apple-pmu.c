// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SPMI PMU RTC with wake alarm
 *
 * The PMU keeps a free-running 48-bit counter at 65536 Hz (measured on J714s:
 * 65547 ticks/s against NTP). Wall time in 1/32768 s is the counter halved
 * plus a signed offset in PMU scratch (info-clock_offset), the convention of
 * the value macOS/iBoot leave there: it gave the correct date on J714s. The
 * alarm register holds the counter's whole seconds (counter >> 16); an alarm
 * of (counter >> 16) + 6 raised the event and SPMI interrupt 2 six seconds
 * later. Programming follows macOS 27's
 * AppleDialogSPMIPMURTC::programRTCAlarm(): clear a pending event, write the
 * alarm, unmask the alarm interrupt, then set the enable bit next to the
 * monitor bit in the alarm control register. All registers are PMU NVMEM
 * cells, so every access sleeps.
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#define RTC_BYTES	6
#define RTC_BITS	(8 * RTC_BYTES)
/* Wall time and offset are in 1/32768 s; the counter runs twice as fast. */
#define RTC_SEC_SHIFT	15

/* Masks from the J714s ADT (pmu,abbey): info-rtc_alarm_{event,ctrl_en,monitor,}_mask. */
#define ALARM_EVENT	BIT(1)
#define ALARM_MONITOR	BIT(0)
#define ALARM_ENABLE	BIT(6)
#define ALARM_IRQ_MASK	BIT(0)

struct apple_pmu_rtc {
	struct device *dev;
	struct rtc_device *rtc;
	struct nvmem_cell *counter, *offset, *alarm, *ctrl, *irq_mask;
	struct mutex lock;
};

static int apple_pmu_rtc_read_cell(struct nvmem_cell *cell, void *val, size_t len)
{
	size_t got;
	void *buf = nvmem_cell_read(cell, &got);

	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (got < len) {
		kfree(buf);
		return -EIO;
	}
	memcpy(val, buf, len);
	kfree(buf);
	return 0;
}

static int apple_pmu_rtc_write_u8(struct nvmem_cell *cell, u8 val)
{
	int ret = nvmem_cell_write(cell, &val, 1);

	return ret < 0 ? ret : 0;
}

/* Wall time in 1/32768 s: counter / 2 + offset. */
static int apple_pmu_rtc_ticks(struct apple_pmu_rtc *prtc, u64 *ticks, u64 *offset)
{
	u64 a = 0, b = 0, off = 0;
	int ret, tries = 3;

	do {
		a = b;
		ret = apple_pmu_rtc_read_cell(prtc->counter, &b, RTC_BYTES);
		if (ret)
			return ret;
	} while (a >> RTC_SEC_SHIFT != b >> RTC_SEC_SHIFT && --tries);

	/* A bad SPMI read here once put the clock 143 days ahead: insist on
	 * two identical reads of the offset. */
	for (tries = 3; tries; tries--) {
		u64 check = 0;

		ret = apple_pmu_rtc_read_cell(prtc->offset, &off, RTC_BYTES);
		if (!ret)
			ret = apple_pmu_rtc_read_cell(prtc->offset, &check, RTC_BYTES);
		if (ret)
			return ret;
		if (off == check)
			break;
	}
	if (!tries)
		return -EIO;
	*ticks = (b >> 1) + off;
	if (offset)
		*offset = off;
	return 0;
}

static int apple_pmu_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ticks;
	int ret = apple_pmu_rtc_ticks(prtc, &ticks, NULL);

	if (ret)
		return ret;
	rtc_time64_to_tm(sign_extend64(ticks, RTC_BITS - 1) >> RTC_SEC_SHIFT, tm);
	return 0;
}

static int apple_pmu_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ctr = 0, off;
	int ret = apple_pmu_rtc_read_cell(prtc->counter, &ctr, RTC_BYTES);

	if (ret)
		return ret;
	/* The set second begins now. */
	off = (rtc_tm_to_time64(tm) << RTC_SEC_SHIFT) - (ctr >> 1);
	ret = nvmem_cell_write(prtc->offset, &off, RTC_BYTES);
	return ret < 0 ? ret : 0;
}

/* Arm or disarm the programmed alarm; caller holds prtc->lock. */
static int apple_pmu_rtc_program(struct apple_pmu_rtc *prtc, bool enable)
{
	u8 ctrl = 0;
	int ret = apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1);

	if (ret)
		return ret;
	if (ctrl & ALARM_EVENT) {
		ret = apple_pmu_rtc_write_u8(prtc->ctrl, ALARM_EVENT);
		if (ret)
			return ret;
	}
	ret = apple_pmu_rtc_write_u8(prtc->irq_mask, enable ? 0 : ALARM_IRQ_MASK);
	if (ret)
		return ret;
	return apple_pmu_rtc_write_u8(prtc->ctrl,
				      ALARM_MONITOR | (enable ? ALARM_ENABLE : 0));
}

static int apple_pmu_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ticks, off;
	u32 alarm = 0;
	u8 ctrl = 0;
	int ret;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_ticks(prtc, &ticks, &off);
	if (!ret)
		ret = apple_pmu_rtc_read_cell(prtc->alarm, &alarm, sizeof(alarm));
	if (!ret)
		ret = apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1);
	mutex_unlock(&prtc->lock);
	if (ret)
		return ret;

	wkalrm->enabled = !!(ctrl & ALARM_ENABLE);
	wkalrm->pending = !!(ctrl & ALARM_EVENT);
	/* The alarm is in counter seconds; one counter second is 1 s. */
	ticks = ((u64)alarm << RTC_SEC_SHIFT) + off;
	rtc_time64_to_tm(sign_extend64(ticks, RTC_BITS - 1) >> RTC_SEC_SHIFT,
			 &wkalrm->time);
	return 0;
}

static int apple_pmu_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 off = 0, ticks;
	int ret;

	u32 alarm;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_read_cell(prtc->offset, &off, RTC_BYTES);
	if (!ret) {
		/* Counter seconds; round up so the alarm never fires early. */
		ticks = (rtc_tm_to_time64(&wkalrm->time) << RTC_SEC_SHIFT) - off;
		ticks &= GENMASK_ULL(RTC_BITS - 1, 0);
		alarm = DIV_ROUND_UP_ULL(ticks, 1ULL << RTC_SEC_SHIFT);
		ret = nvmem_cell_write(prtc->alarm, &alarm, sizeof(alarm));
		ret = ret < 0 ? ret : 0;
	}
	if (!ret)
		ret = apple_pmu_rtc_program(prtc, wkalrm->enabled);
	mutex_unlock(&prtc->lock);
	return ret;
}

static int apple_pmu_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_program(prtc, enabled);
	mutex_unlock(&prtc->lock);
	return ret;
}

static irqreturn_t apple_pmu_rtc_irq(int irq, void *data)
{
	struct apple_pmu_rtc *prtc = data;
	u8 ctrl = 0;

	mutex_lock(&prtc->lock);
	if (apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1) || !(ctrl & ALARM_EVENT)) {
		mutex_unlock(&prtc->lock);
		return IRQ_NONE;
	}
	/* One-shot, as the RTC core expects: clear the event and disarm. */
	apple_pmu_rtc_program(prtc, false);
	mutex_unlock(&prtc->lock);
	rtc_update_irq(prtc->rtc, 1, RTC_AF | RTC_IRQF);
	return IRQ_HANDLED;
}

static const struct rtc_class_ops apple_pmu_rtc_ops = {
	.read_time = apple_pmu_rtc_read_time,
	.set_time = apple_pmu_rtc_set_time,
	.read_alarm = apple_pmu_rtc_read_alarm,
	.set_alarm = apple_pmu_rtc_set_alarm,
	.alarm_irq_enable = apple_pmu_rtc_alarm_irq_enable,
};

static int apple_pmu_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmu_rtc *prtc;
	int i, irq, nirq, ret;

	prtc = devm_kzalloc(dev, sizeof(*prtc), GFP_KERNEL);
	if (!prtc)
		return -ENOMEM;
	prtc->dev = dev;
	mutex_init(&prtc->lock);
	platform_set_drvdata(pdev, prtc);

	struct {
		struct nvmem_cell **cell;
		const char *name;
	} cells[] = {
		{ &prtc->counter, "counter" },
		{ &prtc->offset, "rtc_offset" },
		{ &prtc->alarm, "alarm" },
		{ &prtc->ctrl, "alarm_ctrl" },
		{ &prtc->irq_mask, "irq_mask" },
	};
	for (i = 0; i < ARRAY_SIZE(cells); i++) {
		*cells[i].cell = devm_nvmem_cell_get(dev, cells[i].name);
		if (IS_ERR(*cells[i].cell))
			return dev_err_probe(dev, PTR_ERR(*cells[i].cell),
					     "missing NVMEM cell %s\n", cells[i].name);
	}

	prtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(prtc->rtc))
		return PTR_ERR(prtc->rtc);
	prtc->rtc->ops = &apple_pmu_rtc_ops;
	prtc->rtc->range_min = S64_MIN >> (RTC_SEC_SHIFT + (64 - RTC_BITS));
	prtc->rtc->range_max = S64_MAX >> (RTC_SEC_SHIFT + (64 - RTC_BITS));

	/* The PMU's SPMI interrupt lines (ADT order). The alarm arrives on the
	 * first (line 2 on J714s); the handler checks the event bit either way. */
	nirq = platform_irq_count(pdev);
	for (i = 0; i < nirq; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;
		ret = devm_request_threaded_irq(dev, irq, NULL, apple_pmu_rtc_irq,
						IRQF_ONESHOT | IRQF_SHARED,
						dev_name(dev), prtc);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request IRQ %d\n", irq);
		if (i == 0)
			dev_pm_set_wake_irq(dev, irq);
	}
	if (nirq > 0)
		device_init_wakeup(dev, true);
	else
		clear_bit(RTC_FEATURE_ALARM, prtc->rtc->features);

	return devm_rtc_register_device(prtc->rtc);
}

static const struct of_device_id apple_pmu_rtc_of_match[] = {
	{ .compatible = "apple,pmu-rtc" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_pmu_rtc_of_match);

static struct platform_driver apple_pmu_rtc_driver = {
	.driver = {
		.name = "apple-pmu-rtc",
		.of_match_table = apple_pmu_rtc_of_match,
	},
	.probe = apple_pmu_rtc_probe,
};
module_platform_driver(apple_pmu_rtc_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SPMI PMU RTC with wake alarm");
