#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check actual fixed-hub USB3 reset ordering, failure ownership and parent control.

The fixture models register writes and injected errors, not electrical recovery.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = root / 'tools/diagnostics/test-apple-atc-parking.py'
ns = {'__file__': str(fixture)}
exec(fixture.read_text().split('with tempfile.TemporaryDirectory')[0], ns)
production = ns['production']
reset_block = '''\t/*
\t * Fixed-hub SAFE keeps USB2 up rather than taking the block OFF.
\t * With PIPE parked and DP owners released, reinitialize USB3 before
\t * programming its lanes; the existing sequence releases this reset.
\t */
\tif (atcphy->hw->has_t8140_dp && atcphy->fixed_usb2)
\t\tcore_clear32(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_PHY_RESET_N);

'''
assert production.count(reset_block) == 1
production = production.replace('static u32 registers[', '''static unsigned int warm_reset_edges, warm_unreset_lanes, warm_ownership_errors;
static u32 registers[''', 1)
production = production.replace('    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', '''    if (offset == ATCPHY_POWER_CTRL && (mask & ATCPHY_POWER_PHY_RESET_N) &&
        !(value & ATCPHY_POWER_PHY_RESET_N)) {
        warm_reset_edges++;
        if (p->fixed_usb2 && p->hw->has_t8140_dp &&
            (p->pipe_state != ATCPHY_PIPEHANDLER_STATE_DUMMY || p->dp_owned_pmas ||
             p->dp_idle_pmas || p->aux_power_requested)) warm_ownership_errors++;
    }
    if (offset == p->hw->aciophy_lane_mode && p->fixed_usb2 && p->hw->has_t8140_dp &&
        (registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N)) warm_unreset_lanes++;
    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;''', 1)
controls = r'''
static void warm_initial(struct apple_atcphy *p)
{
    orientation_initial(p, true);
    registers[ATCPHY_POWER_CTRL / 4] |= ATCPHY_POWER_PHY_RESET_N;
    warm_reset_edges = warm_unreset_lanes = warm_ownership_errors = 0;
}
static void warm_roundtrip(void)
{
    bool failed = false;
    for (unsigned int first = 0; first < 2; first++) {
        struct apple_atcphy p;
        const struct phy_configure_opts_dp rate = {
            .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
        };
        warm_initial(&p);
        for (unsigned int attach = 0; attach < 3; attach++) {
            orient(&p, first ^ (attach & 1));
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && !p.mode_dirty);
            assert(j700_dp_configure(&p, &rate) == 0);
            unsigned int edges = warm_reset_edges;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(warm_reset_edges == edges); /* cached same-mode must not reset */
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == 0);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && p.host_active);
            assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
            assert(registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N);
            assert(!reset_writes && !usb2_writes);
        }
        assert(usb2_on == 6); /* no extra USB2 power-on call introduced */
        printf("first=%u reset_assertions=%u unreset_lane_writes=%u ownership_errors=%u\n",
               first, warm_reset_edges, warm_unreset_lanes, warm_ownership_errors);
        failed |= warm_reset_edges != 6 || warm_unreset_lanes || warm_ownership_errors;
    }
    fflush(stdout);
    assert(!failed);
}
static void warm_failure_controls(void)
{
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate = {
        .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
    };
    warm_initial(&p);
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(j700_dp_configure(&p, &rate) == 0);
    unsigned int edges = warm_reset_edges, owned = p.dp_owned_pmas;
    stuck_ack = true;
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == -ETIMEDOUT);
    assert(warm_reset_edges == edges && p.dp_owned_pmas == owned && p.mode_dirty);
    assert(!usb2_writes && !reset_writes);

    warm_initial(&p);
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(j700_dp_configure(&p, &rate) == 0);
    edges = warm_reset_edges; owned = p.dp_owned_pmas;
    fail_caller = "j700_field_update";
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == -ETIMEDOUT);
    assert(warm_reset_edges == edges && p.dp_owned_pmas == owned && p.mode_dirty);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert(!usb2_writes && !reset_writes);

    warm_initial(&p);
    fail_caller = "atcphy_configure"; /* existing RCAL poll, after reset release */
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
    assert(warm_reset_edges == 1 && !warm_ownership_errors && p.mode_dirty);
    assert(registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    struct phy phy = { &p };
    unsigned int before = writes;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EBUSY);
    assert(j700_dp_configure(&p, &rate) == -EBUSY && writes == before);
    fail_caller = NULL;
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(!p.mode_dirty && !warm_ownership_errors);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(!usb2_writes && !reset_writes);
    puts("PASS: park/DP-cleanup failures precede reset; released-reset RCAL failure stays dirty and retryable");
}
static void warm_scope_controls(void)
{
    struct apple_atcphy p;
    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        for (unsigned int t8140 = 0; t8140 < 2; t8140++) {
            orientation_initial(&p, fixed);
            hw.has_t8140_dp = t8140;
            registers[ATCPHY_POWER_CTRL / 4] |= ATCPHY_POWER_PHY_RESET_N;
            warm_reset_edges = warm_unreset_lanes = warm_ownership_errors = 0;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(warm_reset_edges == (fixed && t8140));
            assert(!warm_ownership_errors && !usb2_writes && !reset_writes);
        }
    }
    hw.has_t8140_dp = true;
    puts("PASS: reset scope requires both fixed hub and T8140; ordinary/legacy paths unchanged");
}
int main(int argc, char **argv)
{
    if (argc > 1) return original_fixture_main(argc, argv);
    warm_roundtrip(); warm_failure_controls(); warm_scope_controls();
    puts("PASS: warm reset source ordering; no electrical recovery is modeled");
    return 0;
}
'''
suffix = ns['suffix'].replace('int main(', 'int original_fixture_main(', 1) + controls
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='apple-atc-warm-reset-') as temporary:
    out = args.out or Path(temporary)
    out.mkdir(parents=True, exist_ok=True)
    for name, source in [('parent', production.replace(reset_block, '')),
                         ('candidate', production)]:
        unit = out / (name + '.c')
        unit.write_text(ns['prefix'] + source + suffix)
        command = shlex.split(os.environ.get('CC', 'clang'))
        command += ['-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command + [str(unit), '-o', str(out / name)], check=True)
        result = subprocess.run([str(out / name)], capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        print(name, 'exit', result.returncode, result.stdout.strip(), flush=True)
        if name == 'parent':
            assert result.returncode != 0 and 'reset_assertions=0' in result.stdout
            assert 'first=0' in result.stdout and 'first=1' in result.stdout
        else:
            assert result.returncode == 0, result.stderr
