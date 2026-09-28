/* SPDX-License-Identifier: MIT */
#include "t6050_pcie_sequence.h"

/* Target: apcie,t6050 GP port0, four ports, lane-cfg0, ge-harvested present.
 * All access remains in caller callbacks, which must enforce SPTM ownership.
 * This does not power-cycle an endpoint or assign a DART SID implicitly. */
static int rmw(const struct t6050_pcie_io *io, unsigned reg, unsigned off,
               u32 clear, u32 set)
{
    u32 v;
    int ret = io->read(io->context, reg, off, &v);
    return ret ? ret : io->write(io->context, reg, off, (v & ~clear) | set);
}

int t6050_pcie_reset_port0(const struct t6050_pcie_io *io)
{
    static const struct { unsigned off; u32 value; } first[] = {
        {0x88,0x110}, {0x100,0xffffffff}, {0x148,0xffffffff}, {0x210,0xffffffff},
        {0x80,0}, {0x84,0}, {0x104,0xfffffff0}, {0x124,0x100}, {0x16c,0},
        {0x13c,0}, {0x800,0x100100}, {0x808,0x1001ff}, {0x82c,0x10000},
    }, last[] = {
        {0x130,0x03020000}, {0x140,0x10}, {0x144,0x253770},
        {0x21c,0}, {0x834,0}, {0x83c,0},
    };
    int ret;
    if (!io || !io->write)
        return -1;
    /* APCIECoreRCGen4Port::_resetPortHardware, native27.0 9c803e8..9c808d4. */
    for (unsigned i=0; i<sizeof(first)/sizeof(first[0]); i++) {
        ret=io->write(io->context,T6050_PCIE_PORT0,first[i].off,first[i].value);
        if (ret) return ret;
    }
    for (unsigned i=0; i<64; i++) {
        ret=io->write(io->context,T6050_PCIE_PORT0,0x3000+4*i,0);
        if (ret) return ret;
    }
    for (unsigned i=0; i<256; i++) {
        ret=io->write(io->context,T6050_PCIE_PORT0,0x3800+4*i,0);
        if (ret) return ret;
    }
    for (unsigned i=0; i<sizeof(last)/sizeof(last[0]); i++) {
        ret=io->write(io->context,T6050_PCIE_PORT0,last[i].off,last[i].value);
        if (ret) return ret;
    }
    return 0;
}

int t6050_pcie_enable_port0(const struct t6050_pcie_io *io, unsigned timeout_us)
{
    u32 v;
    unsigned waited=0;
    int ret;
    if (!io || !io->read || !io->write || !io->poll || !io->delay ||
        !io->apply_port_tunables || !timeout_us)
        return -1;
    ret=t6050_pcie_reset_port0(io);
    if (ret) return ret;
    /* Native T6050 _linkUsesAUS5PHY() is always true. */
    ret=io->read(io->context,T6050_PCIE_PORT0,0x82c,&v);
    if (ret) return ret;
    if (!(v & (1u<<16))) return -2;
    ret=io->apply_port_tunables(io->context,"apcie-config-tunables",T6050_PCIE_PORT0);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0,0x800,0,1);
    if (ret) return ret;
    /* Native9c7ffc8 waits while BOTH reset and idle-ack remain asserted.
     * Target port0/lane-cfg0 masks are1 (9c78b98..bb0 and9c78e08..e20). */
    for (;;) {
        ret=io->read(io->context,T6050_PCIE_PORT0,0xa8,&v);
        if (ret) return ret;
        if (!(v & 1) || !(v & (1u<<16))) break;
        if (waited++ >= timeout_us || waited > 10000) return -3;
        io->delay(io->context,1);
    }
    ret=rmw(io,T6050_PCIE_PORT0,0x82c,0,1); /* release root-port PERST */
    if (ret) return ret;
    ret=io->poll(io->context,T6050_PCIE_PORT0,0xa8,1,1,
                 timeout_us < 20000 ? timeout_us : 20000);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0,0x82c,1u<<16,0);
    if (ret) return ret;
    ret=io->poll(io->context,T6050_PCIE_PORT0,0x804,1,1,timeout_us);
    if (ret) return ret;
    /* Native enablePortHardware keeps APPCLK_AUTO_DIS during config. */
    ret=rmw(io,T6050_PCIE_PORT0,0x800,0,0x100);
    if (ret) return ret;
    return io->poll(io->context,T6050_PCIE_PORT0,0x208,4,0,timeout_us);
}

static int find_pcie_cap(const struct t6050_pcie_io *io, unsigned *cap)
{
    u32 v;
    int ret=io->read(io->context,T6050_PCIE_ECAM,0x34,&v);
    if (ret) return ret;
    unsigned off=v & 0xff;
    for (unsigned i=0; i<48 && off; i++) {
        if (off<0x40 || (off & 3)) return -2;
        ret=io->read(io->context,T6050_PCIE_ECAM,off,&v);
        if (ret) return ret;
        if ((v & 0xff)==0x10) { *cap=off; return 0; }
        off=(v>>8)&0xff;
    }
    return -2;
}

