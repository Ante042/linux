// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paravirtualised Apple DART for the x1n1 hypervisor (sptm2mmio transport).
 *
 * On an Apple SoC running under hardware SPTM, a trusted Linux guest cannot
 * touch the DART MMIO registers or own the DART translation tables: SPTM does.
 * The x1n1 hypervisor (EL2) forwards the guest's IOMMU intent to the SPTM DART
 * endpoints. This driver is the guest half: instead of programming an
 * io-pgtable and TTBRs, each map/unmap becomes an entry in an operation batch
 * that x1n1 executes against SPTM.
 *
 * Transport ("sptm2mmio"): x1n1 exposes a synthetic MMIO page whose every
 * access traps to EL2. Reads of the identification registers are emulated
 * inline. A 64-bit write to a per-device doorbell carries the guest-physical
 * address of an operation batch that the guest built in ordinary RAM; x1n1
 * leaves the guest, runs the whole batch against SPTM, and re-enters. One
 * doorbell write therefore costs one trap and one world switch for a batch of
 * up to hundreds of ops.
 *
 * SP_EL1 ABI: SPTM's guest re-dispatch ties the resumed guest's SP_EL1 to the
 * hypervisor's SP_EL2, so a doorbell write clobbers the caller's SP. Every
 * other register survives, so the doorbell is issued with the kernel SP saved
 * in a GPR and restored immediately after, under a local IRQ save.
 *
 * DT: x1n1 presents a paravirt DART as an "apple,dart-x1n1" node with the
 * doorbell page in "reg", a u32 "apple,x1n1-dart-id" (the ADT dart-id SPTM
 * knows) and #iommu-cells = <1> (the stream id).
 */

#include <linux/apple-dart-x1n1.h>
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#include "dma-iommu.h"

/* sptm2mmio register/doorbell map, shared with x1n1 src/sptm.c. */
#define X1N1_MMIO_MAGIC		0x000
#define X1N1_MMIO_VERSION	0x008
#define X1N1_MMIO_MAX_OPS	0x010
#define X1N1_MMIO_FEATURES	0x018
#define X1N1_MMIO_DART_DB	0x800	/* + 8 * dart-id */
#define X1N1_MMIO_SIZE		0x4000

#define X1N1_MAGIC		0x78316e31UL	/* "x1n1" */
#define X1N1_FEAT_DART		BIT(0)
#define X1N1_FEAT_DART_RETAINED BIT(4)

/* DART unit ops. */
#define X1N1_DART_INIT		1	/* ()                       INIT + POWERUP */
#define X1N1_DART_ATTACH	2	/* (sid, dva, level) */
#define X1N1_DART_MAP		3	/* (sid, dva, ipa, size, prot) */
#define X1N1_DART_UNMAP		4	/* (sid, dva, size) */
#define X1N1_DART_ENABLE	5	/* (sid) */
#define X1N1_DART_DISABLE	6	/* (sid) */

#define X1N1_DART_RETAINED_INFO 9
#define X1N1_DART_RETAINED_RUN 10

struct x1n1_op {
	u32 op;
	u32 flags;
	u64 arg[6];
	u64 ret;
} __packed;

struct x1n1_batch {
	u32 count;
	u32 flags;
	s32 status;
	u32 done;
	struct x1n1_op op[];
} __packed;

#define DART_X1N1_PAGE_SHIFT	14
#define DART_X1N1_PAGE_SIZE	(1UL << DART_X1N1_PAGE_SHIFT)	/* 16 KiB */
/* x1n1's DART MAP forwards one contiguous run within a single 32 MiB leaf-table
 * span, so every MAP op must stay inside one aligned block. */
#define DART_X1N1_BLOCK_SHIFT	25
#define DART_X1N1_BLOCK_SIZE	(1UL << DART_X1N1_BLOCK_SHIFT)	/* 32 MiB */

#define DART_X1N1_MAX_STREAMS	256
#define DART_X1N1_APERTURE_END	(SZ_4G - 1)
#define DART_X1N1_NATIVE_APERTURE_END GENMASK_ULL(41, 0)

/* One batch buffer per DART, sized to hold a full unmap/map of the aperture in
 * per-block chunks; kept in the linear map so its guest PA is stable. A page
 * holds (PAGE_SIZE-16)/64 ops; the callers below chunk to fit. */
#define X1N1_BATCH_MAX_OPS	((PAGE_SIZE - sizeof(struct x1n1_batch)) / \
				 sizeof(struct x1n1_op))

static const struct iommu_ops apple_dart_x1n1_ops;

struct x1n1_retained_run {
	u64 dva, ipa, size;
};
struct x1n1_retained_sid {
	unsigned int root_level, count;
	struct x1n1_retained_run *runs;
};

struct apple_dart_x1n1 {
	struct device *dev;
	struct iommu_device iommu;
	void __iomem *mmio;
	u32 dart_id;
	u32 num_streams;
	u64 iova_force; /* Native vm-force, hidden from DMA API/device addresses. */
	u64 dma_min, dma_max;
	bool inited;
	struct x1n1_retained_sid *retained[DART_X1N1_MAX_STREAMS];
	struct mutex init_lock;		/* serialises one-time init */
	/* Protected by apple_dart_x1n1_groups_lock; group-owned lifetime. */
	struct iommu_group *sid2group[DART_X1N1_MAX_STREAMS];

	struct x1n1_batch *batch;	/* linear-map buffer, guest-PA stable */
	phys_addr_t batch_pa;
	spinlock_t batch_lock;		/* protects batch + doorbell */
};

struct apple_dart_x1n1_master {
	struct apple_dart_x1n1 *dart;
	DECLARE_BITMAP(sids, DART_X1N1_MAX_STREAMS);
};

struct apple_dart_x1n1_domain {
	struct iommu_domain domain;
	struct apple_dart_x1n1 *dart;
	DECLARE_BITMAP(sids, DART_X1N1_MAX_STREAMS);
	struct mutex lock;		/* protects sids / dart binding */
	/* level-2 leaf tables already attached, keyed by (sid << 40 | block),
	 * block = dva >> 25. Installing a level-2 table twice is fatal in SPTM,
	 * so each 32 MiB window is attached at most once. */
	struct xarray l2_attached;
	bool has_retained;
	struct xarray mappings; /* tagged (guest-PFN, protection), indexed by IOVA page */
};

static struct apple_dart_x1n1_domain *to_x1n1_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_x1n1_domain, domain);
}

/* Native IODARTMapper strips vm-force before returning a device DMA address
 * and restores it for DART operations. Translate only the bounded transport
 * batch; all live-map bookkeeping and the DMA aperture remain device-visible.
 * The force bits are disjoint from the validated low-4-GiB aperture. */
