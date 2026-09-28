// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include "dcp-link.h"
#include "iomfb_v27.h"
#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "version_utils.h"
#include "dcp-internal.h"

struct dcp_h17p_hotplug_request {
	__le64 connected;
	u8 tiled_display[0x4c];
	u8 tiled_display_null;
	u8 padding[3];
} __packed;

struct dcp_h17p_swap_info_request {
	u8 swap_info[0xe0];
	u8 swap_info_null;
	u8 padding[3];
} __packed;

static_assert(sizeof(struct dcp_h17p_hotplug_request) == 0x58);
static_assert(sizeof(struct dcp_h17p_swap_info_request) == 0xe4);
/* D590 swap_complete_ap_gated: 0x730 on the wire in 1919/1919 traced swaps. */
static_assert(sizeof(struct dc_swap_complete_resp_v27) == 0x730);
static_assert(offsetof(struct dc_swap_complete_resp_v27, swap_id) == 0);

/* Native 26A428 T605X kernelcache RPC serializers. The method IDs are not
 * compatible with the 25G83 H17P or 25G76 H17G tables. See t6050-dcp.rst.
 */
static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),
	IOMFB_METHOD("A027", dcpep_setup_video_limits),
	IOMFB_METHOD("A136", iomfbep_a131_pmu_service_matched),
	IOMFB_METHOD("A137", iomfbep_a132_backlight_service_matched),
	IOMFB_METHOD("A389", dcpep_set_create_dfb),
	IOMFB_METHOD("A390", iomfbep_a358_vi_set_temperature_hint),
	IOMFB_METHOD("A401", dcpep_start_signal),
	IOMFB_METHOD("A406", dcpep_swap_start),
	IOMFB_METHOD("A408", dcpep_swap_submit),
	IOMFB_METHOD("A411", dcpep_set_display_device),
	IOMFB_METHOD("A412", dcpep_is_main_display),
	IOMFB_METHOD("A413", dcpep_set_digital_out_mode),
	IOMFB_METHOD("A423", iomfbep_set_matrix),
	IOMFB_METHOD("A427", iomfbep_get_color_remap_mode),
	IOMFB_METHOD("A450", dcpep_set_parameter_dcp),
	IOMFB_METHOD("A454", dcpep_create_default_fb),
	IOMFB_METHOD("A037", dcpep_get_dfb_compression_info),
	IOMFB_METHOD("A036", dcpep_get_dfb_info),
	IOMFB_METHOD("A103", dcpep_get_dfb_layout),
	IOMFB_METHOD("A453", dcpep_get_dfb_state),
	IOMFB_METHOD("A104", dcpep_set_dfb_dimensions),
	IOMFB_METHOD("A105", dcpep_commit_dfb_info),
	IOMFB_METHOD("A035", dcpep_dfb_query),
	IOMFB_METHOD("A384", dcpep_dfb_ready_query),
	IOMFB_METHOD("A353", dcpep_pipe_query_353),
	IOMFB_METHOD("A352", dcpep_pipe_set_352),
	IOMFB_METHOD("A415", dcpep_pipe_cfg_415),
	IOMFB_METHOD("A033", dcpep_pipe_cfg_031),
	IOMFB_METHOD("A414", dcpep_pipe_cfg_414),
	IOMFB_METHOD("A489", dcpep_pipe_query_478),
	IOMFB_METHOD("A485", dcpep_pipe_query_474),
	IOMFB_METHOD("A487", dcpep_pipe_set_476),
	IOMFB_METHOD("A428", dcpep_pipe_set_428),
	IOMFB_METHOD("A458", dcpep_enable_disable_video_power_savings),
	IOMFB_METHOD("A466", dcpep_first_client_open),
	IOMFB_METHOD("A467", iomfbep_last_client_close),
	IOMFB_METHOD("A475", dcpep_set_display_refresh_properties),
	IOMFB_METHOD("A478", dcpep_flush_supports_power),
	IOMFB_METHOD("A479", iomfbep_abort_swaps_dcp),
	IOMFB_METHOD("A484", dcpep_set_power_state),
	IOMFB_METHOD("A483", dcpep_register_dfb_surface),
};

#define DCP_FW v27
#define DCP_FW_VER DCP_FW_VERSION(27, 0, 0)

#include "iomfb_template.c"

