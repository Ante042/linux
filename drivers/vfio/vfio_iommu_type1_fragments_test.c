// SPDX-License-Identifier: GPL-2.0-only
/* Actual type1 lifecycle with recorded translations; no physical device DMA. */
#include <linux/shmem_fs.h>
#include <linux/mman.h>
#include <linux/sizes.h>

#define VFIO_TEST_VA SZ_64M
#define VFIO_TEST_IOVA SZ_256M

struct vfio_test_domain {
	struct iommu_domain core;
	struct vfio_domain type1;
	struct xarray translations;
	unsigned long mapped;
	unsigned long fail_after;
	unsigned long largest_map;
	unsigned int syncs;
	struct folio *observed;
	struct kunit *test;
};

struct vfio_fragment_fixture {
	struct vfio_iommu *iommu;
	struct vfio_test_domain domains[2];
	struct vfio_iommu_group group;
	struct vfio_device device;
	struct kunit *test;
	unsigned int external_refs;
	unsigned int notifications;
};

static int vfio_test_domain_map(struct iommu_domain *domain, unsigned long iova,
		phys_addr_t phys, size_t pgsize, size_t pgcount, int prot,
		gfp_t gfp, size_t *mapped)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	unsigned long length = pgsize * pgcount;

	mock->largest_map = max(mock->largest_map, length);
	for (unsigned long done = 0; done < length; done += SZ_4K) {
		int ret;

		if (mock->fail_after && mock->mapped >= mock->fail_after)
			return -ENOMEM;
		ret = xa_insert(&mock->translations, (iova + done) >> 12,
				xa_mk_value((phys + done) >> 12), gfp);
		if (ret)
			return ret;
		mock->mapped += SZ_4K;
		*mapped += SZ_4K;
	}
	return 0;
}

static size_t vfio_test_domain_unmap(struct iommu_domain *domain, unsigned long iova,
		size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	unsigned long length = pgsize * pgcount, done;

	for (done = 0; done < length; done += SZ_4K) {
		if (!xa_erase(&mock->translations, (iova + done) >> 12))
			break;
		mock->mapped -= SZ_4K;
	}
	return done;
}

static phys_addr_t vfio_test_domain_translate(struct iommu_domain *domain, dma_addr_t iova)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	void *entry = xa_load(&mock->translations, iova >> 12);

	return entry ? ((phys_addr_t)xa_to_value(entry) << 12) + (iova & (SZ_4K - 1)) : 0;
}

static void vfio_test_domain_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);

	mock->syncs++;
	if (mock->observed)
		KUNIT_EXPECT_TRUE(mock->test, folio_maybe_dma_pinned(mock->observed));
}

static const struct iommu_domain_ops vfio_test_domain_ops = {
	.map_pages = vfio_test_domain_map,
	.unmap_pages = vfio_test_domain_unmap,
	.iova_to_phys = vfio_test_domain_translate,
	.iotlb_sync = vfio_test_domain_sync,
};

static void vfio_test_notify(struct vfio_device *device, u64 iova, u64 length)
{
	struct vfio_fragment_fixture *f = container_of(device, typeof(*f), device);

	f->notifications++;
	while (f->external_refs) {
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
		f->external_refs--;
	}
}

static const struct vfio_device_ops vfio_test_device_ops = {
	.dma_unmap = vfio_test_notify,
};

static void vfio_fragment_fixture_free(void *ptr)
{
	struct vfio_fragment_fixture *f = ptr;
	struct rb_node *node;

	while ((node = rb_first(&f->iommu->dma_list))) {
		struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);
		struct rb_node *pin;

		while ((pin = rb_first(&dma->pfn_list))) {
			struct vfio_pfn *vpfn = rb_entry(pin, struct vfio_pfn, node);

			vfio_unpin_page_external(dma, vpfn->iova,
						list_empty(&f->iommu->domain_list));
		}
		vfio_remove_dma(f->iommu, dma);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		KUNIT_EXPECT_EQ(f->test, f->domains[i].mapped, 0UL);
		KUNIT_EXPECT_TRUE(f->test, xa_empty(&f->domains[i].translations));
		xa_destroy(&f->domains[i].translations);
	}
	kfree(f->iommu);
}