static void x1n1_force_iovas(struct apple_dart_x1n1 *dart, bool restore)
{
	if (!dart->iova_force)
		return;
	for (u32 i = 0; i < dart->batch->count; i++) {
		struct x1n1_op *op = &dart->batch->op[i];

		switch (op->op) {
		case X1N1_DART_ATTACH:
		case X1N1_DART_MAP:
		case X1N1_DART_UNMAP:
			if (restore)
				op->arg[1] &= ~dart->iova_force;
			else
				op->arg[1] |= dart->iova_force;
			break;
		}
	}
}

/*
 * Ring a doorbell: publish the batch, write its guest-physical address to the
 * doorbell, and return the batch status. The doorbell write world-switches to
 * EL2, which clobbers SP_EL1 (see the file header), so save and restore the
 * kernel SP around it with interrupts masked. The caller holds batch_lock.
 */
static int x1n1_ring(struct apple_dart_x1n1 *dart, u32 db_off)
{
	void __iomem *db = dart->mmio + db_off;
	u64 batch_pa = dart->batch_pa;
	unsigned long flags;

	dart->batch->status = 0;
	dart->batch->done = 0;
	x1n1_force_iovas(dart, false);
	/* Order the batch stores before the doorbell; x1n1 does dsb on its side. */
	wmb();

	local_irq_save(flags);
	asm volatile(
		"mov	x9, sp\n"
		"str	%x[pa], [%[db]]\n"
		"mov	sp, x9\n"
		:
		: [db] "r"(db), [pa] "r"(batch_pa)
		: "x9", "memory");
	local_irq_restore(flags);

	rmb();
	x1n1_force_iovas(dart, true);
	return dart->batch->status ?: (dart->batch->done == dart->batch->count ? 0 : -EIO);
}

/* Setup-only host queries. Firmware-owned mappings are reserved, never replayed
 * through MAP, and never inserted in the Linux-owned mappings xarray. */
static int x1n1_load_retained(struct apple_dart_x1n1 *dart, u32 features)
{
	struct device_node *np = dart->dev->of_node;
	int count = of_property_count_u32_elems(np, "apple,x1n1-retained-sids");
	int ret;

	if (!of_find_property(np, "apple,x1n1-retained-sids", NULL))
		return 0;
	if (count < 1 || count > dart->num_streams || dart->iova_force ||
	    !(features & X1N1_FEAT_DART_RETAINED))
		return -EINVAL;
	for (int i = 0; i < count; i++) {
		struct x1n1_retained_sid *r;
		u32 sid;
		u64 previous = 0;
		struct x1n1_op *op = &dart->batch->op[0];

		if (of_property_read_u32_index(np, "apple,x1n1-retained-sids",
					       i, &sid) ||
		    sid >= dart->num_streams || dart->retained[sid])
			return -EINVAL;
		dart->batch->count = 1;
		*op = (struct x1n1_op){ .op = X1N1_DART_RETAINED_INFO,
					.arg = { sid } };
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		if (ret)
			return ret;
		if (op->arg[1] > 1 || op->arg[2] > 131072 || op->arg[5] != 1 ||
		    op->arg[3] != dart->dma_min ||
		    op->arg[4] != dart->dma_max - dart->dma_min + 1)
			return -EINVAL;
		r = devm_kzalloc(dart->dev, sizeof(*r), GFP_KERNEL);
		if (!r)
			return -ENOMEM;
		r->root_level = op->arg[1];
		r->count = op->arg[2];
		r->runs = devm_kcalloc(dart->dev, r->count, sizeof(*r->runs),
				       GFP_KERNEL);
		if (r->count && !r->runs)
			return -ENOMEM;
		for (u64 j = 0; j < r->count; j++) {
			struct x1n1_retained_run *run = &r->runs[j];

			*op = (struct x1n1_op){ .op = X1N1_DART_RETAINED_RUN,
						.arg = { sid, j } };
			ret = x1n1_ring(dart,
					X1N1_MMIO_DART_DB + 8 * dart->dart_id);
			if (ret)
				return ret;
			run->dva = op->arg[2];
			run->ipa = op->arg[3];
			run->size = op->arg[4];
			if (!run->size ||
			    (run->dva | run->size) &
				    (DART_X1N1_PAGE_SIZE - 1) ||
			    run->dva < dart->dma_min ||
			    run->dva > dart->dma_max ||
			    run->size - 1 > dart->dma_max - run->dva ||
			    (j && run->dva < previous) ||
			    (run->ipa != U64_MAX &&
			     ((run->ipa & (DART_X1N1_PAGE_SIZE - 1)) ||
			      run->size > U64_MAX - run->ipa)))
				return -EINVAL;
			previous = run->dva + run->size;
		}
		dart->retained[sid] = r;
	}
	return 0;
}

static const struct x1n1_retained_run *
x1n1_retained_lookup(const struct x1n1_retained_sid *r, u64 dva, u64 size)
{
	unsigned int low = 0, high = r->count;

	while (low < high) {
		unsigned int mid = low + (high - low) / 2;
		const struct x1n1_retained_run *run = &r->runs[mid];

		if (run->dva + run->size <= dva)
			low = mid + 1;
		else
			high = mid;
	}
	return low < r->count && r->runs[low].dva < dva + size ? &r->runs[low] :
								 NULL;
}

static bool x1n1_retained_overlap(struct apple_dart_x1n1_domain *dom, u64 dva,
				  u64 size)
{
	unsigned int sid;

	if (!dom->has_retained)
		return false;
	for_each_set_bit(sid, dom->sids, dom->dart->num_streams) {
		struct x1n1_retained_sid *r = dom->dart->retained[sid];

		if (r && x1n1_retained_lookup(r, dva, size))
			return true;
	}
	return false;
}

static phys_addr_t x1n1_retained_translate(struct apple_dart_x1n1_domain *dom,
					   u64 dva)
{
	unsigned int sid;
	u64 result = 0;

	if (!dom->has_retained)
		return 0;
	/* One domain may serve several streams: an inherited mapping is only
	 * unambiguous when every stream resolves it to the same guest address. */
	for_each_set_bit(sid, dom->sids, dom->dart->num_streams) {
		struct x1n1_retained_sid *r = dom->dart->retained[sid];
		const struct x1n1_retained_run *run =
			r ? x1n1_retained_lookup(r, dva, 1) : NULL;
		u64 pa;

		if (!run || dva < run->dva || run->ipa == U64_MAX)
			return 0;
		pa = run->ipa + dva - run->dva;
		if (result && result != pa)
			return 0;
		result = pa;
	}
	return result;
}