static bool trampoline_rt_bandwidth_h17p(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	struct apple_dcp_link_rpc_view view = {
		.call = APPLE_DCP_LINK_CALL_D003,
		.input_size = sizeof(struct apple_dcp_h17p_rt_bw_request),
		.output_size = sizeof(struct apple_dcp_h17p_rt_bw_reply),
		.input = in,
		.output = out,
	};
	/*
	 * Take these from the macOS capture rather than from the DT resources.
	 * macOS answers D003 with one stable record,
	 *   0000820203000000 0080860203000000 00000000
	 * -> scratch 0x302820000, doorbell 0x302868000 -- which is
	 * DISP0_REG_BASE + 0x20000 and + 0x68000, both inside the 0xbc000
	 * window we already hand out for map_reg index 5.  The DT lookup lands
	 * 0x5a80000 higher, so the firmware was being pointed at the wrong
	 * block for its bandwidth scratch and doorbell.
	 */
	u64 clock_request = DCP_H17P_DISP0_REG_BASE + 0x68000;
	u64 scratch = DCP_H17P_DISP0_REG_BASE + 0x20000;

	/* The T8142 DT contains the native PMC resources. The fallback above
	 * is specific to the incomplete legacy T8140 binding.
	 */
	if (!dcp->hw.legacy_pmc_binding) {
		scratch = dcp->disp_bw_scratch_res.start +
			  dcp->disp_bw_scratch_offset;
		clock_request = dcp->disp_bw_doorbell_res.start +
				dcp->disp_bw_doorbell_offset;
	}

	trace_iomfb_callback(dcp, tag, __func__);
	if (apple_dcp_h17p_rt_bw_encode_reply(&view, scratch,
						     clock_request))
		dev_warn(dcp->dev, "D003 bandwidth request is malformed\n");
	return true;
}

static bool trampoline_get_frequency_v27(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	const struct apple_dcp_h17p_clock_request *request = in;
	u32 id = le32_to_cpu(request->clock_id);
	unsigned int i;
	u64 rate = 0;

	for (i = 0; i < dcp->num_firmware_clocks; i++) {
		if (dcp->firmware_clock_ids[i] == id) {
			rate = clk_get_rate(dcp->firmware_clocks[i].clk);
			break;
		}
	}
	if (i == dcp->num_firmware_clocks)
		dev_err(dcp->dev, "DCP requested undeclared firmware clock %u\n", id);
	*(__le64 *)out = cpu_to_le64(rate);
	return true;
}

static bool trampoline_hotplug_h17p(struct apple_dcp *dcp, int tag,
				    void *out, void *in)
{
	const struct dcp_h17p_hotplug_request *request = in;

	if (!(request->tiled_display_null & 1))
		memcpy(out, request->tiled_display,
		       sizeof(request->tiled_display));
	return trampoline_hotplug(dcp, tag, out, in);
}

static bool trampoline_swap_info_h17p(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	const struct dcp_h17p_swap_info_request *request = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (!(request->swap_info_null & 1))
		memcpy(out, request->swap_info, sizeof(request->swap_info));
	return true;
}

/*
 * D574 (in 4, out 8) has no analogue in the v12.3/v13.5 tables.  macOS 25G83
 * answers 0x435b0000 == 219.0f for the only selector it is ever called with
 * (in == 0), identically in both captured boots.  219 is 235 - 16, the
 * BT.601/709 studio-swing luma range, so this feeds the DCP's colour matrix
 * setup; answering zero would put a zero scale into it.
 */
static bool iomfbep_cb_video_range_h17p(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	__le32 *resp = out;

	resp[0] = cpu_to_le32(0x435b0000); /* 219.0f */
	resp[1] = 0;
	return true;
}

/*
 * Tag numbering below is the one the H17P firmware actually uses, taken from
 * macOS 25G83 traces on this machine rather than by shifting the v13.5 table:
 * the service-creation block is D108..D113 exactly as on v13.5, and the swap
 * completion is D590.  A callback with no entry here is never acked, so a
 * missing tag stalls the DCP rather than degrading it.
 */
/*
 * D400 -- iOSSvcProx::get_property(name, DataBuffer *, bool).
 *
 * The firmware asks the "PROV" (provider) service for the panel's power model
 * during init_power_lut(); answering "not found" makes it log
 *   IOMFB init_power_lut: failed to read power-lut-data-x
 * and then fail to create the default framebuffer surface.  The request
 * carries a capacity at +0x44 and the reply is { u8 data[0xc00]; __le32
 * length; }, where length is the byte count written and 0 means "no such
 * property".  These are the values macOS 25G83 returns on this machine,
 * byte-identical across both captured boots.
 */
struct dcp_h17p_get_data_prop_req {
	char obj[4];
	char key[0x40];
	__le32 length;
	u8 data_null;
	u8 length_null;
	u8 padding[2];
} __packed;
static_assert(sizeof(struct dcp_h17p_get_data_prop_req) == 0x4c);

struct dcp_h17p_get_data_prop_resp {
	u8 data[0xc00];
	__le32 length;
} __packed;
static_assert(sizeof(struct dcp_h17p_get_data_prop_resp) == 0xc04);

