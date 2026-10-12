#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the production calibration dispatch before common-power setup."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/phy/apple/atc.c").read_text()


def function(name):
    start = source.index("static void " + name + "(")
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum atcphy_mode {
	APPLE_ATCPHY_MODE_OFF, APPLE_ATCPHY_MODE_USB2, APPLE_ATCPHY_MODE_USB3,
	APPLE_ATCPHY_MODE_USB3_DP, APPLE_ATCPHY_MODE_DP,
	APPLE_ATCPHY_MODE_TBT, APPLE_ATCPHY_MODE_USB4,
};
struct apple_atcphy {
	bool swap_lanes, fixed_usb2;
	struct { bool has_t8140_dp; } *hw;
	struct { void *core, *axi2af, *lpdptx; } regs;
	struct {
		int *common[2], *axi2af, *lane_usb3[2], *lane_dp[2], *lane_usb4[2];
		int *dp_common_pre, *dp_lpdptx, *dp_common_post;
	} tunables;
};
static unsigned int count;
static int events[20];
static void apple_tunable_apply(void *base, int *table)
{
	(void)base;
	if (table)
		events[count++] = *table;
}
'''
suffix = r'''
static void check(struct apple_atcphy *p, enum atcphy_mode mode,
		  const int *expected, unsigned int size)
{
	count = 0;
	atcphy_apply_tunables(p, mode);
	assert(count == size);
	if (size)
		assert(!memcmp(events, expected, sizeof(int) * size));
}
int main(void)
{
	int ids[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	struct apple_atcphy p = {};
	typeof(*p.hw) hw = { .has_t8140_dp = true };
	p.hw = &hw;
	p.tunables.common[0] = &ids[0]; p.tunables.common[1] = &ids[1];
	p.tunables.axi2af = &ids[2];
	p.tunables.lane_usb3[0] = &ids[3]; p.tunables.lane_usb3[1] = &ids[4];
	p.tunables.lane_dp[0] = &ids[5]; p.tunables.lane_dp[1] = &ids[6];
	p.tunables.dp_common_pre = &ids[7]; p.tunables.dp_lpdptx = &ids[8];
	p.tunables.dp_common_post = &ids[9];
	p.tunables.lane_usb4[0] = &ids[10]; p.tunables.lane_usb4[1] = &ids[11];
	const int dp[] = { 3, 8, 9, 10 };
	const int shared[] = { 1, 3, 2, 4, 7, 3, 8, 9, 10 };
	const int reversed[] = { 1, 3, 2, 5, 6, 3, 8, 9, 10 };
	const int legacy[] = { 1, 3, 2, 6, 7 };
	check(&p, APPLE_ATCPHY_MODE_DP, dp, 4);
	check(&p, APPLE_ATCPHY_MODE_USB3_DP, shared, 9);
	p.swap_lanes = true;
	check(&p, APPLE_ATCPHY_MODE_USB3_DP, reversed, 9);
	p.swap_lanes = false;
	hw.has_t8140_dp = false;
	check(&p, APPLE_ATCPHY_MODE_DP, legacy, 5);
	p.fixed_usb2 = true;
	check(&p, APPLE_ATCPHY_MODE_USB2, NULL, 0);
	puts("PASS calibration order: full DP, shared, reverse, legacy and fixed USB2");
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="j700-tuning-") as work:
    work = Path(work)
    test = work / "test.c"
    test.write_text(prefix + function("j700_dp_tunables") +
                    function("atcphy_apply_tunables") + suffix)
    command = shlex.split(os.environ.get("CC", "clang"))
    command += ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    command += ["-fsanitize=address,undefined", str(test), "-o", str(work / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(work / "test")], check=True,
                   env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