static struct vfio_fragment_fixture *vfio_fragment_fixture_new(struct kunit *test)
{
	struct vfio_fragment_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->test = test;
	f->iommu = vfio_iommu_type1_open(VFIO_TYPE1v2_IOMMU);
	if (IS_ERR(f->iommu))
		return NULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		struct vfio_test_domain *mock = &f->domains[i];

		xa_init(&mock->translations);
		mock->test = test;
		mock->core.ops = &vfio_test_domain_ops;
		mock->core.type = IOMMU_DOMAIN_UNMANAGED;
		mock->core.pgsize_bitmap = SZ_4K | PAGE_SIZE;
		mock->type1.domain = &mock->core;
		INIT_LIST_HEAD(&mock->type1.next);
		INIT_LIST_HEAD(&mock->type1.group_list);
	}
	f->group.iommu_group = (struct iommu_group *)f;
	list_add(&f->group.next, &f->iommu->emulated_iommu_groups);
	f->iommu->num_non_pinned_groups = 1;
	f->device.ops = &vfio_test_device_ops;
	vfio_iommu_type1_register_device(f->iommu, &f->device);
	if (kunit_add_action_or_reset(test, vfio_fragment_fixture_free, f))
		return NULL;
	return f;
}

static void vfio_test_file_put(void *ptr)
{
	fput(ptr);
}

static struct file *vfio_fragment_file(struct kunit *test, unsigned long length,
				     unsigned char value)
{
	struct file *file = shmem_file_setup("vfio-fragments", length, EMPTY_VMA_FLAGS);
	unsigned char *data;
	loff_t pos = 0;

	if (IS_ERR(file))
		return file;
	if (kunit_add_action_or_reset(test, vfio_test_file_put, file))
		return ERR_PTR(-ENOMEM);
	data = kunit_kmalloc(test, length, GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);
	memset(data, value, length);
	if (kernel_write(file, data, length, &pos) != length)
		return ERR_PTR(-EIO);
	return file;
}

static unsigned long vfio_fragment_source(struct vfio_account_mm *ctx,
					 struct file *file, unsigned long length)
{
	unsigned long addr;

	kthread_use_mm(ctx->mm);
	addr = vm_mmap(file, VFIO_TEST_VA, length, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_FIXED_NOREPLACE, 0);
	kthread_unuse_mm(ctx->mm);
	return addr;
}

static int vfio_fragment_test_map(struct vfio_fragment_fixture *f,
				  struct vfio_account_mm *ctx,
				  unsigned long offset, unsigned long length)
{
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = VFIO_TEST_VA + offset,
		.iova = VFIO_TEST_IOVA,
		.size = length,
	};
	int ret;

	kthread_use_mm(ctx->mm);
	ret = vfio_dma_do_map(f->iommu, &map);
	kthread_unuse_mm(ctx->mm);
	return ret;
}

static void vfio_fragments_lifetime_test(struct kunit *test)
{
	unsigned int shifts[] = { 12, 14 };

	for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
		struct vfio_account_mm *ctx;
		struct vfio_fragment_fixture *f;
		struct vfio_dma *dma;
		struct file *file;
		struct page *page;
		unsigned char data[64];
		struct vfio_iommu_type1_dma_unmap unmap = {
			.argsz = sizeof(unmap), .iova = VFIO_TEST_IOVA, .size = 2 * PAGE_SIZE,
		};
		struct vfio_bitmap bitmap = {};

		if (shifts[i] == PAGE_SHIFT)
			continue;
		ctx = vfio_account_mm_new(test, shifts[i]);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x71);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		f = vfio_fragment_fixture_new(test);
		KUNIT_ASSERT_NOT_NULL(test, f);
		list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
		vfio_update_pgsize_bitmap(f->iommu);
		KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
		dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		for (unsigned int d = 0; d < 2; d++)
			f->domains[d].observed = vfio_fragment_find(dma, 0)->pin.folio;
		KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 2 * PAGE_SIZE);
		KUNIT_EXPECT_GE(test, f->domains[0].largest_map, PAGE_SIZE);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA + 17, 1, IOMMU_READ | IOMMU_WRITE, &page), 1);
		f->external_refs++;
		KUNIT_EXPECT_EQ(test, page_to_phys(page),
				iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA));
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		/* The user VA goes away; retained pins are still the DMA buffer. */
		ctx->live = false;
		mmput(ctx->mm);
		KUNIT_ASSERT_EQ(test, atomic_read(&ctx->mm->mm_users), 0);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
			data, sizeof(data), false), 0);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x71, sizeof(data)), NULL);
		KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[1].type1), 0);
		list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
		KUNIT_EXPECT_EQ(test, f->domains[1].mapped, 2 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA),
				iommu_iova_to_phys(&f->domains[1].core, VFIO_TEST_IOVA));
		KUNIT_ASSERT_EQ(test, vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
		KUNIT_EXPECT_EQ(test, unmap.size, 2 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, f->notifications, 1U);
		KUNIT_EXPECT_GT(test, f->domains[0].syncs, 0U);
		KUNIT_EXPECT_GT(test, f->domains[1].syncs, 0U);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
		kunit_info(test, "type1 retained %uK user/%luK native pins after mm exit and replay",
			   1U << (shifts[i] - 10), PAGE_SIZE >> 10);
	}
}

