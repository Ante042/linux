/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple Silicon CPU power down without state retention
 */

#ifndef _APPLE_S2R_SLEEP_H
#define _APPLE_S2R_SLEEP_H

#include <linux/compiler.h>
#include <linux/types.h>

void apple_s2r_cpu_off(u64 cyc_ovrd, u64 acc_ovrd, bool flush_l1) __noreturn;
int apple_s2r_cpu_sleep(u64 cyc_ovrd, u64 acc_ovrd, bool flush_l1);

#endif