/* Bring the DART up in SPTM exactly once, before any stream is enabled. */
static int apple_dart_x1n1_ensure_init(struct apple_dart_x1n1 *dart)
{
	int ret = 0;

	mutex_lock(&dart->init_lock);
	if (!dart->inited) {
		unsigned long flags;

		spin_lock_irqsave(&dart->batch_lock, flags);
		dart->batch->count = 1;
		dart->batch->op[0] = (struct x1n1_op){ .op = X1N1_DART_INIT };
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		spin_unlock_irqrestore(&dart->batch_lock, flags);
		if (ret)
			ret = -EIO;
		else
			dart->inited = true;
	}
	mutex_unlock(&dart->init_lock);
	return ret;
}

/*
 * Optional in-guest self-test (DT "apple,x1n1-selftest"). Drives the real
 * batch path — INIT/POWERUP, attach an L2 table + enable for one stream, MAP a
 * freshly allocated guest page at a DVA, then UNMAP — and logs each batch's
 * status. Proves the driver's forwarding reaches SPTM from within Linux.
 */
static void apple_dart_x1n1_selftest(struct apple_dart_x1n1 *dart)
{
	const u32 sid = 1;
	const u64 dva = dart->iova_force ? dart->dma_min : 0x10000004000ULL;	/* the IOVA the transport was tested with */
	unsigned long flags;
	void *page;
	phys_addr_t pa;
	int ret;

	if (apple_dart_x1n1_ensure_init(dart)) {
		dev_err(dart->dev, "selftest: INIT failed\n");
		return;
	}

	page = (void *)__get_free_page(GFP_KERNEL);
	if (!page)
		return;
	pa = virt_to_phys(page);

	spin_lock_irqsave(&dart->batch_lock, flags);
	/* Install the level-1 root, then the level-2 leaf table for this DVA's
	 * 32 MiB window (level 2 requires level 1 present), enable the stream,
	 * then map one page. */
	dart->batch->count = 4;
	dart->batch->op[0] = (struct x1n1_op){ .op = X1N1_DART_ATTACH,
					       .arg = { sid, dva, 1 } };
	dart->batch->op[1] = (struct x1n1_op){ .op = X1N1_DART_ATTACH,
					       .arg = { sid, dva, 2 } };
	dart->batch->op[2] = (struct x1n1_op){ .op = X1N1_DART_ENABLE,
					       .arg = { sid } };
	dart->batch->op[3] = (struct x1n1_op){ .op = X1N1_DART_MAP,
					       .arg = { sid, dva, pa,
							DART_X1N1_PAGE_SIZE, 3 } };
	ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
	dev_info(dart->dev, "selftest: attach L1+L2+enable+map status %d done %u (dva %#llx pa %pa)\n",
		 ret, dart->batch->done, dva, &pa);

	dart->batch->count = 1;
	dart->batch->op[0] = (struct x1n1_op){ .op = X1N1_DART_UNMAP,
					       .arg = { sid, dva, DART_X1N1_PAGE_SIZE } };
	ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
	dev_info(dart->dev, "selftest: unmap status %d done %u\n",
		 ret, dart->batch->done);
	spin_unlock_irqrestore(&dart->batch_lock, flags);

	free_page((unsigned long)page);
}

/* Live mappings are indexed by IOVA page. Normal map/unmap only touches the
 * requested range; a complete walk is needed solely when adding a new SID.
 * Zero tagged values reserve XArray storage before any hardware mapping. */
#define X1N1_MAP_PROT_BITS 8

static unsigned long x1n1_mapping_value(phys_addr_t pa, int prot)
{
	return ((pa >> DART_X1N1_PAGE_SHIFT) << X1N1_MAP_PROT_BITS) | prot;
}

static int x1n1_reserve_mappings(struct apple_dart_x1n1_domain *dom,
			       unsigned long iova, size_t size, gfp_t gfp)
{
	unsigned long first = iova >> DART_X1N1_PAGE_SHIFT;
	unsigned long pages = size >> DART_X1N1_PAGE_SHIFT;
	unsigned long page;
	int ret;

	for (page = 0; page < pages; page++) {
		ret = xa_insert(&dom->mappings, first + page, xa_mk_value(0), gfp);
		if (ret)
			goto undo;
	}
	return 0;
undo:
	while (page)
		xa_erase(&dom->mappings, first + --page);
	return ret;
}

static void x1n1_release_reservations(struct apple_dart_x1n1_domain *dom,
				    unsigned long iova, size_t size)
{
	unsigned long first = iova >> DART_X1N1_PAGE_SHIFT;
	unsigned long pages = size >> DART_X1N1_PAGE_SHIFT;

	for (unsigned long page = 0; page < pages; page++)
		if (xa_load(&dom->mappings, first + page) == xa_mk_value(0))
			xa_erase(&dom->mappings, first + page);
}

static void x1n1_record_mapping(struct apple_dart_x1n1_domain *dom,
			       u64 iova, u64 pa, size_t size, int prot)
{
	for (size_t off = 0; off < size; off += DART_X1N1_PAGE_SIZE) {
		unsigned long index = (iova + off) >> DART_X1N1_PAGE_SHIFT;
		void *entry = xa_mk_value(x1n1_mapping_value(pa + off, prot));

		/* Reserved entries retain their nodes, so replacing a reservation
		 * requires no allocation, even in this IRQ-disabled section. */
		WARN_ON(xa_err(xa_store(&dom->mappings, index, entry, GFP_NOWAIT)));
	}
}

static void x1n1_forget_mapping(struct apple_dart_x1n1_domain *dom,
			       u64 iova, size_t size)
{
	for (size_t off = 0; off < size; off += DART_X1N1_PAGE_SIZE)
		xa_erase(&dom->mappings, (iova + off) >> DART_X1N1_PAGE_SHIFT);
}

static int x1n1_prepare_leaf(struct apple_dart_x1n1_domain *dom,
			   unsigned int sid, u64 iova)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned long key = ((unsigned long)sid << 40) |
			    (iova >> DART_X1N1_BLOCK_SHIFT);
	int ret;

	if (xa_load(&dom->l2_attached, key))
		return 0;
	ret = xa_insert(&dom->l2_attached, key, xa_mk_value(0), GFP_ATOMIC);
	if (ret)
		return ret;
	dart->batch->count = 1;
	dart->batch->op[0] = (struct x1n1_op){
		.op = X1N1_DART_ATTACH, .arg = { sid, iova, 2 },
	};
	ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
	if (ret) {
		xa_erase(&dom->l2_attached, key);
		return ret;
	}
	WARN_ON(xa_err(xa_store(&dom->l2_attached, key, xa_mk_value(1), GFP_NOWAIT)));
	return 0;
}

