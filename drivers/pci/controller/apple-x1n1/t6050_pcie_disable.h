/* SPDX-License-Identifier: MIT */
#ifndef T6050_PCIE_DISABLE_H
#define T6050_PCIE_DISABLE_H
#include "t6050_pcie_sequence.h"
/* See t6050_pcie_disable.c for the mandatory quiescence/PME/PERST contract. */
int t6050_pcie_disable_port0(const struct t6050_pcie_io *io,
                            int (*force_available)(void *, bool),
                            unsigned timeout_us);
#endif
