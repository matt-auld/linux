// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/prime_numbers.h>

#include "../i915_selftest.h"

#include "mock_drm.h"
#include "mock_gem_device.h"
#include "mock_region.h"

#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"
#include "gem/i915_gem_object_blt.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_gt.h"
#include "selftests/igt_flush_test.h"

static void close_objects(struct list_head *objects)
{
	struct drm_i915_gem_object *obj, *on;

	list_for_each_entry_safe(obj, on, objects, st_link) {
		if (i915_gem_object_has_pinned_pages(obj))
			i915_gem_object_unpin_pages(obj);
		/* No polluting the memory region between tests */
		__i915_gem_object_put_pages(obj, I915_MM_NORMAL);
		i915_gem_object_put(obj);
		list_del(&obj->st_link);
	}
}

static int igt_mock_fill(void *arg)
{
	struct intel_memory_region *mem = arg;
	resource_size_t total = resource_size(&mem->region);
	resource_size_t page_size;
	resource_size_t rem;
	unsigned long max_pages;
	unsigned long page_num;
	LIST_HEAD(objects);
	int err = 0;

	page_size = mem->mm.chunk_size;
	max_pages = total / page_size;
	rem = total;

	for_each_prime_number_from(page_num, 1, max_pages) {
		resource_size_t size = page_num * page_size;
		struct drm_i915_gem_object *obj;

		obj = i915_gem_object_create_region(mem, size, 0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			break;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			break;
		}

		list_add(&obj->st_link, &objects);
		rem -= size;
	}

	if (err == -ENOMEM)
		err = 0;
	if (err == -ENXIO) {
		if (page_num * page_size <= rem) {
			pr_err("igt_mock_fill failed, space still left in region\n");
			err = -EINVAL;
		} else {
			err = 0;
		}
	}

	close_objects(&objects);

	return err;
}

static void igt_mark_evictable(struct drm_i915_gem_object *obj)
{
	if (i915_gem_object_has_pinned_pages(obj))
		i915_gem_object_unpin_pages(obj);
	obj->mm.madv = I915_MADV_DONTNEED;
	list_move(&obj->mm.region_link, &obj->mm.region->purgeable);
}

static int igt_frag_region(struct intel_memory_region *mem,
			   struct list_head *objects)
{
	struct drm_i915_gem_object *obj;
	unsigned long n_objects;
	resource_size_t target;
	resource_size_t total;
	int err = 0;

	target = mem->mm.chunk_size;
	total = resource_size(&mem->region);
	n_objects = total / target;

	while (n_objects--) {
		obj = i915_gem_object_create_region(mem,
						    target,
						    0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err_close_objects;
		}

		list_add(&obj->st_link, objects);

		err = i915_gem_object_pin_pages(obj);
		if (err)
			goto err_close_objects;

		/*
		 * Make half of the region evictable, though do so in a
		 * horribly fragmented fashion.
		 */
		if (n_objects % 2)
			igt_mark_evictable(obj);
	}

	return 0;

err_close_objects:
	close_objects(objects);
	return err;
}

static void igt_defrag_region(struct list_head *objects)
{
	struct drm_i915_gem_object *obj;

	list_for_each_entry(obj, objects, st_link) {
		if (obj->mm.madv == I915_MADV_WILLNEED)
			igt_mark_evictable(obj);
	}
}

static int igt_mock_shrink(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	LIST_HEAD(objects);
	resource_size_t target;
	resource_size_t total;
	int err;

	err = igt_frag_region(mem, &objects);
	if (err)
		return err;

	total = resource_size(&mem->region);
	target = mem->mm.chunk_size;

	while (target <= total / 2) {
		obj = i915_gem_object_create_region(mem, target, 0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err_close_objects;
		}

		list_add(&obj->st_link, &objects);

		/* Provoke the shrinker to start violently swinging its axe! */
		err = i915_gem_object_pin_pages(obj);
		if (err) {
			pr_err("failed to shrink for target=%pa", &target);
			goto err_close_objects;
		}

		/* Again, half of the region should remain evictable */
		igt_mark_evictable(obj);

		target <<= 1;
	}

err_close_objects:
	close_objects(&objects);

	if (err == -ENOMEM)
		err = 0;

	return err;
}

static int igt_mock_continuous(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	LIST_HEAD(objects);
	resource_size_t target;
	resource_size_t total;
	int err;

	err = igt_frag_region(mem, &objects);
	if (err)
		return err;

	total = resource_size(&mem->region);
	target = total / 2;

	/*
	 * Sanity check that we can allocate all of the available fragmented
	 * space.
	 */
	obj = i915_gem_object_create_region(mem, target, 0);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate available space\n");
		goto err_close_objects;
	}

	igt_mark_evictable(obj);

	/* Try the smallest possible size -- should succeed */
	obj = i915_gem_object_create_region(mem, mem->mm.chunk_size,
					    I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate smallest possible size\n");
		goto err_close_objects;
	}

	igt_mark_evictable(obj);

	if (obj->mm.pages->nents != 1) {
		pr_err("[1]object spans multiple sg entries\n");
		err = -EINVAL;
		goto err_close_objects;
	}

	/*
	 * Even though there is enough free space for the allocation, we
	 * shouldn't be able to allocate it, given that it is fragmented, and
	 * non-continuous.
	 */
	obj = i915_gem_object_create_region(mem, target, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (!err) {
		pr_err("expected allocation to fail\n");
		err = -EINVAL;
		goto err_close_objects;
	}

	igt_defrag_region(&objects);

	/* Should now succeed */
	obj = i915_gem_object_create_region(mem, target, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate from defraged area\n");
		goto err_close_objects;
	}

	if (obj->mm.pages->nents != 1) {
		pr_err("object spans multiple sg entries\n");
		err = -EINVAL;
	}

err_close_objects:
	close_objects(&objects);

	return err;
}