static bool trampoline_get_data_prop_v27(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	const struct dcp_h17p_get_data_prop_req *req = in;
	struct dcp_h17p_get_data_prop_resp *resp = out;
	const struct property *prop;
	u32 cap = min_t(u32, le32_to_cpu(req->length), sizeof(resp->data));

	if (memcmp(req->obj, "VORP", sizeof(req->obj)) ||
	    strnlen(req->key, sizeof(req->key)) == sizeof(req->key))
		return true;
	/* Preserve the firmware's native little-endian property bytes in DT. */
	prop = of_find_property(dcp->dev->of_node, req->key, NULL);
	if (!prop || prop->length < 0 || prop->length > cap)
		return true;
	memcpy(resp->data, prop->value, prop->length);
	resp->length = cpu_to_le32(prop->length);
	return true;
}

static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = dcpep_cb_d000_h17p, /* native did_boot callback */
	[1] = trampoline_true,
	[2] = trampoline_nop,
	[3] = trampoline_rt_bandwidth_h17p,
	[6] = trampoline_set_frame_sync_props_h17p, /* 0x54/0x50, identity scale */
	[100] = iomfbep_cb_match_pmu_service, /* match_pmu_service */
	[101] = trampoline_zero,
	[102] = trampoline_nop, /* set_number_property */
	[104] = trampoline_nop, /* set_boolean_property */
	[107] = trampoline_true, /* create_provider_service */
	[108] = trampoline_true, /* create_product_service */
	[109] = trampoline_true, /* create_PMU_service */
	[110] = trampoline_true, /* create_iomfb_service */
	[111] = trampoline_create_backlight_service, /* create_backlight_service */
	[112] = trampoline_true, /* create_nvram_service */
	[113] = trampoline_zero, /* CoreAnalyticsSendEvent */
	[116] = trampoline_nop, /* set_idle_caching_state_ap */
	[117] = trampoline_zero, /* upload_trace_start */
	[118] = trampoline_zero, /* upload_trace_chunk */
	[119] = trampoline_zero, /* upload_trace_end */
	[120] = dcpep_cb_boot_1, /* start_hardware_boot */
	[121] = trampoline_false, /* is_dark_boot */
	[122] = trampoline_false, /* is_waking_from_hibernate */
	[123] = trampoline_zero, /* detect_fastsim */
	[124] = trampoline_read_edt_data, /* read_edt_data */
	[126] = trampoline_prop_start, /* setDCPAVPropStart */
	[127] = trampoline_prop_chunk, /* setDCPAVPropChunk */
	[128] = trampoline_prop_end, /* setDCPAVPropEnd */
	[129] = trampoline_allocate_bandwidth, /* allocate_bandwidth */
	[201] = trampoline_map_piodma, /* map_buf */
	[202] = trampoline_unmap_piodma, /* unmap_buf */
	[206] = iomfbep_cb_match_pmu_service_2, /* match_pmu_service */
	[207] = iomfbep_cb_match_backlight_service, /* match_backlight_service */
	[208] = trampoline_nop, /* update_backlight_factor_prop */
	[209] = trampoline_get_time, /* get_calendar_time_ms */
	[300] = trampoline_pr_publish,
	[400] = trampoline_get_data_prop_v27, /* get_property */
	[401] = trampoline_get_uint_prop, /* get_uint_prop */
	[404] = trampoline_nop, /* set_uint_prop */
	[406] = trampoline_set_fx_prop, /* set_fx_prop */
	[408] = trampoline_get_frequency_v27, /* getClockFrequency */
	[411] = trampoline_map_reg, /* mapDeviceMemoryWithIndex */
	[413] = trampoline_true, /* setProperty */
	[414] = trampoline_sr_set_property_int, /* setProperty */
	[415] = trampoline_true, /* setProperty */
	[451] = trampoline_allocate_buffer, /* allocate_buffer */
	[452] = trampoline_map_physical, /* prepare */
	[454] = trampoline_release_mem_desc, /* release_descriptor */
	[552] = trampoline_true,
	[561] = trampoline_true,
	[563] = trampoline_true,
	[565] = trampoline_true,
	[567] = trampoline_true,
	[572] = trampoline_zero, /* powerUpDART */
	[574] = iomfbep_cb_video_range_h17p,
	[575] = trampoline_hotplug_h17p,
	[576] = trampoline_nop,
	/*
	 * We do not create a default framebuffer surface, so answering "yes"
	 * makes the firmware go on to program the display engine for a surface
	 * that does not exist: it then DMAs from a null base and the piodma
	 * DART (412300000) faults continuously on SID 0, which starves the
	 * system into an RCU stall.  Decline instead.
	 */
	[586] = iomfbep_cb_create_dfb_surface, /* create_default_fb_surface */
	[587] = trampoline_nop, /* clear_default_surface */
	[588] = trampoline_nop, /* swap_notify_gated */
	[589] = trampoline_swap_info_h17p, /* swap_info_notify_dispatch */
	[594] = trampoline_swap_complete, /* swap_complete_ap_gated */
	[596] = trampoline_swap_complete_intent_gated, /* swap_complete_intent_gated */
	[597] = trampoline_abort_swap_ap_gated, /* abort_swap_ap_gated */
	[598] = trampoline_enable_backlight_message_ap_gated, /* enable_backlight_message_ap_gated */
	[599] = trampoline_nop, /* setSystemConsoleMode */
	[601] = trampoline_false, /* isDFBAllocated */
	[602] = trampoline_false,
	[603] = trampoline_nop, /* find_swap_function_gated */
};

