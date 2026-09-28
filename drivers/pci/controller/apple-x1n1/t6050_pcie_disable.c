/* SPDX-License-Identifier: MIT */
#include "t6050_pcie_disable.h"

static int update(const struct t6050_pcie_io *io, unsigned reg, unsigned off,
                  u32 clear, u32 set)
{
    u32 v;
    int ret = io->read(io->context, reg, off, &v);
    return ret ? ret : io->write(io->context, reg, off, (v & ~clear) | set);
}

/* Native27 APCIECoreRCGen4Port::disablePortHardware, T6050 AUS5 port0 branch.
 * Caller has stopped endpoint DMA, requested PME/L2, disabled bridge command
 * decode and endpoint access, and asserted GPIO PERST. DART force_available
 * must implement the legal native DART power transition, not a guessed MMIO
 * bit. No cleanup tries to restart DMA after an error. */
int t6050_pcie_disable_port0(const struct t6050_pcie_io *io,
                            int (*force_available)(void *, bool),
                            unsigned timeout_us)
{
    u32 v;
    unsigned elapsed;
    int ret;
    if (!io || !io->read || !io->write || !io->poll || !io->delay ||
        !force_available || !timeout_us)
        return -1;
    ret = force_available(io->context, false); /* Fact at native9c8140c..43c */
    if (ret) return ret;
    /* Native first checks804 bit2, then polls820 bit2 until it is SET. */
    ret = io->read(io->context, T6050_PCIE_PORT0, 0x804, &v);
    if (ret) return ret;
    if (!(v & 4)) {
        ret = io->poll(io->context, T6050_PCIE_PORT0, 0x820, 4, 4,
                       timeout_us < 10000 ? timeout_us : 10000);
        if (ret) return ret;
    }
    /* Native9c81560 waits only while BOTH counters are nonzero. */
    for (elapsed = 0;; elapsed++) {
        ret = io->read(io->context, T6050_PCIE_PORT0, 0x820, &v);
        if (ret) return ret;
        if (!(v & 0x1ff) || !(v & 0xff0000)) break;
        if (elapsed >= timeout_us || elapsed >= 10000) return -3;
        io->delay(io->context, 1);
    }
    ret = update(io, T6050_PCIE_PORT0, 0x13c, 0, 0x100);
    if (ret) return ret;
    /* Native9c816d4 exits when LTSSM is NOT0x14; J813 helper differs. */
    for (elapsed = 0;; elapsed += 5) {
        ret = io->read(io->context, T6050_PCIE_ECAM, 0x728, &v);
        if (ret) return ret;
        if ((v & 0x3f) != 0x14) break;
        if (elapsed >= timeout_us || elapsed >= 5000) return -3;
        io->delay(io->context, 5);
    }
    /* AUS5 skips the legacy208 bit12 wait entirely. Native9c81a5c onward. */
    ret = io->poll(io->context, T6050_PCIE_PORT0, 0xa8, 1, 1,
                   timeout_us < 10000 ? timeout_us : 10000);
    if (ret) return ret;
    ret = update(io, T6050_PCIE_PORT0, 0x82c, 0, 1u << 24);
    if (ret) return ret;
    ret = io->poll(io->context, T6050_PCIE_PORT0, 0xa8, 1u << 16, 1u << 16,
                   timeout_us < 2000 ? timeout_us : 2000);
    if (ret) return ret;
    ret = update(io, T6050_PCIE_PORT0, 0x82c, 0, 1u << 16);
    if (ret) return ret;
    io->delay(io->context, 1);
    ret = update(io, T6050_PCIE_PORT0, 0x82c, 1, 0);
    if (ret) return ret;
    io->delay(io->context, 1);
    ret = update(io, T6050_PCIE_PORT0, 0x82c, 1u << 24, 0);
    if (ret) return ret;
    ret = update(io, T6050_PCIE_PORT0, 0x13c, 0x100, 0);
    if (ret) return ret;
    static const u32 clear[] = {1u << 31, 1u << 10, 1u << 9, 0, 1, 2};
    for (unsigned i = 0; i < sizeof(clear)/sizeof(clear[0]); i++) {
        ret = update(io, T6050_PCIE_PORT0_PHY, 0, clear[i], i == 3 ? 0x10 : 0);
        if (ret) return ret;
    }
    ret = io->write(io->context, T6050_PCIE_PORT0, 0x100, (~0U));
    if (ret) return ret;
    ret = update(io, T6050_PCIE_PORT0, 0x800, 1, 0);
    if (ret) return ret;
    /* Native final RUN loop is unbounded; propagate a bounded timeout here. */
    return io->poll(io->context, T6050_PCIE_PORT0, 0x804, 1, 0, timeout_us);
}