/* Return the next physically contiguous, same-protection run, stopping at a
 * leaf-table boundary. Reservations are not yet live and are skipped. */
static bool x1n1_next_mapping(struct apple_dart_x1n1_domain *dom,
			     unsigned long *cursor, struct x1n1_op *op,
			     unsigned int sid)
{
	unsigned long index = *cursor, value, first;
	void *entry;
	u64 iova, pa;
	size_t size;
	int prot;

	for (;;) {
		entry = xa_find(&dom->mappings, &index, ULONG_MAX, XA_PRESENT);
		if (!entry)
			return false;
		value = xa_to_value(entry);
		if (value)
			break;
		index++;
	}
	first = index;
	prot = value & ((1UL << X1N1_MAP_PROT_BITS) - 1);
	pa = (value >> X1N1_MAP_PROT_BITS) << DART_X1N1_PAGE_SHIFT;
	iova = (u64)first << DART_X1N1_PAGE_SHIFT;
	size = DART_X1N1_PAGE_SIZE;
	while ((iova + size) & (DART_X1N1_BLOCK_SIZE - 1)) {
		entry = xa_load(&dom->mappings, index + 1);
		if (entry != xa_mk_value(x1n1_mapping_value(pa + size, prot)))
			break;
		index++;
		size += DART_X1N1_PAGE_SIZE;
	}
	*cursor = index + 1;
	*op = (struct x1n1_op){
		.op = X1N1_DART_MAP, .arg = { sid, iova, pa, size, prot },
	};
	return true;
}

static int x1n1_undo_replay(struct apple_dart_x1n1_domain *dom,
			     unsigned int sid, u64 mapped_end)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned long cursor = 0;
	struct x1n1_op op;

	while (x1n1_next_mapping(dom, &cursor, &op, sid) && op.arg[1] < mapped_end) {
		dart->batch->count = 1;
		dart->batch->op[0] = (struct x1n1_op){
			.op = X1N1_DART_UNMAP,
			.arg = { sid, op.arg[1], min_t(u64, op.arg[3], mapped_end - op.arg[1]) },
		};
		if (x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id)) {
			dev_err(dart->dev, "failed to undo SID %u replay\n", sid);
			return -EIO;
		}
	}
	return 0;
}

/* New SID is disabled throughout replay. On failure, keep it disabled and
 * remove the successful replay prefix. Existing SIDs/maps stay untouched. */
static int x1n1_replay_sid(struct apple_dart_x1n1_domain *dom, unsigned int sid)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned long cursor = 0;
	struct x1n1_op op;
	u64 mapped_end = 0;
	u32 n = 0;
	int ret;

	while (x1n1_next_mapping(dom, &cursor, &op, sid)) {
		if (dart->retained[sid] && x1n1_retained_lookup(dart->retained[sid], op.arg[1], op.arg[3]))
			return -EBUSY;
		ret = x1n1_prepare_leaf(dom, sid, op.arg[1]);
		if (ret)
			return ret;
	}
	cursor = 0;
	for (;;) {
		bool have = x1n1_next_mapping(dom, &cursor, &op, sid);

		if (have)
			dart->batch->op[n++] = op;
		if (n == X1N1_BATCH_MAX_OPS || (!have && n)) {
			dart->batch->count = n;
			ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
			for (u32 i = 0; i < min(dart->batch->done, n); i++)
				mapped_end = dart->batch->op[i].arg[1] +
					     dart->batch->op[i].arg[3];
			if (ret)
				goto undo;
			n = 0;
		}
		if (!have)
			return 0;
	}
undo:
	x1n1_undo_replay(dom, sid, mapped_end);
	return ret;
}

static int apple_dart_x1n1_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart;
	unsigned long flags;
	unsigned int sid;
	DECLARE_BITMAP(added_sids, DART_X1N1_MAX_STREAMS);
	struct apple_dart_x1n1_domain *prior = NULL;
	DECLARE_BITMAP(moved_sids, DART_X1N1_MAX_STREAMS);
	int ret;

	if (!master)
		return -ENODEV;
	dart = master->dart;

	ret = apple_dart_x1n1_ensure_init(dart);
	if (ret)
		return ret;

	if (old && old != domain && old->owner == &apple_dart_x1n1_ops &&
	    old->ops == apple_dart_x1n1_ops.default_domain_ops)
		prior = to_x1n1_domain(old);
	if (prior && (unsigned long)prior < (unsigned long)dom) {
		mutex_lock(&prior->lock);
		mutex_lock_nested(&dom->lock, SINGLE_DEPTH_NESTING);
	} else {
		mutex_lock(&dom->lock);
		if (prior)
			mutex_lock_nested(&prior->lock, SINGLE_DEPTH_NESTING);
	}
	if (dom->dart && dom->dart != dart) {
		if (prior)
			mutex_unlock(&prior->lock);
		mutex_unlock(&dom->lock);
		return -EINVAL;
	}
	dom->dart = dart;
	domain->geometry.aperture_start = dart->dma_min;
	domain->geometry.aperture_end = dart->dma_max;

	/* A new stream must see all existing mappings before it can DMA. */
	spin_lock_irqsave(&dart->batch_lock, flags);
	ret = 0;
	bitmap_zero(added_sids, DART_X1N1_MAX_STREAMS);
	bitmap_zero(moved_sids, DART_X1N1_MAX_STREAMS);
	for_each_set_bit(sid, master->sids, dart->num_streams) {
		if (test_bit(sid, dom->sids))
			continue;
		if (dart->retained[sid] && prior && prior->dart == dart &&
		    test_bit(sid, prior->sids)) {
			set_bit(sid, moved_sids);
			ret = x1n1_undo_replay(prior, sid, U64_MAX);
			if (ret)
				break;
		}
		if (dart->retained[sid]) {
			dart->batch->count = 1;
			dart->batch->op[0] = (struct x1n1_op){
				.op = X1N1_DART_RETAINED_INFO,
				.arg = { sid },
			};
		} else {
			dart->batch->count = 2;
			dart->batch->op[0] = (struct x1n1_op){
				.op = dart->retained[sid] ?
					      X1N1_DART_RETAINED_INFO :
					      X1N1_DART_DISABLE,
				.arg = { sid },
			};
			dart->batch->op[1] = (struct x1n1_op){
				.op = X1N1_DART_ATTACH,
				.arg = { sid, dart->dma_min, 1 },
			};
		}
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		if (ret)
			break;
		ret = x1n1_replay_sid(dom, sid);
		if (ret)
			break;
		dart->batch->count = 1;
		dart->batch->op[0] = (struct x1n1_op){
			.op = dart->retained[sid] ? X1N1_DART_RETAINED_INFO :
						    X1N1_DART_ENABLE,
			.arg = { sid },
		};
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		if (ret) {
			/* A failed completion may still have enabled the SID. */
			dart->batch->count = 1;
			dart->batch->op[0] = (struct x1n1_op){
				.op = dart->retained[sid] ?
					      X1N1_DART_RETAINED_INFO :
					      X1N1_DART_DISABLE,
				.arg = { sid },
			};
			if (x1n1_ring(dart,
				      X1N1_MMIO_DART_DB + 8 * dart->dart_id))
				dev_err(dart->dev,
					"failed to disable new SID %u after enable error\n",
					sid);
			x1n1_undo_replay(dom, sid, U64_MAX);
			break;
		}
		dom->has_retained |= !!dart->retained[sid];
		set_bit(sid, dom->sids);
		set_bit(sid, added_sids);
	}
	if (ret) {
		/* A master can contain multiple SIDs. Undo those added by this
		 * failed attachment without disturbing streams already in use. */
		for_each_set_bit(sid, added_sids, dart->num_streams) {
			dart->batch->count = 1;
			dart->batch->op[0] = (struct x1n1_op){
				.op = dart->retained[sid] ?
					      X1N1_DART_RETAINED_INFO :
					      X1N1_DART_DISABLE,
				.arg = { sid },
			};
			if (x1n1_ring(dart,
				      X1N1_MMIO_DART_DB + 8 * dart->dart_id))
				dev_err(dart->dev,
					"failed to disable new SID %u after attachment error\n",
					sid);
			x1n1_undo_replay(dom, sid, U64_MAX);
			clear_bit(sid, dom->sids);
		}
	}
	for_each_set_bit(sid, moved_sids, dart->num_streams) {
		if (ret) {
			if (x1n1_replay_sid(prior, sid))
				dev_err(dart->dev,
					"failed to restore previous domain SID %u\n",
					sid);
		} else {
			clear_bit(sid, prior->sids);
		}
	}
	spin_unlock_irqrestore(&dart->batch_lock, flags);

	if (prior)
		mutex_unlock(&prior->lock);
	mutex_unlock(&dom->lock);
	return ret ? -EIO : 0;
}

