// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Aurora Silicon */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
struct phy { int unused; };
struct settings {
	unsigned int lanes, voltage[4], pre[4];
	bool set_voltages;
};

union phy_configure_opts { struct settings dp; };
struct dptx_port {
	struct phy *atcphy;
	u32 trained_voltage[4], trained_pre[4];
};

struct endpoint { bool wire_v2; };
struct apple_epic_service { struct dptx_port *cookie; struct endpoint *ep; };
static unsigned int calls;
static int failure;
#define PHY_MODE_DP 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) min((t)(a), (t)(b))
#define trace_dptxport_apcall(port, ...) ((void)(port))
#define dev_dbg(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define print_hex_dump_debug(...) ((void)0)
#define DUMP_PREFIX_OFFSET 0

static u32 get_unaligned_le32(const void *p)
{
	u32 v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static void put_unaligned_le32(u32 v, void *p)
{
	memcpy(p, &v, sizeof(v));
}

static int phy_validate(struct phy *p, int mode, int sub, union phy_configure_opts *o)
{
	assert(p && mode == PHY_MODE_DP && !sub);
	o->dp.lanes = 2;
	return 0;
}

static int phy_configure(struct phy *p, union phy_configure_opts *o)
{
	assert(p && o->dp.set_voltages && o->dp.lanes <= 2);
	calls++;
	return failure;
}

#include "drivers/gpu/drm/apple/dptx-j700-training.h"

#ifdef DPTX_GROUP_TEST
#define DPTX_APCALL_SET_DRIVE_SETTINGS 3
#define DPTX_APCALL_GET_DRIVE_SETTINGS 4
#define DPTX_APCALL_GET_VIRTUAL_DEVICE_MODE 26
struct dptxport_apcall_get_support { u8 bytes[32]; };
static int dptxport_call(struct apple_epic_service *service, u32 command,
			 const void *data, size_t size, void *reply, size_t room)
{
	(void)service;
	(void)command;
	(void)data;
	(void)size;
	(void)reply;
	(void)room;
	return 7;
}

#include "group-call.h"
#endif

int main(void)
{
	struct phy phy = {};
	struct dptx_port port = { .atcphy = &phy };
	struct endpoint ep = { .wire_v2 = true };
	struct apple_epic_service service = { .cookie = &port, .ep = &ep };
	u8 input[81] = {}, output[81];
	u8 *request = input + 1, *reply = output + 1;
	unsigned int cases = 0;

	put_unaligned_le32(2, request + 16);
	put_unaligned_le32(1, request + 36);
	put_unaligned_le32(2, request + 52);
	for (unsigned int size = 0; size < 56; size++) {
		assert(dptx_j700_training(&service, true, request, size, reply, 80) == -EINVAL);
		assert(dptx_j700_training(&service, true, request, 80, reply, size) == -EINVAL);
		assert(!calls);
		cases += 2;
	}
	put_unaligned_le32(9, request + 52);
	assert(dptx_j700_training(&service, true, request, 80, reply, 80) == -EINVAL);
	assert(!calls && !port.trained_voltage[0]);
	put_unaligned_le32(2, request + 52);
	failure = -EIO;
	assert(dptx_j700_training(&service, true, request, 80, reply, 80) == -EIO);
	assert(!port.trained_voltage[0]);
	failure = 0;
	assert(!dptx_j700_training(&service, true, request, 80, reply, 80));
	assert(port.trained_voltage[0] == 1 && port.trained_pre[1] == 2);
	assert(get_unaligned_le32(reply + 16) == 2 && !get_unaligned_le32(reply));
	put_unaligned_le32(4, request);
	assert(!dptx_j700_training(&service, false, request, 32, reply, 80));
	assert(get_unaligned_le32(reply) == 4 && !get_unaligned_le32(reply + 16));
	assert(get_unaligned_le32(reply + 36) == 1 && get_unaligned_le32(reply + 52) == 2);
	assert(!get_unaligned_le32(reply + 76));

#ifdef DPTX_GROUP_TEST
	for (unsigned int group = 0; group < 4; group++) {
		for (unsigned int command = 0; command < 27; command++) {
			int expected = group ? -EOPNOTSUPP : 7;

			if (group == 0 && command == 3)
				expected = -EOPNOTSUPP;
			if ((group == 1 && (command == 3 || command == 4)) ||
			    (group == 0 && (command == 4 || command == 26)))
				expected = 0;
			assert(dptxport_group_call(&service, group, command,
						   request, 80, reply, 80) == expected);
		}
	}
	/* Real unified dispatcher: legacy replies retain fields/canaries and
	 * never enter native PHY training. All original v2 cases above stay.
	 */
	ep.wire_v2 = false;
	for (unsigned int command = 3; command <= 4; command++) {
		for (unsigned int length = 48; length <= 64; length += 16) {
			unsigned int before = calls;
			memset(request, 0xa5, 80);
			memset(output, 0xc3, sizeof(output));
			assert(!dptxport_group_call(&service, 1, command,
						   request, length, reply, length));
			for (unsigned int i = 0; i < length; i++) {
				bool cleared = command == 3 ? i < 4 :
					(i >= 16 && i < 20) || i >= 32;
				assert(reply[i] == (cleared ? 0 : 0xa5));
			}
			assert(output[0] == 0xc3 && output[length + 1] == 0xc3);
			assert(calls == before);
		}
	}
	assert(dptxport_group_call(&service, 0, 3, request, 80, reply, 80) == 7);
	ep.wire_v2 = true;
	put_unaligned_le32(2, request);
	unsigned int before_readback = calls;

	memset(reply, 0xff, 80);
	assert(!dptxport_group_call(&service, 1, 4, request, 56, reply, 56));
	assert(calls == before_readback);
	assert(get_unaligned_le32(reply) == 2 && !get_unaligned_le32(reply + 16));
	assert(get_unaligned_le32(reply + 36) == port.trained_voltage[0]);
	assert(get_unaligned_le32(reply + 52) == port.trained_pre[1]);
	assert(reply[56] == 0xff);
	assert(dptxport_group_call(&service, 1, 4, request, 56, reply, 55) == -EINVAL);
	assert(calls == before_readback);
	assert(dptxport_group_call(&service, 0, 26, request, 80, reply, 31) == -EINVAL);
	memset(reply, 0xff, 80);
	assert(!dptxport_group_call(&service, 0, 26, request, 80, reply, 80));
	assert(!get_unaligned_le32(reply + 16));
#endif
	const unsigned int transitions[] = { 0, 2, 0, 2, 1, 2 };

	for (unsigned int i = 0; i < ARRAY_SIZE(transitions); i++)
		assert(!dptx_j700_validate_lanes(&port, transitions[i]));
	assert(dptx_j700_validate_lanes(&port, 3) == -EINVAL);
	assert(dptx_j700_validate_lanes(&port, 4) == -EINVAL);
	assert(dptx_j700_validate_lanes(&port, UINT64_MAX) == -EINVAL);
	port.atcphy = NULL;
	assert(dptx_j700_validate_lanes(&port, 2) == -ENODEV);
	assert(dptx_j700_training(&service, true, request, 80, reply, 80) == -ENODEV);
	printf("DPTX training: %u boundary cases plus failure/cache/readback checks passed\n",
	       cases);
	return 0;
}
