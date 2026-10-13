#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual diagnostic scope, software override and SUSPHY writer."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/usb/dwc3'
core = (base / 'core.c').read_text()
apple = (base / 'dwc3-apple.c').read_text()
header = (base / 'core.h').read_text()
glue = (base / 'glue.h').read_text()


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


probe = block(core, 'int dwc3_core_probe(')
assert probe.index('dwc3_get_properties(dwc)') < probe.index('dwc3_get_software_properties')
assert probe.index('dwc3_get_software_properties') < probe.index('dwc3_core_init(dwc)')
assert 'module_param(diagnostic_j700_u3_awake, bool, 0444);' in apple
assert 'static bool diagnostic_j700_u3_awake;' in apple
assert core.count('reg |= DWC3_GUSB3PIPECTL_SUSPHY;') == 1
assert 'if (enable && !dwc->dis_u3_susphy_quirk)' in core
parts = [block(glue, 'struct dwc3_properties {') + ';']
for name in ('DWC3_GSBUSCFG0_REQINFO_UNSPECIFIED', 'DWC3_GUSB3PIPECTL_SUSPHY',
             'DWC3_GUSB2PHYCFG_SUSPHY'):
    parts.append(re.search(r'^#define ' + name + r'\b[^\n]*', header, re.M).group())
parts += [block(apple, 'static bool dwc3_apple_j700_u3_trial('),
          block(core, 'static void dwc3_get_software_properties('),
          block(core, 'void dwc3_enable_susphy(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define DWC3_GUSB3PIPECTL(i) ((i) * 4)
#define DWC3_GUSB2PHYCFG(i) (16 + (i) * 4)
struct device { void *of_node; struct device *parent; };
struct resource { uint64_t start; };
struct dwc3_apple { bool fixed_hub; struct device *dev; struct resource *mmio_resource; };
struct dwc3 {
    struct device *dev;
    bool dis_u3_susphy_quirk, dis_u2_susphy_quirk, needs_full_reinit;
    uint8_t num_usb3_ports, num_usb2_ports;
    u32 gsbuscfg0_reqinfo, regs[8];
};
static bool diagnostic_j700_u3_awake, j700, t8140;
static bool of_machine_is_compatible(const char *s)
{ assert(!strcmp(s, "apple,j700")); return j700; }
static bool of_device_is_compatible(void *node, const char *s)
{ (void)node; assert(!strcmp(s, "apple,t8140-dwc3")); return t8140; }
static int device_property_read_u16(struct device *d, const char *s, u16 *v)
{ (void)d; (void)s; (void)v; return -1; }
static u32 dwc3_readl(struct dwc3 *d, unsigned int off) { return d->regs[off / 4]; }
static void dwc3_writel(struct dwc3 *d, unsigned int off, u32 v) { d->regs[off / 4] = v; }
'''
suffix = r'''
int main(void)
{
    struct device dev = {0};
    struct resource res = { .start = 0x40a280000ULL };
    struct dwc3_apple a = { .dev = &dev, .mmio_resource = &res };
    unsigned int scopes = 0, writers = 0;
    for (unsigned int flags = 0; flags < 32; flags++) {
        diagnostic_j700_u3_awake = flags & 1;
        j700 = flags & 2; t8140 = flags & 4; a.fixed_hub = flags & 8;
        res.start = flags & 16 ? 0x40a280000ULL : 0x82280000ULL;
        assert(dwc3_apple_j700_u3_trial(&a) == (flags == 31));
        scopes++;
    }
    a.mmio_resource = NULL;
    assert(!dwc3_apple_j700_u3_trial(&a));
    for (unsigned int force = 0; force < 2; force++) {
        for (unsigned int dt = 0; dt < 2; dt++) {
            for (unsigned int u2 = 0; u2 < 2; u2++) {
                struct dwc3 d = { .dev = &dev, .num_usb3_ports = 2, .num_usb2_ports = 2,
                                 .dis_u3_susphy_quirk = dt, .dis_u2_susphy_quirk = u2 };
                struct dwc3_properties p = { .gsbuscfg0_reqinfo = 0x55, .dis_u3_susphy = force };
                dwc3_get_software_properties(&d, &p);
                assert(d.dis_u3_susphy_quirk == (force || dt) && d.dis_u2_susphy_quirk == u2);
                assert(d.gsbuscfg0_reqinfo == 0x55); /* Override precedes the early return too. */
                for (unsigned int enabled = 0; enabled < 2; enabled++) {
                    for (unsigned int repeat = 0; repeat < 3; repeat++) {
                        for (unsigned int i = 0; i < 8; i++) d.regs[i] = 0xa5a5a5a5;
                        dwc3_enable_susphy(&d, enabled);
                        for (unsigned int i = 0; i < 2; i++) {
                            assert(!!(d.regs[i] & DWC3_GUSB3PIPECTL_SUSPHY) == (enabled && !(force || dt)));
                            assert(!!(d.regs[4+i] & DWC3_GUSB2PHYCFG_SUSPHY) == (enabled && !u2));
                            assert((d.regs[i] & ~DWC3_GUSB3PIPECTL_SUSPHY) == (0xa5a5a5a5 & ~DWC3_GUSB3PIPECTL_SUSPHY));
                        }
                        writers++;
                    }
                }
            }
        }
    }
    printf("PASS: %u scope controls, %u actual SUSPHY writer/restore cases; DT/default and USB2 policy retained\n", scopes, writers);
}
'''
with tempfile.TemporaryDirectory(prefix='j700-u3-susphy-') as directory:
    out = Path(directory)
    (out / 'test.c').write_text(prefix + '\n'.join(parts) + suffix)
    cc = shlex.split(os.environ.get('CC', 'clang'))
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-fsanitize=address,undefined', str(out / 'test.c'), '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
