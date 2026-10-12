#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production J700 AUX transitions against a bounded register mock."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[2] / 'drivers/phy/apple/atc.c'
text = source.read_text()
parts = [line for line in text.splitlines() if line.startswith('#define J700_AUX_')]
for name in ('j700_aux_update', 'j700_aux_enable', 'j700_aux_disable'):
    start = text.index('static int ' + name + '(')
    begin = text.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    parts.append(text[start:end])
production = '\n'.join(parts)
print('Production transition SHA256:', hashlib.sha256(production.encode()).hexdigest())
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
struct apple_atcphy { struct { unsigned char *core; } regs; bool aux_power_requested; };
struct event { u32 offset, mask, value; } events[32];
static u32 registers[0x18000 / 4];
static unsigned int writes, reject;
static void core_mask32(struct apple_atcphy *phy, u32 offset, u32 mask, u32 value)
{
    assert(offset < sizeof(registers) && offset % 4 == 0 && writes < 32);
    assert(!(value & ~mask));
    events[writes] = (struct event){offset, mask, value};
    writes++;
    if (writes != reject)
        registers[offset / 4] = (registers[offset / 4] & ~mask) | value;
    (void)phy;
}
static void usleep_range(unsigned int low, unsigned int high)
{
    assert(low && high >= low);
}
#define readl_poll_timeout_atomic(address, value, condition, interval, timeout) \
    ({ (value) = *(u32 *)(address); (condition) ? 0 : -110; })
'''
suffix = r'''
static void initial(struct apple_atcphy *phy)
{
    memset(registers, 0, sizeof(registers));
    memset(events, 0, sizeof(events));
    registers[J700_AUX_SUPPLY_CONTROL / 4] = 0x80000200;
    registers[J700_AUX_ANALOG_CONTROL / 4] = 0x40000001;
    *phy = (struct apple_atcphy){ .regs.core = (unsigned char *)registers };
    writes = reject = 0;
}
int main(void)
{
    struct apple_atcphy phy;
    initial(&phy);
    assert(j700_aux_disable(&phy) == 0 && writes == 0);
    assert(j700_aux_enable(&phy) == 0 && phy.aux_power_requested && writes == 4);
    assert(registers[J700_AUX_SUPPLY_CONTROL / 4] == 0x8000010f);
    assert(registers[J700_AUX_ANALOG_CONTROL / 4] == 0x40000000);
    assert(events[0].mask == 3 && events[1].mask == 12);
    assert(events[2].mask == 0x300 && events[2].value == 0x100);
    assert(events[3].offset == J700_AUX_ANALOG_CONTROL);
    writes = 0;
    assert(j700_aux_disable(&phy) == 0 && !phy.aux_power_requested && writes == 4);
    assert(events[0].offset == J700_AUX_ANALOG_CONTROL);
    assert(events[1].mask == 0x300 && events[2].mask == 8 && events[3].mask == 2);
    assert(registers[J700_AUX_SUPPLY_CONTROL / 4] == 0x80000305);
    assert(registers[J700_AUX_ANALOG_CONTROL / 4] == 0x40000001);
    for (unsigned int fault = 1; fault <= 4; fault++) {
        initial(&phy);
        reject = fault;
        assert(j700_aux_enable(&phy) == -110 && phy.aux_power_requested);
        reject = 0;
        writes = 0;
        assert(j700_aux_disable(&phy) == 0 && !phy.aux_power_requested);
        initial(&phy);
        assert(j700_aux_enable(&phy) == 0);
        writes = 0;
        reject = fault;
        assert(j700_aux_disable(&phy) == -110 && phy.aux_power_requested);
    }
    puts("PASS: sequencing, unrelated bits, every failed phase, retained cleanup ownership");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-atc-aux-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'cc'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1']
    if os.environ.get('ATC_SANITIZERS') == '1':
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