int t6050_pcie_configure_port0(const struct t6050_pcie_io *io, unsigned max_speed)
{
    static const char *const shadow[] = {"pcie-rc-gen3-shadow-tunables",
        "pcie-rc-gen4-shadow-tunables", "pcie-rc-gen5-shadow-tunables"};
    const u32 select[] = {0,1u<<24,0};
    unsigned cap;
    u32 v;
    u16 v16;
    int ret;
    if (!io || !io->read || !io->write || !io->read16 || !io->write16 ||
        !io->apply_port_tunables || max_speed!=3)
        return -1; /* Current target ADT maximum-link-speed is3. */
    ret=find_pcie_cap(io,&cap);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_ECAM,0x8bc,0,1);
    if (ret) return ret;
    ret=io->apply_port_tunables(io->context,"pcie-rc-tunables",T6050_PCIE_ECAM);
    if (ret) goto lock;
    /* Native _applyrcTunables9c82500: selector before each target dataset. */
    for (unsigned i=0; i<3; i++) {
        ret=rmw(io,T6050_PCIE_ECAM,0x890,3u<<24,select[i]);
        if (ret) goto lock;
        ret=io->apply_port_tunables(io->context,shadow[i],T6050_PCIE_ECAM);
        if (ret) goto lock;
    }
    ret=rmw(io,T6050_PCIE_ECAM,0x194,1,1); /* T6050 intermediate-rate training */
    if (ret) goto lock;
    ret=io->read(io->context,T6050_PCIE_ECAM,cap+0x2c,&v);
    if (ret) goto lock;
    if (!(v & (1u<<max_speed))) {ret=-2;goto lock;}
    ret=io->read16(io->context,T6050_PCIE_ECAM,cap+0x30,&v16);
    if (ret) goto lock;
    ret=io->write16(io->context,T6050_PCIE_ECAM,cap+0x30,(v16 & ~0xf)|max_speed);
    if (ret) goto lock;
    /* Native target port0 width1: LINK_MODE1, NUM_LANES1, advertised width1. */
    ret=rmw(io,T6050_PCIE_ECAM,0x710,0x3fu<<16,1u<<16);
    if (ret) goto lock;
    ret=rmw(io,T6050_PCIE_ECAM,0x80c,0x1fu<<8,1u<<8);
    if (ret) goto lock;
    ret=rmw(io,T6050_PCIE_ECAM,cap+0xc,0x3fu<<4,1u<<4);
lock:
    /* Native clears the DBI override at9c81310; do so on error as well. */
    {
        int cleanup=rmw(io,T6050_PCIE_ECAM,0x8bc,1,0);
        if (!ret) ret=cleanup;
    }
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_ECAM,cap+0x10,0,0x400);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_ECAM,0x80c,0,1u<<17);
    if (ret) return ret;
    /* Native _initializeRootComplex tail routes the port interrupt block. */
    return io->write(io->context,T6050_PCIE_PORT0_INTR2AXI,0x80,1);
}

int t6050_pcie_train_port0(const struct t6050_pcie_io *io, unsigned timeout_us)
{
    int ret;
    if (!io || !io->read || !io->write || !io->poll || !io->delay ||
        !io->endpoint_reset || !timeout_us)
        return -1;
    /* initializeRefclkBuffer9c7fc40, mapped PHY lane reg18 for port0. */
    ret=rmw(io,T6050_PCIE_PORT0_PHY,0,0,1);
    if (ret) return ret;
    ret=io->poll(io->context,T6050_PCIE_PORT0_PHY,0,4,4,timeout_us);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0_PHY,0,0,2);
    if (ret) return ret;
    ret=io->poll(io->context,T6050_PCIE_PORT0_PHY,0,8,8,timeout_us);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0_PHY,0,0x10,0);
    if (ret) return ret;
    io->delay(io->context,1);
    ret=rmw(io,T6050_PCIE_PORT0_PHY,0,0,0x200);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0_PHY,0,0,0x400);
    if (ret) return ret;
    io->delay(io->context,100); /* Target t-refclk-to-perst, microseconds. */
    ret=io->endpoint_reset(io->context,false);
    if (ret) return ret;
    ret=rmw(io,T6050_PCIE_PORT0,0x80,0,1);
    if (ret) return ret;
    ret=io->poll(io->context,T6050_PCIE_PORT0,0x208,1,1,timeout_us);
    if (ret) return ret;
    io->delay(io->context,100000); /* Target perst-to-config is milliseconds. */
    return 0;
}
