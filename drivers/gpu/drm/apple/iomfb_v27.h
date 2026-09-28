// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_IOMFB_V27_H__
#define __APPLE_IOMFB_V27_H__

#include "version_utils.h"

#define DCP_FW v27
#define DCP_FW_VER DCP_FW_VERSION(27, 0, 0)

#include "iomfb_template.h"

static_assert(sizeof(struct dcp_swap_v27) == 0x6e0);
static_assert(sizeof(struct dcp_surface_v27) == 0x22c);
static_assert(sizeof(struct dcp_swap_submit_req_v27) == 0xff4);
static_assert(sizeof(struct dcp_swap_submit_resp_v27) == 0x0c);

/* 26A428 IOMobileFramebufferAP::swap_submit_dcp_main, ab7fd34..ab7fe3c. */
static_assert(offsetof(struct dcp_swap_submit_req_v27, surf) == 0x6e0);
static_assert(offsetof(struct dcp_swap_submit_req_v27, surf_iova) == 0xf90);
static_assert(offsetof(struct dcp_swap_submit_req_v27, swap_null) == 0xfea);
static_assert(offsetof(struct dcp_swap_v27, swap_id) == 0x98);
static_assert(offsetof(struct dcp_swap_v27, src_rect) == 0xac);
static_assert(offsetof(struct dcp_swap_v27, dst_rect) == 0x10c);
static_assert(offsetof(struct dcp_swap_v27, swap_enabled) == 0x14c);

bool iomfb_v27_callback_size_valid(unsigned int tag, u32 input, u32 output);

#undef DCP_FW_VER
#undef DCP_FW

#endif /* __APPLE_IOMFB_V27_H__ */