struct x1n1_map_progress {
	size_t mapped;
	unsigned int streams;
	struct x1n1_op partial;
};

/* Track only fully installed stream cohorts. The batch may split one cohort
 * without reducing batching; keep its progress across doorbells. */
static int x1n1_flush_maps(struct apple_dart_x1n1_domain *dom,
			 struct x1n1_map_progress *progress)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned int total_streams = bitmap_weight(dom->sids, dart->num_streams);
	int ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);

	for (u32 i = 0; i < min(dart->batch->done, dart->batch->count); i++) {
		struct x1n1_op *op = &dart->batch->op[i];

		progress->partial = *op;
		if (++progress->streams == total_streams) {
			x1n1_record_mapping(dom, op->arg[1], op->arg[2], op->arg[3], op->arg[4]);
			progress->mapped += op->arg[3];
			progress->streams = 0;
		}
	}
	if (ret && progress->streams) {
		unsigned int sid;
		u32 n = 0;

		for_each_set_bit(sid, dom->sids, dart->num_streams) {
			if (n == progress->streams)
				break;
			dart->batch->op[n++] = (struct x1n1_op){
				.op = X1N1_DART_UNMAP,
				.arg = { sid, progress->partial.arg[1], progress->partial.arg[3] },
			};
		}
		dart->batch->count = n;
		if (x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id))
			dev_err(dart->dev, "failed to undo partial DART map\n");
	}
	return ret;
}

static int apple_dart_x1n1_map_pages(struct iommu_domain *domain,
				     unsigned long iova, phys_addr_t paddr,
				     size_t pgsize, size_t pgcount, int prot,
				     gfp_t gfp, size_t *mapped)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart = dom->dart;
	struct x1n1_map_progress progress = {};
	size_t total;
	unsigned long flags;
	u32 n = 0;
	int ret;

	*mapped = 0;
	if (!dart || bitmap_empty(dom->sids, dart->num_streams))
		return -ENODEV;
	if (pgsize != DART_X1N1_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova | paddr) & (DART_X1N1_PAGE_SIZE - 1) ||
	    prot & ~((1U << X1N1_MAP_PROT_BITS) - 1) ||
	    !paddr || iova < dart->dma_min || iova > dart->dma_max ||
	    total - 1 > dart->dma_max - iova ||
	    iova + total > DART_X1N1_NATIVE_APERTURE_END)
		return -EINVAL;
	if (x1n1_retained_overlap(dom, iova, total))
		return -EBUSY;
	ret = x1n1_reserve_mappings(dom, iova, total, gfp);
	if (ret)
		return ret;
	spin_lock_irqsave(&dart->batch_lock, flags);
	/* Install all leaf tables before accumulating the mapping batch. */
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk = min_t(size_t, total - pos,
				    DART_X1N1_BLOCK_SIZE - (cur & (DART_X1N1_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, dom->sids, dart->num_streams) {
			ret = x1n1_prepare_leaf(dom, sid, cur);
			if (ret)
				goto out;
		}
		pos += chunk;
	}
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk = min_t(size_t, total - pos,
				    DART_X1N1_BLOCK_SIZE - (cur & (DART_X1N1_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, dom->sids, dart->num_streams) {
			dart->batch->op[n++] = (struct x1n1_op){
				.op = X1N1_DART_MAP,
				.arg = { sid, cur, paddr + pos, chunk, prot },
			};
			if (n == X1N1_BATCH_MAX_OPS) {
				dart->batch->count = n;
				ret = x1n1_flush_maps(dom, &progress);
				if (ret)
					goto out;
				n = 0;
			}
		}
		pos += chunk;
	}
	if (n) {
		dart->batch->count = n;
		ret = x1n1_flush_maps(dom, &progress);
	}
out:
	spin_unlock_irqrestore(&dart->batch_lock, flags);
	*mapped = progress.mapped;
	/* Remove unused reservations after a partial failure; committed mappings
	 * remain available both to caller unwind and a later stream attachment. */
	if (ret)
		x1n1_release_reservations(dom, iova + progress.mapped, total - progress.mapped);
	return ret ? -EIO : 0;
}

static int x1n1_restore_partial_unmap(struct apple_dart_x1n1_domain *dom,
				    const struct x1n1_op *partial, unsigned int streams)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned long cursor = partial->arg[1] >> DART_X1N1_PAGE_SHIFT;
	u64 end = partial->arg[1] + partial->arg[2];
	struct x1n1_op op;
	u32 n = 0;

	while (x1n1_next_mapping(dom, &cursor, &op, 0) && op.arg[1] < end) {
		unsigned int sid, count = 0;

		op.arg[3] = min_t(u64, op.arg[3], end - op.arg[1]);
		for_each_set_bit(sid, dom->sids, dart->num_streams) {
			if (count++ == streams)
				break;
			op.arg[0] = sid;
			dart->batch->op[n++] = op;
			if (n == X1N1_BATCH_MAX_OPS) {
				dart->batch->count = n;
				if (x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id))
					return -EIO;
				n = 0;
			}
		}
	}
	if (!n)
		return 0;
	dart->batch->count = n;
	return x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
}

