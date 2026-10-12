// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Aurora Silicon */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define hweight32(v) __builtin_popcount(v)
static void test_guard_mutex(void *p) { (void)p; }
#define guard(kind) test_guard_##kind
static void report(void *p, const char *s, ...) { (void)p; (void)s; }
#define dev_err report
#define dev_dbg report
#define ndelay(n) ((void)(n))
#define udelay(n) ((void)(n))

enum {
	ATCPHY_DP_LINK_RATE_RBR, ATCPHY_DP_LINK_RATE_HBR,
	ATCPHY_DP_LINK_RATE_HBR2, ATCPHY_DP_LINK_RATE_HBR3,
};

enum { APPLE_ATCPHY_MODE_OFF, APPLE_ATCPHY_MODE_DP, APPLE_ATCPHY_MODE_USB3_DP };
struct phy_configure_opts_dp {
	unsigned int link_rate, lanes, voltage[4], pre[4];
	bool set_rate, set_lanes, set_voltages, ssc;
};

struct atcphy_mode_configuration { bool dp_lane[2]; };
struct apple_atcphy {
	struct { void *core; } regs;
	void *dev;
	int lock, mode, dp_link_rate;
	unsigned int dp_owned_pmas, dp_idle_pmas;
	bool dp_spread_spectrum, swap_lanes, mode_dirty, dp_presets_valid[5];
	u32 dp_presets[5][16];
};

static u32 registers[0x20000 / 4];
static unsigned int writes, polls, fail_poll, pin_mask = 2;
static u32 transactions[2048], transaction_masks[2048];
static bool fail_all;

static void core_mask32(struct apple_atcphy *p, u32 off, u32 mask, u32 value)
{
	(void)p;
	assert(!(off % 4) && off / 4 < ARRAY_SIZE(registers));
	assert(!(value & ~mask));
	registers[off / 4] = (registers[off / 4] & ~mask) | value;
	assert(writes < ARRAY_SIZE(transactions));
	transaction_masks[writes] = mask;
	transactions[writes++] = off;
	if (off == 0x2000) {
		if (value == 0x10000001) {
			registers[off / 4] |= BIT(1);
			registers[0x7034 / 4] |= BIT(3);
		} else {
			registers[off / 4] &= ~BIT(1);
			if (value == 0x10000018)
				registers[0x7034 / 4] &= ~BIT(3);
		}
	}
}

static void core_set32(struct apple_atcphy *p, u32 o, u32 v)
{
	core_mask32(p, o, v, v);
}

static void core_clear32(struct apple_atcphy *p, u32 o, u32 v)
{
	core_mask32(p, o, v, 0);
}

static void writel(u32 value, void *ptr)
{
	u32 off = (char *)ptr - (char *)registers;

	core_mask32(NULL, off, ~0U, value);
}

static u32 readl(void *ptr) { return *(u32 *)ptr; }
#define readl_poll_timeout_atomic(addr, val, condition, delay, timeout) \
	({ \
		int rc; \
		(void)(delay); \
		(void)(timeout); \
		polls++; \
		val = readl(addr); \
		rc = fail_all || polls == fail_poll || !(condition) ? -ETIMEDOUT : 0; \
		rc; \
	})

static int j700_pma_idle(struct apple_atcphy *p, unsigned int lane)
{
	core_set32(p, 0xc, BIT(2 + lane) | BIT(6 + lane));
	core_mask32(p, 0x200, 15U << (4 * lane), 1U << (4 * lane));
	return 0;
}

static const struct atcphy_mode_configuration *
atcphy_get_mode_config(struct apple_atcphy *p, int mode)
{
	static struct atcphy_mode_configuration config;

	(void)p;
	(void)mode;
	config.dp_lane[0] = pin_mask & 1;
	config.dp_lane[1] = pin_mask & 2;
	return &config;
}

#include "drivers/phy/apple/atc-j700-dp.h"

static struct apple_atcphy device(void)
{
	struct apple_atcphy p = {
		.regs.core = registers, .mode = APPLE_ATCPHY_MODE_USB3_DP,
		.dp_link_rate = -1,
	};

	memset(registers, 0xa5, sizeof(registers));
	for (unsigned int i = 0; i < ARRAY_SIZE(p.dp_presets_valid); i++)
		p.dp_presets_valid[i] = true;
	writes = 0;
	polls = 0;
	fail_poll = 0;
	fail_all = false;
	return p;
}

static void check_transmitter_order(unsigned int pmas)
{
	for (unsigned int lane = 0; lane < 2; lane++) {
		u32 base = 0xd000 + lane * 0x7000;
		bool found_rate = false, found_power = false;

		if (!(pmas & BIT(lane)))
			continue;
		for (unsigned int t = 0; t + 4 < writes; t++) {
			if (transactions[t] == base + 4 && transactions[t + 1] == base + 4) {
				assert(transactions[t + 2] == base + 0xc);
				assert(transactions[t + 3] == base + 0x10);
				assert(transactions[t + 4] == base + 0x1c);
				found_rate = true;
			}
			if (transactions[t] == 0x10c && transaction_masks[t] == BIT(4 + lane)) {
				assert(transactions[t + 1] == 0x10c);
				assert(transaction_masks[t + 1] == BIT(6 + lane));
				assert(transaction_masks[t + 2] == BIT(lane));
				assert(transaction_masks[t + 3] == BIT(2 + lane));
				found_power = true;
			}
		}
		assert(found_rate && found_power);
	}
}

