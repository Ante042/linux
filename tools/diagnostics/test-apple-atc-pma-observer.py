#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test actual observational helper scope and read addresses, never PHY readiness."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/phy/apple/atc.c').read_text()


def function(name):
    match = re.search(r'^static [^\n]+\b' + name + r'\(', source, re.M)
    start = match.start()
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


functions = function('core_read32') + '\n' + function('atcphy_observe_pmas')
assert not re.search(r'\b(?:writel|mask32|core_mask32|core_set32|core_clear32|udelay|msleep|fsleep)\s*\(', functions)
# Verify the production debug macro guards argument evaluation before function call.
header = (root / 'include/linux/dev_printk.h').read_text()
assert 'if (DYNAMIC_DEBUG_BRANCH(descriptor) &&' in header
assert '__ratelimit(&_rs))' in header
assert '__dynamic_dev_dbg(&descriptor, dev, dev_fmt(fmt),' in header
print('Actual observer SHA256:', hashlib.sha256(functions.encode()).hexdigest(), flush=True)
defines = '\n'.join(re.findall(r'^#define [^\n]*(?:\\\n[^\n]*)*', source, re.M))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
enum atcphy_mode { APPLE_ATCPHY_MODE_OFF, APPLE_ATCPHY_MODE_USB2,
                  APPLE_ATCPHY_MODE_USB3, APPLE_ATCPHY_MODE_USB3_DP };
struct resource { uint64_t start, end; };
static uint64_t resource_size(struct resource *r) { return r->end - r->start + 1; }
struct hw { bool has_t8140_dp; u32 aciophy_lane_mode, aciophy_crossbar; };
struct apple_atcphy {
    struct hw *hw;
    struct { unsigned char *core, *pipehandler; } regs;
    struct { struct resource *core; } res;
    void *dev;
    bool fixed_usb2, host_active, swap_lanes, mode_dirty;
    enum atcphy_mode mode;
    unsigned int pipe_state, dp_owned_pmas, dp_idle_pmas;
    int lock;
};
static u32 core_regs[0x4c000 / 4], pipe_regs[0x4000 / 4];
static bool is_j700, debug_enabled;
static unsigned int read_offsets[22], reads, records, rate_remaining;
static bool of_machine_is_compatible(const char *s)
{ assert(!strcmp(s, "apple,j700")); return is_j700; }
#define lockdep_assert_held(lock) assert(*(lock) == 1)
static u32 readl(void *address)
{
    uintptr_t a = (uintptr_t)address, c = (uintptr_t)core_regs, p = (uintptr_t)pipe_regs;
    assert(reads < ARRAY_SIZE(read_offsets));
    if (a >= c && a < c + sizeof(core_regs)) read_offsets[reads++] = a - c;
    else { assert(a >= p && a < p + sizeof(pipe_regs)); read_offsets[reads++] = 0x100000 + a - p; }
    return *(u32 *)address;
}
/* Same argument-evaluation guard as the production dynamic-debug/rate-limit macro. */
#define dev_dbg_ratelimited(dev, fmt, ...) do { \
    if (debug_enabled && rate_remaining) { \
        rate_remaining--; records++; (void)(dev); printf(fmt, __VA_ARGS__); \
    } \
} while (0)
'''
controls = r'''
static struct hw hw;
static struct resource resource;
static struct apple_atcphy p;
static void initial(void)
{
    hw = (struct hw) { true, ACIOPHY_LANE_MODE_T8122, ACIOPHY_CROSSBAR_T8122 };
    resource = (struct resource) { 0x40b000000ULL, 0x40b04bfffULL };
    p = (struct apple_atcphy) {
        .hw = &hw, .regs.core = (unsigned char *)core_regs,
        .regs.pipehandler = (unsigned char *)pipe_regs, .res.core = &resource,
        .fixed_usb2 = true, .host_active = true, .mode = APPLE_ATCPHY_MODE_USB3_DP,
        .swap_lanes = true, .lock = 1,
    };
    reads = records = 0; rate_remaining = 10; debug_enabled = is_j700 = true;
}
static int compare_uint(const void *a, const void *b)
{
    unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;
    return (x > y) - (x < y);
}
int main(void)
{
    const unsigned int expected[] = {
        0xc, 0x60, 0x64, 0x200, 0x9244, 0xb068, 0xb06c, 0xd004, 0xd010,
        0x10244, 0x12068, 0x1206c, 0x14004, 0x14010, 0x17000, 0x17080,
        0x17088, 0x1708c, 0x20000, 0x20008, 0x10000c, 0x100020,
    };
    for (unsigned int guard = 0; guard < 9; guard++) {
        initial();
        switch (guard) {
        case 0: debug_enabled = false; break;
        case 1: rate_remaining = 0; break;
        case 2: hw.has_t8140_dp = false; break;
        case 3: p.fixed_usb2 = false; break;
        case 4: p.host_active = false; break;
        case 5: p.mode = APPLE_ATCPHY_MODE_OFF; break;
        case 6: is_j700 = false; break;
        case 7: p.res.core = NULL; break;
        case 8: resource.start++; break;
        }
        atcphy_observe_pmas(&p, "bist-before", p.mode);
        assert(!reads && !records);
    }
    initial(); resource.end = resource.start + ATCPHY_MISC + sizeof(u32) - 2;
    atcphy_observe_pmas(&p, "bist-before", p.mode);
    assert(!reads && !records);
    const char *stages[] = { "mux-before", "safe-before", "safe-after",
                            "lanes-after", "bist-before", "bist-after", "pipe-after" };
    for (unsigned int stage = 0; stage < ARRAY_SIZE(stages); stage++) {
        initial();
        atcphy_observe_pmas(&p, stages[stage], p.mode);
        assert(reads == ARRAY_SIZE(expected) && records == 1);
        qsort(read_offsets, reads, sizeof(read_offsets[0]), compare_uint);
        assert(!memcmp(read_offsets, expected, sizeof(expected)));
    }
    initial();
    for (unsigned int i = 0; i < 12; i++) {
        reads = 0;
        atcphy_observe_pmas(&p, "bist-after", p.mode);
        assert(reads == (i < 10 ? ARRAY_SIZE(expected) : 0));
    }
    assert(records == 10 && rate_remaining == 0);
    puts("PASS: disabled/rate-limited and ten scope/resource guards perform zero reads; all stages read exactly 22 approved offsets; no writes");
    return 0;
}
'''
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='apple-atc-observer-') as temporary:
    out = args.out or Path(temporary)
    out.mkdir(parents=True, exist_ok=True)
    unit = out / 'observer.c'
    unit.write_text(prefix + defines + '\n' + functions + '\n' + controls)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(out / 'observer')], check=True)
    subprocess.run([str(out / 'observer')], check=True)
