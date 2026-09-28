/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PCI_APPLE_X1N1_H
#define _LINUX_PCI_APPLE_X1N1_H
#include <linux/errno.h>
#include <linux/kconfig.h>
struct pci_dev;
#if IS_REACHABLE(CONFIG_PCIE_APPLE_X1N1)
int apple_x1n1_pcie_cycle(struct pci_dev *pdev);
#else
static inline int apple_x1n1_pcie_cycle(struct pci_dev *pdev)
{
	return -EOPNOTSUPP;
}
#endif
#endif
