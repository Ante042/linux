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

struct apple_epic_service { struct dptx_port *cookie; };
static unsigned int calls;
static int failure;
#define PHY_MODE_DP 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

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
	struct apple_epic_service service = { .cookie = &port };
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
			if ((group == 1 && command == 3) ||
			    (group == 0 && (command == 4 || command == 26)))
				expected = 0;
			assert(dptxport_group_call(&service, group, command,
						   request, 80, reply, 80) == expected);
		}
	}
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
