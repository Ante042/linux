/* SPDX-License-Identifier: MIT */
#ifndef T6050_PCIE_SEQUENCE_H
#define T6050_PCIE_SEQUENCE_H
#include <linux/types.h>


/* The caller owns mapped ADT resources and SPTM IOFILTER policy. These helpers
 * never translate an arbitrary physical address or bypass protected writes.
 * read/write return 0 on success; poll returns 0 only after the masked value
 * matches, with the supplied finite timeout. */
struct t6050_pcie_io {
    void *context;
    int (*read)(void *, unsigned adt_reg, unsigned offset, u32 *);
    int (*write)(void *, unsigned adt_reg, unsigned offset, u32);
    int (*poll)(void *, unsigned adt_reg, unsigned offset,
                u32 mask, u32 value, unsigned timeout_us);
    int (*enable_clock)(void *, unsigned adt_clock_index);
    int (*apply_tunables)(void *, const char *property, unsigned adt_reg);
    int (*apply_port_tunables)(void *, const char *property, unsigned adt_reg);
    int (*read16)(void *, unsigned adt_reg, unsigned offset, u16 *);
    int (*write16)(void *, unsigned adt_reg, unsigned offset, u16);
    void (*delay)(void *, unsigned usec);
    /* GPIO PERST callback: true asserts, false releases; caller resolves the
     * target ADT function. This is distinct from SMC function-reset. */
    int (*endpoint_reset)(void *, bool asserted);
};

/* Native27.0 AppleT6050PCIe::start pins these indexes, PCs9c6dd94..9c6dde4. */
enum {
    T6050_PCIE_ECAM = 0,
    T6050_PCIE_RC = 1,
    T6050_PCIE_PHY = 2,
    T6050_PCIE_PHY_PHY = 3,
    T6050_PCIE_PHY_IP = 4,
    T6050_PCIE_AXI = 5,
    T6050_PCIE_CLKGEN = 6,
    T6050_PCIE_GP_AUS5_TOP = 10,
    T6050_PCIE_GP_AUS5_AON_TOP = 11,
    T6050_PCIE_PORT0 = 16,
    T6050_PCIE_PORT0_PHY = 18,
    T6050_PCIE_PORT0_INTR2AXI = 19,
    T6050_PCIE_PORT_STRIDE = 6,
};

/* Implements the decoded Gen5 PHY tail after power/clocks/tunables and lane
 * configuration are established. It is NOT a full controller or link init.
 * Native27.0 GPInit9c7dc44 and9c7de50, Gen5Init9c73fa8. */
int t6050_pcie_finish_gen5_phy(const struct t6050_pcie_io *io, unsigned timeout_us);

/* Four-port T6050 with the ge-harvested property present, as on this J714s.
 * enable_clock resolves ADT clock indices, enables parent dependencies, and
 * must return failure if its required power/clock state is not established.
 * apply_tunables uses this target's ADT data and returns 0 for an absent optional
 * property (the native common-tunables call also accepts absent data).
 * This prepares the GP PHY; root-port/PERST/link training are separate. */
int t6050_pcie_prepare_gp_phy(const struct t6050_pcie_io *io, unsigned lane_cfg,
                              unsigned timeout_us);

/* After GP PHY preparation, enable native RC/PTM and wait for RC+0x58 bit0.
 * Still does not release an endpoint or train an individual port. */
int t6050_pcie_finish_gp_controller(const struct t6050_pcie_io *io, unsigned timeout_us);

/* Native getLinkUp9c837c8: port+0x208 BIT0 (not J813 helper's BIT12). */
int t6050_pcie_get_n1_link(const struct t6050_pcie_io *io, bool *up);

/* Pure native RID→SID format; caller must allocate a free SPTM-valid SID and
 * keep bus mastering stopped while installing routing. Native uses entries1..15
 * and slot==SID. This function does not choose a SID or mutate hardware. */
int t6050_pcie_n1_rid_entry(unsigned slot, unsigned sid, unsigned rid,
                          u32 *offset, u32 *value);
/* Native port0 phases; endpoint must be held in reset before calling reset/
 * enable/configure, so clearing RID/MSI routes cannot affect active DMA.
 * The host must install explicit SPTM-valid DART routing before train_port0.
 * No implicit SID allocation or diagnostic polling transport is introduced. */
int t6050_pcie_reset_port0(const struct t6050_pcie_io *io);
int t6050_pcie_enable_port0(const struct t6050_pcie_io *io, unsigned timeout_us);
int t6050_pcie_configure_port0(const struct t6050_pcie_io *io, unsigned max_speed);
int t6050_pcie_train_port0(const struct t6050_pcie_io *io, unsigned timeout_us);
int t6050_pcie_configure_msi_port0(const struct t6050_pcie_io *io);
#endif
