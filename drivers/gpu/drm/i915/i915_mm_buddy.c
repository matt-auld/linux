// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/list.h>

#include "i915_mm_buddy.h"

#include "i915_gem.h"
#include "i915_utils.h"

static void mark_allocated(struct i915_mm_buddy *mm,
			   struct i915_mm_buddy_block *b)
{
	int err;

	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b));

	list_del_init(&b->free_link);
	err = drm_mm_reserve_node(&mm->drm_mm, &b->node);
	GEM_BUG_ON(err);
}

static void mark_free(struct i915_mm_buddy *mm,
		      struct i915_mm_buddy_block *b)
{
	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b));

	list_add(&b->free_link, &mm->free_list[b->order]);
}

int i915_mm_buddy_init(struct i915_mm_buddy *mm, u64 size, u64 min_size)
{
	unsigned int i;
	u64 start;

	if (size < min_size)
		return -EINVAL;

	if (min_size < PAGE_SIZE)
		return -EINVAL;

	if (!is_power_of_2(min_size))
		return -EINVAL;

	size = round_down(size, min_size);

	mm->size = size;
	mm->min_size = min_size;
	mm->max_order = ilog2(rounddown_pow_of_two(size)) - ilog2(min_size);

	mm->free_list = kmalloc_array(mm->max_order + 1,
				      sizeof(struct list_head),
				      GFP_KERNEL);
	if (!mm->free_list)
		return -ENOMEM;

	for (i = 0; i <= mm->max_order; ++i)
		INIT_LIST_HEAD(&mm->free_list[i]);

	mm->blocks = KMEM_CACHE(i915_mm_buddy_block, SLAB_HWCACHE_ALIGN);
	if (!mm->blocks)
		goto out_free_list;

	INIT_LIST_HEAD(&mm->global_list);
	start = 0;

	/*
	 * Split into power-of-two blocks, in case we are given a size that is
	 * not itself a power-of-two.
	 */
	do {
		struct i915_mm_buddy_block *root;
		unsigned int order;
		u64 root_size;

		root = kmem_cache_zalloc(mm->blocks, GFP_KERNEL);
		if (!root)
			goto out_free_roots;

		root_size = rounddown_pow_of_two(size);
		order = ilog2(root_size) - ilog2(min_size);

		GEM_BUG_ON(order > mm->max_order);
		GEM_BUG_ON(root_size < min_size);

		root->node.start = start;
		root->node.size = root_size;
		root->order = order;

		list_add_tail(&root->global_link, &mm->global_list);
		INIT_LIST_HEAD(&root->tmp_link);

		mark_free(mm, root);

		start += root_size;
		size -= root_size;
	} while (size);

	drm_mm_init(&mm->drm_mm, 0, mm->size);
	return 0;

out_free_roots:
	i915_mm_buddy_free_list(mm, &mm->global_list);
	kmem_cache_destroy(mm->blocks);
out_free_list:
	kfree(mm->free_list);
	return -ENOMEM;
}

void i915_mm_buddy_fini(struct i915_mm_buddy *mm)
{
	struct i915_mm_buddy_block *root;
	struct i915_mm_buddy_block *on;
	int err = 0;

	list_for_each_entry_safe(root, on, &mm->global_list, global_link) {
		if (!i915_mm_buddy_block_is_free(root)) {
			err = -EBUSY;
			continue;
		}

		kmem_cache_free(mm->blocks, root);
	}

	/*
	 * XXX: Rather leak memory for now, than hit a potential user-after-free
	 */
	if (WARN_ON(err))
		return;

	kfree(mm->free_list);
	kmem_cache_destroy(mm->blocks);
	drm_mm_takedown(&mm->drm_mm);
}

void i915_mm_buddy_free_list(struct i915_mm_buddy *mm,
			     struct list_head *objects)
{
	struct i915_mm_buddy_block *block, *on;

	list_for_each_entry_safe(block, on, objects, free_link)
		i915_mm_buddy_free(mm, block);
}

static bool is_buddy(struct i915_mm_buddy_block *b1,
		     struct i915_mm_buddy_block *b2)
{
	if (b1->order != b2->order)
		return false;

	if (b1->node.start != (b2->node.start ^ b2->node.size))
		return false;

	return true;
}

/* Merge b2 into b1 */
static struct i915_mm_buddy_block *
merge_blocks(struct i915_mm_buddy *mm,
	     struct i915_mm_buddy_block *b1,
	     struct i915_mm_buddy_block *b2)
{
	GEM_BUG_ON(b1->node.size != b2->node.size);
	GEM_BUG_ON(b1->node.start >= b2->node.start);
	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b1));
	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b2));

	list_del_init(&b2->global_link);
	list_del_init(&b2->free_link);

	drm_mm_remove_node(&b1->node);
	drm_mm_remove_node(&b2->node);

	kmem_cache_free(mm->blocks, b2);

	b1->node.size <<= 1;
	b1->order++;

	return b1;
}

static struct i915_mm_buddy_block *merge_block(struct i915_mm_buddy *mm,
					       struct i915_mm_buddy_block *b)
{
	struct i915_mm_buddy_block *buddy;

	buddy = list_prev_entry(b, global_link);
	if (buddy && is_buddy(b, buddy) &&
	    i915_mm_buddy_block_is_free(buddy)) {
		return merge_blocks(mm, buddy, b);
	}

	buddy = list_next_entry(b, global_link);
	if (buddy && is_buddy(b, buddy) &&
	    i915_mm_buddy_block_is_free(buddy)) {
		return merge_blocks(mm, b, buddy);
	}