int main(void)
{
	const unsigned int rates[] = { 1620, 2700, 5400, 8100 };
	unsigned int cases = 0;

	for (pin_mask = 1; pin_mask <= 3; pin_mask++) {
		for (unsigned int rate = 0; rate < ARRAY_SIZE(rates); rate++) {
			for (unsigned int ssc = 0; ssc < 2; ssc++) {
				struct apple_atcphy p = device();
				struct phy_configure_opts_dp o = {
					.set_rate = true, .link_rate = rates[rate], .ssc = ssc,
				};
				u32 before = registers[0x200 / 4];

				assert(!j700_dp_configure(&p, &o));
				assert(p.dp_owned_pmas == pin_mask && p.dp_link_rate == (int)rate);
				check_transmitter_order(pin_mask);
				if (pin_mask != 3) {
					u32 other = pin_mask == 1 ? 0xf0 : 0xf;

					assert(!((registers[0x200 / 4] ^ before) & other));
				}
				unsigned int first_writes = writes;

				assert(!j700_dp_configure(&p, &o) && writes == first_writes);
				struct phy_configure_opts_dp voltage = {
					.set_voltages = true, .lanes = 2 * hweight32(pin_mask),
				};

				for (unsigned int lane = 0; lane < voltage.lanes; lane++) {
					unsigned int index = 4 * lane + 1;

					voltage.voltage[lane] = lane;
					voltage.pre[lane] = 1;
					p.dp_presets[rate + 1][index] = 0x12340 + lane;
				}
				assert(!j700_dp_configure(&p, &voltage));
				for (unsigned int lane = 0; lane < voltage.lanes; lane++) {
					unsigned int first = pin_mask == 1 ? 0 : 1;
					u32 base = (lane % 2 ? 0xc000 : 0x9000) +
						   (first ^ (lane / 2)) * 0x7000;
					u32 offset = lane % 2 ? 0x50 : 0x210;
					u32 preset = 0x12340 + lane;

					assert((registers[(base + offset) / 4] & 0xc00f) ==
					       (((preset & 3) << 14) | ((preset >> 2) & 15)));
				}

				assert(!j700_dp_stop(&p));
				assert(!p.dp_owned_pmas && p.dp_link_rate == -1);
				cases++;
			}
		}
	}
	for (unsigned int full = 0; full < 2; full++) {
		for (unsigned int swap = 0; swap < 2; swap++) {
			struct apple_atcphy p = device();
			u32 old_selector = registers[0x64 / 4];
			u32 old_misc = registers[4 / 4];
			u32 mask = full ? 0x3ffff : 0x1ffff;

			p.swap_lanes = swap;
			assert(!j700_dp_route(&p, full));
			assert(!((registers[0x64 / 4] ^ old_selector) & ~mask));
			assert(registers[4 / 4] == old_misc);
			assert((registers[0x60 / 4] & 0xfff) ==
			       (full ? 0x492 : (swap ? 0x252 : 0x489)));
			assert((registers[0x64 / 4] & mask) ==
			       (full ? (swap ? 0x114 : 0x22014) : (0x110 | swap)));
			cases++;
		}
	}
	pin_mask = 2;
	for (unsigned int fault = 1; fault <= 3; fault++) {
		struct apple_atcphy p = device();
		struct phy_configure_opts_dp o = { .set_rate = true, .link_rate = 8100 };

		fail_poll = fault;
		assert(j700_dp_configure(&p, &o) == -ETIMEDOUT);
		assert(!p.dp_owned_pmas && p.dp_link_rate == -1);
		cases++;
	}
	{
		struct apple_atcphy p = device();
		struct phy_configure_opts_dp o = { .set_rate = true, .link_rate = 8100 };

		fail_all = true;
		assert(j700_dp_configure(&p, &o) == -ETIMEDOUT && p.dp_owned_pmas == 2);
		fail_all = false;
		assert(!j700_dp_stop(&p) && !p.dp_owned_pmas);
		cases++;
	}
	{
		struct apple_atcphy p = device();
		struct phy_configure_opts_dp o = { .set_rate = true, .link_rate = 1234 };

		assert(j700_dp_configure(&p, &o) == -EINVAL && !writes);
		o.link_rate = 8100;
		o.lanes = 3;
		assert(j700_dp_configure(&p, &o) == -EINVAL && !writes);
		o.lanes = 0;
		p.dp_presets_valid[0] = false;
		assert(j700_dp_configure(&p, &o) == -ENOENT && !writes);
		p.dp_presets_valid[0] = true;
		o.set_voltages = true;
		o.lanes = 2;
		o.voltage[1] = 4;
		assert(j700_dp_configure(&p, &o) == -EINVAL && !writes);
		cases++;
	}
	printf("J700 DP: %u register/state cases passed\n", cases);
	return 0;
}
