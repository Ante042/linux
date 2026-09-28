/* SPDX-License-Identifier: MIT */
#ifndef T6050_PCIE_GPIO_H
#define T6050_PCIE_GPIO_H
#include <linux/types.h>

/* Callbacks access the resolved GPIO0 pin80 pad (target PA0x29c000140).
 * They must preserve SPTM ownership and report guarded access exceptions.
 * No physical address mapping or protection bypass occurs here. */
struct t6050_gpio_pad_io {
    void *context;
    int (*read)(void *, u32 *);
    int (*write)(void *, u32);
};
int t6050_pcie_gpio_perst(const struct t6050_gpio_pad_io *, bool asserted);
#endif
