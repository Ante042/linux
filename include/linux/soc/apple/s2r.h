/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_S2R_H
#define _LINUX_SOC_APPLE_S2R_H
#include <linux/types.h>

struct apple_rtkit;

/* Ops of the PMP hook: restart the stopped PMP, or idle and stop it for sleep */
#define APPLE_S2R_PMP_START	0
#define APPLE_S2R_PMP_STOP	1

#if IS_ENABLED(CONFIG_APPLE_S2R)
/* The PMP driver is a module; s2r stops and restarts it through this hook. */
void apple_s2r_set_pmp_hook(int (*sleep)(int op));
/* The AOP driver hands over its RTKit for the AP power state around sleep, NULL on unbind. */
void apple_s2r_set_aop_rtkit(struct apple_rtkit *rtk);
/* The GPU driver does the same for the GPU firmware. */
void apple_s2r_set_gpu_rtkit(struct apple_rtkit *rtk);
/* Whether this suspend goes to deep sleep (S2R), also from PM_SUSPEND_PREPARE on. */
bool apple_s2r_deep_sleep(void);
/* Whether the SoC lost power in the sleep that is ending: false for one that was given up. */
bool apple_s2r_power_cut(void);
/* The display driver stopped this coprocessor for deep sleep: see apple_s2r_finish(). */
void apple_s2r_display_parked(struct device_node *np);
#else
static inline void apple_s2r_set_pmp_hook(int (*sleep)(int op)) { }
static inline void apple_s2r_set_aop_rtkit(struct apple_rtkit *rtk) { }
static inline void apple_s2r_set_gpu_rtkit(struct apple_rtkit *rtk) { }
static inline bool apple_s2r_deep_sleep(void) { return false; }
static inline bool apple_s2r_power_cut(void) { return false; }
static inline void apple_s2r_display_parked(struct device_node *np) { }
#endif
#endif