static void vfio_fragments_lazy_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct file *file;
	struct vfio_dma *dma;
	struct page *page;
	unsigned char data[64];

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x52);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
		data, sizeof(data), false), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x52, sizeof(data)), NULL);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + 37, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	ctx->live = false;
	mmput(ctx->mm);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + PAGE_SIZE, 1, IOMMU_READ, &page), -EFAULT);
	for (unsigned int i = 0; i < 3; i++)
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 37, 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&dma->fragments->records));
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
}

static void vfio_fragments_map_failure_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct file *file;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x25);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
	f->domains[1].fail_after = SZ_4K;
	KUNIT_EXPECT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&f->iommu->dma_list));
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	KUNIT_EXPECT_EQ(test, f->domains[1].mapped, 0UL);
	f->domains[1].fail_after = 0;
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
}

static void vfio_fragments_fine_test(struct kunit *test)
{
	unsigned int shifts[] = { PAGE_SHIFT, 12, 14 };

	for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
		struct vfio_account_mm *ctx;
		struct vfio_fragment_fixture *f;
		struct vfio_dma *dma;
		struct file *file;
		struct page *page = NULL;
		unsigned char data[64];
		unsigned long length = PAGE_SIZE + SZ_4K;
		size_t copied = 0;

		if (i && shifts[i] == PAGE_SHIFT)
			continue;
		ctx = vfio_account_mm_new(test, shifts[i]);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x64);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		f = vfio_fragment_fixture_new(test);
		KUNIT_ASSERT_NOT_NULL(test, f);
		list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
		vfio_update_pgsize_bitmap(f->iommu);
		KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, SZ_4K, length), 0);
		dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
		KUNIT_EXPECT_EQ(test, f->domains[0].mapped, length);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		/* The physical base is offset inside a native page, not representable. */
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA, 1, IOMMU_READ, &page), -EINVAL);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA + PAGE_SIZE, 1, IOMMU_READ, &page), -EINVAL);
		KUNIT_ASSERT_EQ(test, vfio_dma_bitmap_alloc_all(f->iommu,
			vfio_dirty_pgsize(f->iommu)), 0);
		f->iommu->dirty_page_tracking = true;
		memset(data, 0x97, sizeof(data));
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw_chunk(f->iommu,
			VFIO_TEST_IOVA + SZ_4K - 17, data, sizeof(data), true, &copied), 0);
		KUNIT_EXPECT_EQ(test, copied, sizeof(data));
		KUNIT_EXPECT_EQ(test, bitmap_weight(dma->bitmap, length / SZ_4K), 2U);
		KUNIT_EXPECT_TRUE(test, test_bit(0, dma->bitmap));
		KUNIT_EXPECT_TRUE(test, test_bit(1, dma->bitmap));
		memset(data, 0, sizeof(data));
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu,
			VFIO_TEST_IOVA + SZ_4K - 17, data, sizeof(data), false), 0);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x97, sizeof(data)), NULL);
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu,
			VFIO_TEST_IOVA + length, data, sizeof(data), false), -EINVAL);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		kunit_info(test, "%uK userspace fine IOVA/physical offsets and dirty bytes checked",
			   1U << (shifts[i] - 10));
	}
}