	return b;
}

static void __i915_mm_buddy_free(struct i915_mm_buddy *mm,
				 struct i915_mm_buddy_block *b)
{
	list_del_init(&b->free_link); /* We have ownership now */

	do {
		struct i915_mm_buddy_block *merged;

		merged = merge_block(mm, b);
		if (merged == b)
			break;

		b = merged;
	} while (1);

	mark_free(mm, b);
}

void i915_mm_buddy_free(struct i915_mm_buddy *mm,
			struct i915_mm_buddy_block *b)
{
	GEM_BUG_ON(i915_mm_buddy_block_is_free(b));

	drm_mm_remove_node(&b->node);
	__i915_mm_buddy_free(mm, b);
}

static struct i915_mm_buddy_block *split_block(struct i915_mm_buddy *mm,
					       struct i915_mm_buddy_block *b)
{
	struct i915_mm_buddy_block *buddy;

	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b));
	GEM_BUG_ON(!b->order);

	buddy = kmem_cache_zalloc(mm->blocks, GFP_KERNEL);
	if (!buddy)
		return ERR_PTR(-ENOMEM);

	b->node.size >>= 1;
	b->order--;

	buddy->node.start = b->node.start ^ b->node.size;
	buddy->node.size = b->node.size;
	buddy->order = b->order;

	INIT_LIST_HEAD(&buddy->tmp_link);
	list_add_tail(&buddy->global_link, &b->global_link);

	mark_free(mm, buddy);

	return buddy;
}
/*
 * Allocate power-of-two block. The order value here translates to:
 *
 *   0 = 2^0 * mm->min_size
 *   1 = 2^1 * mm->min_size
 *   2 = 2^2 * mm->min_size
 *   ...
 */
struct i915_mm_buddy_block *
i915_mm_buddy_alloc(struct i915_mm_buddy *mm, unsigned int order)
{
	struct i915_mm_buddy_block *b = NULL;
	unsigned int i;
	int err;

	for (i = order; i <= mm->max_order; ++i) {
		b = list_first_entry_or_null(&mm->free_list[i],
					     struct i915_mm_buddy_block,
					     free_link);
		if (b)
			break;
	}

	if (!b)
		return ERR_PTR(-ENOSPC);

	GEM_BUG_ON(!i915_mm_buddy_block_is_free(b));

	while (i != order) {
		struct i915_mm_buddy_block *buddy;

		buddy = split_block(mm, b);
		if (IS_ERR(buddy)) {
			err = PTR_ERR(buddy);
			goto out_free;
		}

		i--;
	}

	mark_allocated(mm, b);
	return b;

out_free:
	__i915_mm_buddy_free(mm, b);
	return ERR_PTR(err);
}

static inline bool overlaps(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= e2 && e1 >= s2;
}

static inline bool contains(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= s2 && e1 >= e2;
}

/*
 * Allocate range. Note that it's safe to chain together multiple alloc_ranges
 * with the same blocks list.
 *
 * Intended for pre-allocating portions of the address space, for example to
 * reserve a block for the initial framebuffer or similar, hence the expectation
 * here is that i915_mm_buddy_alloc() is still the main vehicle for
 * allocations, so if that's not the case then the drm_mm range allocator is
 * probably a much better fit, and so you should probably go use that instead.
 */
int i915_mm_buddy_alloc_range(struct i915_mm_buddy *mm,
			      struct list_head *blocks,
			      u64 start, u64 size)
{
	struct i915_mm_buddy_block *buddy;
	struct i915_mm_buddy_block *b;
	LIST_HEAD(allocated);
	LIST_HEAD(found);
	u64 end;
	u64 bs;
	u64 be;
	int err;

	if (size < mm->min_size)
		return -EINVAL;

	if (!IS_ALIGNED(start, mm->min_size))
		return -EINVAL;

	if (!size || !IS_ALIGNED(size, mm->min_size))
		return -EINVAL;

	if (range_overflows(start, size, mm->size))
		return -EINVAL;

	end = start + size - 1;
	err = 0;

	//node = __drm_mm_interval_first(&mm->mm_drm, start, end);

	/*
	 * XXX: surely we can do better. we just need to find the first
	 * overlapping block.
	 */
	list_for_each_entry(b, &mm->global_list, global_link) {
		bs = b->node.start;
		be = bs + b->node.size - 1;

		if (overlaps(start, end, bs, be)) {
			if (!i915_mm_buddy_block_is_free(b)) {
				err = -ENOSPC;
				break;
			}

			list_add_tail(&b->tmp_link, &found);
		}
	}

	do {
		b = list_first_entry_or_null(&found,
					     struct i915_mm_buddy_block,
					     tmp_link);
		if (!b)
			break;

		GEM_BUG_ON(!i915_mm_buddy_block_is_free(b));

		list_del_init(&b->tmp_link);

		if (err)
			continue;

		bs = b->node.start;
		be = bs + b->node.size - 1;

		if (!overlaps(start, end, bs, be))
			continue;

		if (contains(start, end, bs, be)) {
			mark_allocated(mm, b);
			list_add_tail(&b->free_link, &allocated);
			continue;
		}

		buddy = split_block(mm, b);
		if (IS_ERR(buddy)) {
			merge_block(mm, b);
			continue;
		}

		list_add(&buddy->tmp_link, &found);
		list_add(&b->tmp_link, &found);
	} while (1);

	if (err) {
		i915_mm_buddy_free_list(mm, &allocated);
		return err;
	}

	list_splice_tail(&allocated, blocks);
	return 0;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_mm_buddy.c"
#endif