/* Bounds from the native callback deserializers, before invoking a handler.
 * This protects the typed callbacks from a mismatched firmware ABI.
 */
static const struct {
	u16 input, output;
} callback_sizes[IOMFB_MAX_CB] = {
	[0] = { 0x0, 0x4 },
	[1] = { 0x0, 0x4 },
	[2] = { 0x0, 0x0 },
	[3] = { 0x4, 0x14 },
	[6] = { 0x54, 0x50 },
	[300] = { 0x10, 0x0 },
	[100] = { 0x0, 0x0 },
	[101] = { 0x0, 0x4 },
	[102] = { 0x44, 0x0 },
	[104] = { 0x44, 0x0 },
	[107] = { 0x0, 0x4 },
	[108] = { 0x0, 0x4 },
	[109] = { 0x0, 0x4 },
	[110] = { 0x0, 0x4 },
	[111] = { 0x0, 0x4 },
	[112] = { 0x0, 0x4 },
	[113] = { 0x1044, 0x1004 },
	[116] = { 0xc, 0x0 },
	[117] = { 0x14, 0x4 },
	[118] = { 0x3e88, 0x4 },
	[119] = { 0x4, 0x4 },
	[120] = { 0x0, 0x4 },
	[121] = { 0x0, 0x4 },
	[122] = { 0x0, 0x4 },
	[123] = { 0x0, 0x4 },
	[124] = { 0x64, 0x24 },
	[126] = { 0x4, 0x4 },
	[127] = { 0x1008, 0x4 },
	[128] = { 0x40, 0x4 },
	[129] = { 0x1c, 0x14 },
	[201] = { 0xc, 0x10 },
	[202] = { 0x18, 0x0 },
	[206] = { 0x0, 0x4 },
	[207] = { 0x0, 0x4 },
	[208] = { 0x4, 0x0 },
	[209] = { 0x0, 0x8 },
	[400] = { 0x4c, 0xc04 },
	[401] = { 0x50, 0xc },
	[404] = { 0x48, 0x0 },
	[406] = { 0x48, 0x0 },
	[408] = { 0x8, 0x8 },
	[411] = { 0x10, 0x1c },
	[413] = { 0x1048, 0x4 },
	[414] = { 0x50, 0x4 },
	[415] = { 0x4c, 0x4 },
	[451] = { 0x14, 0x14 },
	[452] = { 0x8, 0x4 },
	[454] = { 0x4, 0x4 },
	[552] = { 0x1044, 0x4 },
	[561] = { 0x1044, 0x4 },
	[563] = { 0x4c, 0x4 },
	[565] = { 0x48, 0x4 },
	[567] = { 0x80, 0x4 },
	[572] = { 0x4, 0x4 },
	[574] = { 0x4, 0x8 },
	[575] = { 0x58, 0x4c },
	[576] = { 0x4, 0x0 },
	[586] = { 0x8, 0x4 },
	[587] = { 0x0, 0x0 },
	[588] = { 0x18, 0x0 },
	[589] = { 0xe4, 0xe0 },
	[594] = { 0x730, 0x0 },
	[596] = { 0x14, 0x0 },
	[597] = { 0x4, 0x0 },
	[598] = { 0x4, 0x0 },
	[599] = { 0x4, 0x0 },
	[601] = { 0x0, 0x4 },
	[602] = { 0x0, 0x4 },
	[603] = { 0x0, 0x0 },
};

bool iomfb_v27_callback_size_valid(unsigned int tag, u32 input, u32 output)
{
	if (tag >= ARRAY_SIZE(callback_sizes) || !cb_handlers[tag])
		return false;
	return input >= callback_sizes[tag].input &&
	       output >= callback_sizes[tag].output;
}

void iomfb_start_v27(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;
	dcp_start_signal(dcp, false, dcp_started, NULL);
}

#undef DCP_FW_VER
#undef DCP_FW