static int x1n1_flush_unmaps(struct apple_dart_x1n1_domain *dom,
			   struct x1n1_map_progress *progress)
{
	struct apple_dart_x1n1 *dart = dom->dart;
	unsigned int total_streams = bitmap_weight(dom->sids, dart->num_streams);
	int ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);

	for (u32 i = 0; i < min(dart->batch->done, dart->batch->count); i++) {
		struct x1n1_op *op = &dart->batch->op[i];

		progress->partial = *op;
		if (++progress->streams == total_streams) {
			x1n1_forget_mapping(dom, op->arg[1], op->arg[2]);
			progress->mapped += op->arg[2];
			progress->streams = 0;
		}
	}
	if (ret && progress->streams &&
	    x1n1_restore_partial_unmap(dom, &progress->partial, progress->streams))
		dev_err(dart->dev, "failed to restore partial DART unmap\n");
	return ret;
}

static size_t apple_dart_x1n1_unmap_pages(struct iommu_domain *domain,
					  unsigned long iova, size_t pgsize,
					  size_t pgcount,
					  struct iommu_iotlb_gather *gather)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart = dom->dart;
	struct x1n1_map_progress progress = {};
	size_t total;
	unsigned long flags;
	u32 n = 0;

	if (!dart || bitmap_empty(dom->sids, dart->num_streams) ||
	    pgsize != DART_X1N1_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova & (DART_X1N1_PAGE_SIZE - 1)) ||
	    iova < dart->dma_min || iova > dart->dma_max ||
	    total - 1 > dart->dma_max - iova)
		return 0;
	if (x1n1_retained_overlap(dom, iova, total))
		return 0;
	spin_lock_irqsave(&dart->batch_lock, flags);
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk = min_t(size_t, total - pos,
				    DART_X1N1_BLOCK_SIZE - (cur & (DART_X1N1_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, dom->sids, dart->num_streams) {
			dart->batch->op[n++] = (struct x1n1_op){
				.op = X1N1_DART_UNMAP, .arg = { sid, cur, chunk },
			};
			if (n == X1N1_BATCH_MAX_OPS) {
				dart->batch->count = n;
				if (x1n1_flush_unmaps(dom, &progress))
					goto out;
				n = 0;
			}
		}
		pos += chunk;
	}
	if (n) {
		dart->batch->count = n;
		x1n1_flush_unmaps(dom, &progress);
	}
out:
	spin_unlock_irqrestore(&dart->batch_lock, flags);
	return progress.mapped;
}

static void apple_dart_x1n1_iotlb_sync(struct iommu_domain *domain,
				       struct iommu_iotlb_gather *gather)
{
	/* x1n1 invalidates inside every MAP/UNMAP it forwards to SPTM, so a
	 * separate flush is a no-op. */
}

static phys_addr_t apple_dart_x1n1_iova_to_phys(struct iommu_domain *domain,
						dma_addr_t iova)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	void *entry = xa_load(&dom->mappings, iova >> DART_X1N1_PAGE_SHIFT);
	unsigned long value = entry ? xa_to_value(entry) : 0;

	if (!value)
		return x1n1_retained_translate(dom, iova);
	return ((value >> X1N1_MAP_PROT_BITS) << DART_X1N1_PAGE_SHIFT) |
	       (iova & (DART_X1N1_PAGE_SIZE - 1));
}

static void apple_dart_x1n1_domain_free(struct iommu_domain *domain)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);

	if (dom->has_retained && dom->dart) {
		unsigned int sid;
		unsigned long flags;

		spin_lock_irqsave(&dom->dart->batch_lock, flags);
		for_each_set_bit(sid, dom->sids, dom->dart->num_streams)
			x1n1_undo_replay(dom, sid, U64_MAX);
		spin_unlock_irqrestore(&dom->dart->batch_lock, flags);
	}
	xa_destroy(&dom->l2_attached);
	xa_destroy(&dom->mappings);
	kfree(dom);
}

static struct iommu_domain *
apple_dart_x1n1_domain_alloc_paging(struct device *dev)
{
	struct apple_dart_x1n1_domain *dom;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	mutex_init(&dom->lock);
	xa_init(&dom->l2_attached);
	xa_init(&dom->mappings);
	dom->domain.pgsize_bitmap = DART_X1N1_PAGE_SIZE;
	dom->domain.geometry.aperture_start = 0;
	dom->domain.geometry.aperture_end = DART_X1N1_APERTURE_END;
	dom->domain.geometry.force_aperture = true;

	if (dev) {
		struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);

		if (master) {
			dom->dart = master->dart;
			dom->domain.geometry.aperture_start = master->dart->dma_min;
			dom->domain.geometry.aperture_end = master->dart->dma_max;
		}
	}

	return &dom->domain;
}

static struct iommu_device *apple_dart_x1n1_probe_device(struct device *dev)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);

	if (!dev_iommu_fwspec_get(dev) || !master)
		return ERR_PTR(-ENODEV);

	device_link_add(dev, master->dart->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);

	return &master->dart->iommu;
}

static void apple_dart_x1n1_release_device(struct device *dev)
{
	kfree(dev_iommu_priv_get(dev));
}

