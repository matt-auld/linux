// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915_ttm_buddy_manager.h"

#include "i915_buddy.h"
#include "i915_gem.h"

#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/slab.h>

struct i915_ttm_buddy_manager {
	struct ttm_resource_manager manager;
	struct i915_buddy_mm mm;
	struct list_head reserved;
	struct mutex lock;
};

static inline struct i915_ttm_buddy_manager *
to_buddy_manager(struct ttm_resource_manager *man)
{
	return container_of(man, struct i915_ttm_buddy_manager, manager);
}

static int i915_ttm_buddy_man_alloc(struct ttm_resource_manager *man,
				    struct ttm_buffer_object *bo,
				    const struct ttm_place *place,
				    struct ttm_resource *mem)
{
	struct i915_ttm_buddy_manager *bman = to_buddy_manager(man);
	struct i915_buddy_mm *mm = &bman->mm;
	unsigned long n_pages;
	unsigned int min_order;
	LIST_HEAD(blocks);
	u64 size;

	GEM_BUG_ON(place->fpfn || place->lpfn);
	GEM_BUG_ON(bo->page_alignment < mm->chunk_size);

	min_order = ilog2(bo->page_alignment) - ilog2(mm->chunk_size);
	if (place->flags & TTM_PL_FLAG_CONTIGUOUS) {
		size = roundup_pow_of_two(size);
		min_order = ilog2(size) - ilog2(mm->chunk_size);
	}

	if (size > mm->size)
		return -E2BIG;

	size = mem->num_pages << PAGE_SHIFT;
	n_pages = size >> ilog2(mm->chunk_size);

	mutex_lock(&bman->lock);

	do {
		struct i915_buddy_block *block;
		unsigned int order;

		order = fls(n_pages) - 1;
		GEM_BUG_ON(order > mm->max_order);
		GEM_BUG_ON(order < min_order);

		do {
			block = i915_buddy_alloc(mm, order);
			if (!IS_ERR(block))
				break;

			if (order-- == min_order)
				goto err_free_blocks;
		} while (1);

		n_pages -= BIT(order);

		block->private = mm; /* XXX */
		list_add_tail(&block->link, &blocks);

		if (!n_pages)
			break;
	} while (1);

	mutex_unlock(&bman->lock);

	mem->mm_node = &blocks;
	mem->start = 0; /* XXX: do we need this? */
	return 0;

err_free_blocks:
	i915_buddy_free_list(mm, &blocks);
	mutex_unlock(&bman->lock);
	return -ENXIO;
}

static void i915_ttm_buddy_man_free(struct ttm_resource_manager *man,
				    struct ttm_resource *mem)
{
	struct i915_ttm_buddy_manager *bman = to_buddy_manager(man);

	if (mem->mm_node) {
		struct list_head *blocks = mem->mm_node;

		mutex_lock(&bman->lock);
		i915_buddy_free_list(&bman->mm, blocks);
		mutex_unlock(&bman->lock);

		mem->mm_node = NULL;
	}
}

static const struct ttm_resource_manager_func i915_ttm_buddy_manager_func = {
	.alloc = i915_ttm_buddy_man_alloc,
	.free = i915_ttm_buddy_man_free,
};

struct ttm_resource_manager *
i915_ttm_buddy_man_init_nodev(u64 p_size, u64 chunk_size, bool use_tt)
{
	struct ttm_resource_manager *man;
	struct i915_ttm_buddy_manager *bman;
	int err;

	bman = kzalloc(sizeof(*bman), GFP_KERNEL);
	if (!bman)
		return ERR_PTR(-ENOMEM);

	err = i915_buddy_init(&bman->mm, p_size, chunk_size);
	if (err)
		goto err_free_bman;

	mutex_init(&bman->lock);
	INIT_LIST_HEAD(&bman->reserved);

	man = &bman->manager;
	man->use_tt = use_tt;
	man->func = &i915_ttm_buddy_manager_func;
	ttm_resource_manager_init(man, p_size);
	return man;

err_free_bman:
	kfree(bman);
	return ERR_PTR(err);
}

int i915_ttm_buddy_man_init(struct ttm_device *bdev,
			    unsigned type, bool use_tt,
			    unsigned long p_size,
			    u64 chunk_size)
{
	struct ttm_resource_manager *man;

	man = i915_ttm_buddy_man_init_nodev(p_size, chunk_size, use_tt);
	if (IS_ERR(man))
		return PTR_ERR(man);

	ttm_resource_manager_set_used(man, true);
	ttm_set_driver_manager(bdev, type, man);
	return 0;
}

void i915_ttm_buddy_man_fini_nodev(struct ttm_resource_manager *man)
{
	struct i915_ttm_buddy_manager *bman = to_buddy_manager(man);
	struct i915_buddy_mm *mm = &bman->mm;

	mutex_lock(&bman->lock);
	i915_buddy_free_list(mm, &bman->reserved);
	i915_buddy_fini(mm);
	mutex_unlock(&bman->lock);

	ttm_resource_manager_cleanup(man);
	kfree(bman);
}

int i915_ttm_buddy_man_fini(struct ttm_device *bdev, unsigned type)
{
	struct ttm_resource_manager *man = ttm_manager_type(bdev, type);
	int ret;

	ttm_resource_manager_set_used(man, false);

	ret = ttm_resource_manager_evict_all(bdev, man);
	if (ret)
		return ret;

	ttm_set_driver_manager(bdev, type, NULL);
	i915_ttm_buddy_man_fini_nodev(man);
	return 0;
}

int i915_ttm_buddy_man_reserve_nodev(struct ttm_resource_manager *man,
				     u64 start, u64 size)
{
	struct i915_ttm_buddy_manager *bman = to_buddy_manager(man);
	struct i915_buddy_mm *mm = &bman->mm;

	return i915_buddy_alloc_range(mm, &bman->reserved, start, size);
}

int i915_ttm_buddy_man_reserve(struct ttm_device *bdev, unsigned type,
			       u64 start, u64 size)
{
	struct ttm_resource_manager *man = ttm_manager_type(bdev, type);

	return i915_ttm_buddy_man_reserve_nodev(man, start, size);
}

