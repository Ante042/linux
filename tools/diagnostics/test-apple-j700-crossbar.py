#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual T602x mux function for J700 and the other outputs."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/mux/apple-display-crossbar.c").read_text()


def function(name):
    start = source.index("static ", source.index(name) - 30)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
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
#include <errno.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(mask, value) (((u32)(value) << __builtin_ctz(mask)) & (mask))
#define MUX_IDLE_DISCONNECT (-2)
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define udelay(value) ((void)(value))
#define t602x_dump(...) ((void)0)
struct apple_dpxbar { int selected_dispext[3], lock; };
struct mux_chip { struct apple_dpxbar *data; };
struct mux_control { struct mux_chip *chip; unsigned int index; };
static struct apple_dpxbar *mux_chip_priv(struct mux_chip *chip) { return chip->data; }
static unsigned int mux_control_get_index(struct mux_control *mux) { return mux->index; }
static bool j700;
static u32 regs[1024], writes[1024];
static bool of_machine_is_compatible(const char *name)
{
	assert(!strcmp(name, "apple,j700"));
	return j700;
}
static void dpxbar_mask32(struct apple_dpxbar *x, u32 reg, u32 mask, u32 value)
{
	(void)x;
	assert(reg < sizeof(regs) && !(reg % 4) && !(value & ~mask));
	regs[reg / 4] = (regs[reg / 4] & ~mask) | value;
	writes[reg / 4]++;
}
static void dpxbar_set32(struct apple_dpxbar *x, u32 reg, u32 value)
{
	dpxbar_mask32(x, reg, value, value);
}
static void dpxbar_clear32(struct apple_dpxbar *x, u32 reg, u32 value)
{
	dpxbar_mask32(x, reg, value, 0);
}
'''
definitions = "\n".join(re.findall(r"^#define (?:T602X_|CROSSBAR_|ATC_|FIFO_).*$",
                                  source, re.M))
enum = re.search(r"enum \{ MUX_DPPHY.*?;", source).group()
body = "\n".join(function(name) for name in (
    "t602x_atc_bit(", "t602x_mux_mask(", "t602x_mux_set(",
    "apple_dpxbar_set_t602x("))
suffix = r'''
int main(void)
{
	unsigned int cases = 0;
	for (unsigned int machine = 0; machine < 2; machine++) {
		for (unsigned int output = 0; output < 3; output++) {
			for (int state = 0; state < 9; state++) {
				struct apple_dpxbar data = { .selected_dispext = { -1, -1, -1 } };
				struct mux_chip chip = { .data = &data };
				struct mux_control mux = { .chip = &chip, .index = output };
				j700 = machine;
				memset(regs, 0xa5, sizeof(regs));
				memset(writes, 0, sizeof(writes));
				assert(!apple_dpxbar_set_t602x(&mux, state));
				assert(data.selected_dispext[output] == state);
				assert(!!writes[0x50 / 4] == !(j700 && output == 0));
				assert(!!writes[0x70 / 4] == !(j700 && output == 0));
				memset(writes, 0, sizeof(writes));
				assert(!apple_dpxbar_set_t602x(&mux, MUX_IDLE_DISCONNECT));
				assert(data.selected_dispext[output] == -1);
				assert(!!writes[0x50 / 4] == !(j700 && output == 0));
				assert(!!writes[0x70 / 4] == !(j700 && output == 0));
				assert(writes[(j700 && output == 0 ? 0x20 : 0x24) / 4]);
				assert(apple_dpxbar_set_t602x(&mux, 9) == -EINVAL);
				cases++;
			}
		}
	}
	printf("PASS %u actual crossbar select/disconnect scenarios\n", cases);
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="j700-crossbar-") as work:
    work = Path(work)
    unit = work / "test.c"
    unit.write_text(prefix + definitions + "\n" + enum + "\n" + body + suffix)
    command = shlex.split(os.environ.get("CC", "clang"))
    # The existing mux implementation compares an int loop index with an unsigned port.
    command += ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-O1", "-g"]
    command += ["-fsanitize=address,undefined", str(unit), "-o", str(work / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(work / "test")], check=True)
