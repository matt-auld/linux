/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "../i915_selftest.h"

#include "mock_gem_device.h"
#include "mock_context.h"
#include "mock_drm.h"

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

	page_size = mem->mm.min_size;
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
	if (err == -ENOSPC) {
		if (page_num * page_size <= rem) {
			pr_err("%s failed, space still left in region\n",
				__func__);
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
	i915_gem_object_unpin_pages(obj);
	obj->mm.madv = I915_MADV_DONTNEED;
	list_move(&obj->region_link, &obj->memory_region->purgeable);
}

static int igt_mock_shrink(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	unsigned long n_objects;
	LIST_HEAD(objects);
	resource_size_t target;
	resource_size_t total;
	int err = 0;

	target = mem->mm.min_size;
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

		list_add(&obj->st_link, &objects);

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

int intel_memory_region_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_mock_fill),
		SUBTEST(igt_mock_shrink),
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
