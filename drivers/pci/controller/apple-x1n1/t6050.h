/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _X1N1_T6050_H
#define _X1N1_T6050_H
#include <linux/pci.h>
#include <linux/platform_device.h>
struct t6050_pcie;
struct t6050_pcie *t6050_pcie_init(struct platform_device *pdev);
int t6050_pcie_cycle(struct t6050_pcie *pcie, struct pci_dev *endpoint);
#endif