static void vfio_fragments_replay_failure_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	struct file *file;
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x58);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	f->domains[0].fail_after = SZ_4K;
	KUNIT_EXPECT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), -ENOMEM);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_FALSE(test, dma->iommu_mapped);
	f->domains[0].fail_after = 0;
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), 0);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
	/* Detaching the last hardware domain keeps only the external page. */
	vfio_iommu_unmap_unpin_reaccount(f->iommu);
	list_del_init(&f->domains[0].type1.next);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
}

static void vfio_fragments_anon_owner_test(struct kunit *test)
{
	struct vfio_account_mm *old = vfio_account_mm_new(test, 12);
	struct vfio_account_mm *next = vfio_account_mm_new(test, PAGE_SHIFT);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	unsigned char *data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned long addr, copied, charge;
	unsigned char check[64];

	KUNIT_ASSERT_NOT_NULL(test, old);
	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memset(data, 0x49, PAGE_SIZE);
	kthread_use_mm(old->mm);
	addr = vm_mmap(NULL, VFIO_TEST_VA, PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	copied = addr == VFIO_TEST_VA ?
		copy_to_user((void __user *)addr, data, PAGE_SIZE) : PAGE_SIZE;
	kthread_unuse_mm(old->mm);
	KUNIT_ASSERT_EQ(test, addr, VFIO_TEST_VA);
	KUNIT_ASSERT_EQ(test, copied, 0UL);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, old, 0, PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_NOT_NULL(test, vfio_fragment_find(dma, 0)->pin.subpage);
	charge = dma->locked_vm;
	KUNIT_ASSERT_GT(test, charge, 0UL);
	old->live = false;
	mmput(old->mm);
	KUNIT_ASSERT_EQ(test, vfio_change_dma_mm(dma, dma->task, next->mm, true), 0);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(old->mm), 0UL);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), charge);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
		check, sizeof(check), false), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(check, 0x49, sizeof(check)), NULL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[1].type1), 0);
	list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(next->mm), 0UL);
	kunit_info(test, "anonymous 4K slot pins survive source exit and native-owner transfer");
}

static void vfio_fragments_domain_granule_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	struct file *file;
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, PAGE_SIZE, 0x73);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_EQ(test, vfio_dma_bitmap_alloc_all(f->iommu,
		vfio_dirty_pgsize(f->iommu)), 0);
	f->iommu->dirty_page_tracking = true;
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + 31, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, bitmap_weight(dma->bitmap, PAGE_SIZE / SZ_4K),
			(unsigned int)(PAGE_SIZE / SZ_4K));
	f->domains[0].core.pgsize_bitmap = SZ_16K;
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), 0);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, 1UL << __ffs(f->iommu->pgsize_bitmap), SZ_16K);
	KUNIT_EXPECT_EQ(test, vfio_dirty_pgsize(f->iommu), SZ_4K);
	vfio_iommu_unmap_unpin_reaccount(f->iommu);
	list_del_init(&f->domains[0].type1.next);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, 1UL << __ffs(f->iommu->pgsize_bitmap), SZ_4K);
	KUNIT_EXPECT_EQ(test, vfio_dirty_pgsize(f->iommu), SZ_4K);
	vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 31, 1);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
}

static struct kunit_case vfio_fragments_cases[] = {
	KUNIT_CASE(vfio_fragments_lifetime_test),
	KUNIT_CASE(vfio_fragments_lazy_test),
	KUNIT_CASE(vfio_fragments_map_failure_test),
	KUNIT_CASE(vfio_fragments_fine_test),
	KUNIT_CASE(vfio_fragments_replay_failure_test),
	KUNIT_CASE(vfio_fragments_anon_owner_test),
	KUNIT_CASE(vfio_fragments_domain_granule_test),
	{}
};

static struct kunit_suite vfio_fragments_suite = {
	.name = "vfio-type1-fragments",
	.test_cases = vfio_fragments_cases,
};

kunit_test_suite(vfio_fragments_suite);
