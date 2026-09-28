/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Native UnifiedPipeline2 shares these three counters between all pipes. */
static DEFINE_SPINLOCK(external_counts_lock_v27);
static u32 external_pipe_count_v27;
static u32 external_apple_display_count_v27;
static u32 external_suppression_count_v27;

static bool trampoline_update_external_counts_v27(struct apple_dcp *dcp, int tag,
						  void *out, void *in)
{
	const u8 *request = in;
	bool plug = request[0] & 1;
	u32 apple_display = request[1] & 1;
	u32 pipes = get_unaligned_le32(request + 2);
	unsigned long flags;

	trace_iomfb_callback(dcp, tag, __func__);
	/* Native UPPipe2::is_disp_external gates only the pipe-count update. */
	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP)
		return true;
	spin_lock_irqsave(&external_counts_lock_v27, flags);
	external_pipe_count_v27 += plug ? pipes : -pipes;
	external_apple_display_count_v27 += plug ? apple_display : -apple_display;
	spin_unlock_irqrestore(&external_counts_lock_v27, flags);
	return true;
}

static bool trampoline_get_external_counts_v27(struct apple_dcp *dcp, int tag,
					       void *out, void *in)
{
	const u8 *request = in;
	unsigned long flags;
	u32 counts[3];

	trace_iomfb_callback(dcp, tag, __func__);
	spin_lock_irqsave(&external_counts_lock_v27, flags);
	counts[0] = external_pipe_count_v27;
	counts[1] = external_apple_display_count_v27;
	counts[2] = external_suppression_count_v27;
	spin_unlock_irqrestore(&external_counts_lock_v27, flags);
	/* Three u32 in/out values followed by three native pointer-null flags. */
	for (unsigned int i = 0; i < ARRAY_SIZE(counts); i++)
		if (!(request[12 + i] & 1))
			put_unaligned_le32(counts[i], (u8 *)out + 4 * i);
	return true;
}

static bool trampoline_apply_suppression_v27(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	const u8 *request = in;
	unsigned long flags;

	trace_iomfb_callback(dcp, tag, __func__);
	spin_lock_irqsave(&external_counts_lock_v27, flags);
	external_suppression_count_v27 += (request[0] & 1) ? 1U : -1U;
	spin_unlock_irqrestore(&external_counts_lock_v27, flags);
	return true;
}