static int apple_dart_x1n1_of_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	struct platform_device *iommu_pdev;
	struct apple_dart_x1n1 *dart;
	u32 sid;

	if (args->args_count != 1)
		return -EINVAL;
	sid = args->args[0];
	if (sid >= DART_X1N1_MAX_STREAMS)
		return -EINVAL;

	iommu_pdev = of_find_device_by_node(args->np);
	if (!iommu_pdev)
		return -ENODEV;
	dart = platform_get_drvdata(iommu_pdev);
	put_device(&iommu_pdev->dev);
	if (!dart)
		return -ENODEV;

	if (!master) {
		master = kzalloc(sizeof(*master), GFP_KERNEL);
		if (!master)
			return -ENOMEM;
		master->dart = dart;
		dev_iommu_priv_set(dev, master);
	} else if (master->dart != dart) {
		/* This driver forwards per (dart-id, sid); a device spanning
		 * two paravirt DARTs is not modelled. */
		return -EINVAL;
	}

	if (sid >= dart->num_streams)
		return -EINVAL;

	set_bit(sid, master->sids);
	return 0;
}

/* Devices sharing a stream cannot have independent IOVA allocators. PCI
 * multifunction/topology grouping is also required even with distinct SIDs.
 * Mirrors the native Apple DART lifetime model: the group owns its SID union,
 * and its release callback clears the reverse lookup before freeing it. */
static DEFINE_MUTEX(apple_dart_x1n1_groups_lock);

static void apple_dart_x1n1_release_group(void *data)
{
	struct apple_dart_x1n1_master *group_master = data;
	unsigned int sid;

	mutex_lock(&apple_dart_x1n1_groups_lock);
	for_each_set_bit(sid, group_master->sids, group_master->dart->num_streams)
		group_master->dart->sid2group[sid] = NULL;
	mutex_unlock(&apple_dart_x1n1_groups_lock);
	kfree(group_master);
}

static struct iommu_group *apple_dart_x1n1_device_group(struct device *dev)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	struct apple_dart_x1n1_master *group_master;
	struct iommu_group *group = NULL, *put_group = NULL;
	struct iommu_group *result = ERR_PTR(-EINVAL);
	unsigned int sid;

	if (!master || bitmap_empty(master->sids, master->dart->num_streams))
		return result;

	mutex_lock(&apple_dart_x1n1_groups_lock);
	for_each_set_bit(sid, master->sids, master->dart->num_streams) {
		struct iommu_group *existing = master->dart->sid2group[sid];

		if (!existing)
			continue;
		if (group && existing != group)
			goto out;
		group = existing;
	}

	if (group) {
		group = iommu_group_ref_get(group);
	} else {
#ifdef CONFIG_PCI
		if (dev_is_pci(dev))
			group = pci_device_group(dev);
		else
#endif
			group = generic_device_group(dev);
	}
	if (IS_ERR(group)) {
		result = group;
		goto out;
	}
	if (!group) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}

	/* A PCI topology group may already contain another function. */
	group_master = iommu_group_get_iommudata(group);
	if (group_master && group_master->dart != master->dart) {
		put_group = group;
		goto out;
	}
	if (!group_master) {
		group_master = kmemdup(master, sizeof(*master), GFP_KERNEL);
		if (!group_master) {
			result = ERR_PTR(-ENOMEM);
			put_group = group;
			goto out;
		}
		iommu_group_set_iommudata(group, group_master,
					 apple_dart_x1n1_release_group);
	} else {
		bitmap_or(group_master->sids, group_master->sids, master->sids,
			  master->dart->num_streams);
	}

	for_each_set_bit(sid, master->sids, master->dart->num_streams)
		master->dart->sid2group[sid] = group;
	result = group;
out:
	mutex_unlock(&apple_dart_x1n1_groups_lock);
	/* Dropping the last reference can invoke release_group: do it unlocked. */
	if (put_group)
		iommu_group_put(put_group);
	return result;
}

static bool apple_dart_x1n1_capable(struct device *dev, enum iommu_cap cap)
{
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

/* PCIe captures the MSI doorbell before DART translation. Advertise the
 * identity MSI page to the DMA layer so it never tries to map this non-RAM
 * address through the SPTM transport. Other devices retain normal reservations.
 * This follows apple-dart.c's hardware MSI reservation (Asahi contributors).
 */
static void apple_dart_x1n1_get_resv_regions(struct device *dev,
										   struct list_head *head)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	unsigned int sid;

	if (master) {
		/* SPTM converts both start and exclusive end with a strict 42-bit
		 * address check (26A428: 270d63d8/270d640c). A range ending at
		 * 4 TiB is rejected, so keep its final granule out of allocation. */
		if (master->dart->dma_max == DART_X1N1_NATIVE_APERTURE_END) {
			struct iommu_resv_region *region =
				iommu_alloc_resv_region(
					DART_X1N1_NATIVE_APERTURE_END + 1 -
						DART_X1N1_PAGE_SIZE,
					DART_X1N1_PAGE_SIZE, 0,
					IOMMU_RESV_RESERVED, GFP_KERNEL);

			if (region)
				list_add_tail(&region->list, head);
		}
		for_each_set_bit(sid, master->sids, master->dart->num_streams) {
			struct x1n1_retained_sid *r =
				master->dart->retained[sid];
			if (!r)
				continue;
			for (unsigned int i = 0; i < r->count; i++) {
				struct iommu_resv_region *region =
					iommu_alloc_resv_region(
						r->runs[i].dva, r->runs[i].size,
						0, IOMMU_RESV_RESERVED,
						GFP_KERNEL);
				if (region)
					list_add_tail(&region->list, head);
			}
		}
	}
	if (dev_is_pci(dev)) {
		struct pci_host_bridge *bridge = pci_find_host_bridge(to_pci_dev(dev)->bus);
		struct device_node *np = bridge->dev.parent->of_node;
		struct iommu_resv_region *region;
		u64 address;

		if (of_device_is_compatible(np, "apple,x1n1-pcie") &&
		    !of_property_read_u64(np, "apple,msi-address", &address)) {
			region = iommu_alloc_resv_region(address & PAGE_MASK, PAGE_SIZE,
				IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO,
				IOMMU_RESV_MSI, GFP_KERNEL);
			if (region)
				list_add_tail(&region->list, head);
		}
	}
	iommu_dma_get_resv_regions(dev, head);
}

static const struct iommu_ops apple_dart_x1n1_ops = {
	.get_resv_regions = apple_dart_x1n1_get_resv_regions,
	.capable		= apple_dart_x1n1_capable,
	.domain_alloc_paging	= apple_dart_x1n1_domain_alloc_paging,
	.probe_device		= apple_dart_x1n1_probe_device,
	.release_device		= apple_dart_x1n1_release_device,
	.device_group		= apple_dart_x1n1_device_group,
	.of_xlate		= apple_dart_x1n1_of_xlate,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= apple_dart_x1n1_attach_dev,
		.map_pages		= apple_dart_x1n1_map_pages,
		.unmap_pages		= apple_dart_x1n1_unmap_pages,
		.iotlb_sync		= apple_dart_x1n1_iotlb_sync,
		.iova_to_phys		= apple_dart_x1n1_iova_to_phys,
		.free			= apple_dart_x1n1_domain_free,
	},
};

