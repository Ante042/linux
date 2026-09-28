/* SPDX-License-Identifier: MIT */
#include "t6050_pcie_sequence.h"
/* Native27 configMSIRange9c8799c, target ADT msi-address fffff000,
 * port0 msi-vector-base0/#msi-vectors32. All endpoints must have MSI and bus
 * mastering disabled during setup. This only installs routing; Linux owns
 * MSI vector allocation and AIC enable/mask/EOI. */
int t6050_pcie_configure_msi_port0(const struct t6050_pcie_io *io)
{
    int ret;
    if (!io || !io->write)
        return -1;
    ret=io->write(io->context,T6050_PCIE_PORT0,0x124,1);
    if(ret)return ret;
    ret=io->write(io->context,T6050_PCIE_PORT0,0x16c,0xfffff000);
    if(ret)return ret;
    ret=io->write(io->context,T6050_PCIE_PORT0,0x170,0);
    if(ret)return ret;
    for(unsigned i=0;i<32;i++) {
        ret=io->write(io->context,T6050_PCIE_PORT0,0x3800+4*i,0x80000000|i);
        if(ret)return ret;
    }
    return 0;
}
