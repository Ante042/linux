/* SPDX-License-Identifier: MIT */
#include "t6050_pcie_gpio.h"

/* Native27 AppleARMGPIOFunction callFunction 921bcb4: data=logical^flags[1]^1.
 * Target function-perst flags[1]=0, so assertion drives low.
 * setGPIODirection 9218efc, absent ie-fix: clear0x27f, set0x202|data.
 * _safeReconfigure 92197d8 sequences the input-enable bit separately. */
int t6050_pcie_gpio_perst(const struct t6050_gpio_pad_io *io, bool asserted)
{
    u32 old, desired, first, verify;
    int ret;
    if (!io || !io->read || !io->write)
        return -1;
    ret = io->read(io->context, &old);
    if (ret)
        return ret;
    /* A locked pad is not changed; do not duplicate the native panic path. */
    if (old & (1u << 21))
        return -2;
    desired = (old & ~0x27fu) | 0x202u | !asserted;
    first = (desired & ~0x200u) | (old & 0x200u);
    ret = io->write(io->context, first);
    if (ret)
        return ret;
    if (!(old & 0x200u)) {
        ret = io->write(io->context, desired);
        if (ret)
            return ret;
    }
    ret = io->read(io->context, &verify);
    if (ret)
        return ret;
    /* Ignore unrelated input/status bits; verify only configuration touched. */
    return ((verify ^ desired) & 0x27fu) ? -3 : 0;
}