/* Used only after the PCI host has stopped every endpoint on this DART.
 * Native POWERDOWN/POWERUP retains the SPTM-owned tables and live mappings. */
int apple_dart_x1n1_set_power(struct device *dev, bool on)
{
	struct apple_dart_x1n1_master *master;
	struct apple_dart_x1n1 *dart;
	unsigned long flags;
	int ret;

	if (!dev->iommu || !dev->iommu->iommu_dev ||
	    dev->iommu->iommu_dev->ops != &apple_dart_x1n1_ops)
		return -ENODEV;
	master = dev_iommu_priv_get(dev);
	if (!master)
		return -ENODEV;
	dart = master->dart;
	mutex_lock(&dart->init_lock);
	if (!dart->inited) {
		ret = -EINVAL;
		goto out;
	}
	spin_lock_irqsave(&dart->batch_lock, flags);
	dart->batch->count = 1;
	dart->batch->op[0] = (struct x1n1_op){ .op = on ? 7 : 8 };
	ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
	spin_unlock_irqrestore(&dart->batch_lock, flags);
out:
	mutex_unlock(&dart->init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_x1n1_set_power);

static int apple_dart_x1n1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dart_x1n1 *dart;
	u32 magic, features;
	int ret;

	dart = devm_kzalloc(dev, sizeof(*dart), GFP_KERNEL);
	if (!dart)
		return -ENOMEM;

	dart->dev = dev;
	mutex_init(&dart->init_lock);
	spin_lock_init(&dart->batch_lock);

	dart->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dart->mmio))
		return PTR_ERR(dart->mmio);

	if (of_property_read_u32(dev->of_node, "apple,x1n1-dart-id",
				 &dart->dart_id))
		return dev_err_probe(dev, -EINVAL,
				     "missing apple,x1n1-dart-id\n");
	if (dart->dart_id > 0xff)
		return dev_err_probe(dev, -EINVAL, "dart-id out of range\n");

	dart->dma_max = DART_X1N1_APERTURE_END;
	if (of_find_property(dev->of_node, "apple,dma-range", NULL)) {
		u64 range[2], end;

		if (of_property_read_u64_array(dev->of_node, "apple,dma-range", range, 2) ||
		    !range[1] || (range[0] | range[1]) & (DART_X1N1_PAGE_SIZE - 1) ||
		    check_add_overflow(range[0], range[1] - 1, &end) ||
		    end > DART_X1N1_NATIVE_APERTURE_END)
			return dev_err_probe(dev, -EINVAL, "bad DMA range\n");
		dart->dma_min = range[0];
		dart->dma_max = end;
	}
	if (of_find_property(dev->of_node, "apple,x1n1-iova-force", NULL)) {
		if (of_property_read_u64(dev->of_node, "apple,x1n1-iova-force",
					 &dart->iova_force) ||
		    (dart->iova_force && dart->iova_force != BIT_ULL(40)))
			return dev_err_probe(dev, -EINVAL, "unsupported IOVA force mask\n");
		/* A native DVA aperture already contains its high address bits.
		 * Hidden force bits are only valid for the low-address device view.
		 */
		if (dart->iova_force && dart->dma_max >= dart->iova_force)
			return dev_err_probe(dev, -EINVAL, "IOVA force overlaps DMA aperture\n");
	}

	dart->num_streams = DART_X1N1_MAX_STREAMS;
	of_property_read_u32(dev->of_node, "apple,x1n1-num-streams",
			     &dart->num_streams);
	if (!dart->num_streams || dart->num_streams > DART_X1N1_MAX_STREAMS)
		return dev_err_probe(dev, -EINVAL, "bad stream count\n");

	/* Confirm we really are under x1n1: the emulated MAGIC register reads
	 * back "x1n1", and the DART feature bit is advertised. */
	magic = readl(dart->mmio + X1N1_MMIO_MAGIC);
	features = readl(dart->mmio + X1N1_MMIO_FEATURES);
	if (magic != (u32)X1N1_MAGIC || !(features & X1N1_FEAT_DART))
		return dev_err_probe(dev, -ENODEV,
				     "x1n1 paravirt IOMMU not present (magic 0x%x)\n",
				     magic);

	/* Batch buffer in the linear map so its guest PA is stable and x1n1
	 * can read it by guest-physical address. */
	dart->batch = (void *)devm_get_free_pages(dev, GFP_KERNEL, 0);
	if (!dart->batch)
		return -ENOMEM;
	dart->batch_pa = virt_to_phys(dart->batch);
	ret = x1n1_load_retained(dart, features);
	if (ret)
		return dev_err_probe(dev, ret, "retained DART discovery failed\n");

	platform_set_drvdata(pdev, dart);

	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL, "apple-dart-x1n1.%s",
				     dev_name(dev));
	if (ret)
		return ret;

	ret = iommu_device_register(&dart->iommu, &apple_dart_x1n1_ops, dev);
	if (ret)
		goto err_sysfs;

	dev_info(dev, "x1n1 paravirt DART id %u, %u streams (sptm2mmio)\n",
		 dart->dart_id, dart->num_streams);

	if (of_property_read_bool(dev->of_node, "apple,x1n1-selftest"))
		apple_dart_x1n1_selftest(dart);
	return 0;

err_sysfs:
	iommu_device_sysfs_remove(&dart->iommu);
	return ret;
}

static void apple_dart_x1n1_remove(struct platform_device *pdev)
{
	struct apple_dart_x1n1 *dart = platform_get_drvdata(pdev);

	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);
}

static const struct of_device_id apple_dart_x1n1_of_match[] = {
	{ .compatible = "apple,dart-x1n1" },
	{ },
};
MODULE_DEVICE_TABLE(of, apple_dart_x1n1_of_match);

static struct platform_driver apple_dart_x1n1_driver = {
	.driver = {
		.name = "apple-dart-x1n1",
		.of_match_table = apple_dart_x1n1_of_match,
	},
	.probe = apple_dart_x1n1_probe,
	.remove = apple_dart_x1n1_remove,
};
module_platform_driver(apple_dart_x1n1_driver);

MODULE_DESCRIPTION("Paravirtualised Apple DART for the x1n1 hypervisor");
MODULE_LICENSE("GPL");