static int igt_mock_volatile(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	int err;

	obj = i915_gem_object_create_region(mem, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto err_put;

	i915_gem_object_unpin_pages(obj);

	err = intel_memory_region_evict(mem, PAGE_SIZE, 0);
	if (err != -ENOSPC) {
		pr_err("shrink memory region\n");
		goto err_put;
	}

	i915_gem_object_put(obj);

	obj = i915_gem_object_create_region(mem, PAGE_SIZE, I915_BO_ALLOC_VOLATILE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	if (!(obj->flags & I915_BO_ALLOC_VOLATILE)) {
		pr_err("missing flags\n");
		goto err_put;
	}

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto err_put;

	i915_gem_object_unpin_pages(obj);

	err = intel_memory_region_evict(mem, PAGE_SIZE, 0);
	if (err) {
		pr_err("failed to shrink memory\n");
		goto err_put;
	}

	if (i915_gem_object_has_pages(obj)) {
		pr_err("object pages not discarded\n");
		err = -EINVAL;
	}

err_put:
	i915_gem_object_put(obj);
	return err;
}

static int igt_lmem_create(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj;
	int err = 0;

	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto out_put;

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_lmem_write_cpu(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_context *ce = i915->engine[BCS0]->kernel_context;
	struct drm_i915_gem_object *obj;
	struct rnd_state prng;
	u32 *vaddr;
	u32 dword;
	u32 val;
	u32 sz;
	int err;

	if (!HAS_ENGINE(i915, BCS0))
		return 0;

	sz = round_up(prandom_u32_state(&prng) % SZ_32M, PAGE_SIZE);

	obj = i915_gem_object_create_lmem(i915, sz, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vaddr = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(vaddr)) {
		pr_err("Failed to iomap lmembar; err=%d\n", (int)PTR_ERR(vaddr));
		err = PTR_ERR(vaddr);
		goto out_put;
	}

	val = prandom_u32_state(&prng);

	/* Write from gpu and then read from cpu */
	err = i915_gem_object_fill_blt(obj, ce, val);
	if (err)
		goto out_unpin;

	i915_gem_object_lock(obj);
	err = i915_gem_object_set_to_wc_domain(obj, true);
	i915_gem_object_unlock(obj);
	if (err)
		goto out_unpin;

	for (dword = 0; dword < sz / sizeof(u32); ++dword) {
		if (vaddr[dword] != val) {
			pr_err("vaddr[%u]=%u, val=%u\n", dword, vaddr[dword],
			        val);
			err = -EINVAL;
			break;
		}
	}

	/* Write from the cpu and read again from the cpu */
	memset32(vaddr, val ^ 0xdeadbeaf, sz / sizeof(u32));

	for (dword = 0; dword < sz / sizeof(u32); ++dword) {
		if (vaddr[dword] != (val ^ 0xdeadbeaf)) {
			pr_err("vaddr[%u]=%u, val=%u\n", dword, vaddr[dword],
			        val ^ 0xdeadbeaf);
			err = -EINVAL;
			break;
		}
	}

out_unpin:
	i915_gem_object_unpin_map(obj);
out_put:
	i915_gem_object_put(obj);

	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		err = -EIO;

	return err;
}

int intel_memory_region_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_mock_fill),
		SUBTEST(igt_mock_shrink),
		SUBTEST(igt_mock_continuous),
		SUBTEST(igt_mock_volatile),
	};
	struct intel_memory_region *mem;
	struct drm_i915_private *i915;
	int err;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	mem = mock_region_create(i915, 0, SZ_2G,
				 I915_GTT_PAGE_SIZE_4K, 0);
	if (IS_ERR(mem)) {
		pr_err("failed to create memory region\n");
		err = PTR_ERR(mem);
		goto out_unref;
	}

	mutex_lock(&i915->drm.struct_mutex);
	err = i915_subtests(tests, mem);
	mutex_unlock(&i915->drm.struct_mutex);

	i915_gem_drain_freed_objects(i915);
	intel_memory_region_destroy(mem);

out_unref:
	drm_dev_put(&i915->drm);

	return err;
}

int intel_memory_region_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_create),
		SUBTEST(igt_lmem_write_cpu),
	};
	int err;

	if (!HAS_LMEM(i915)) {
		pr_info("device lacks LMEM support, skipping\n");
		return 0;
	}

	if (intel_gt_is_wedged(&i915->gt))
		return 0;

	mutex_lock(&i915->drm.struct_mutex);
	err = i915_subtests(tests, i915);
	mutex_unlock(&i915->drm.struct_mutex);

	return err;
}
