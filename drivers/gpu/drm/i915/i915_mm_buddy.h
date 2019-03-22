/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_MM_BUDDY_H__
#define __I915_MM_BUDDY_H__

#include <drm/drm_mm.h>

struct list_head;

/*
 * Binary buddy allocator as a simple shim-on-drm_mm. The main advantage of
 * doing so is that we get the drm_mm scan roster for free, which might come in
 * handy for eviction, if say we want to allocate an object as contiguous,
 * either as a single power-of-two block, or a range of blocks. The idea at
 * least works in my head...
 */
struct i915_mm_buddy {
	struct drm_mm drm_mm;

	unsigned int max_order;
	u64 min_size;
	u64 size;

	struct kmem_cache *blocks;

	/*
	 * Manage the address space as in-order list of power-of-two blocks.
	 * Blocks can be free or allocated, if free then they will also exist on
	 * the free list at the respective order. Finding the buddy and
	 * performing the potentially recursive merge step is as simple as
	 * looking left or right before crossing. Also allocating a range is
	 * just a simple linear search, if we need to pre-allocate or reserve
	 * portions of the address space.
	 */
	struct list_head global_list;

	/*
	 * Maintain a free list for each order. At each level blocks are ordered
	 * by most-recently-freed. Another idea could be to use an r-b tree at
	 * each level instead, where we order by offset, not sure if that's
	 * better though.
	 */
	struct list_head *free_list;
};

struct i915_mm_buddy_block {
	unsigned int order;

	struct list_head tmp_link;
	struct list_head free_link;
	struct list_head global_link;

	struct drm_mm_node node;
};

static inline bool i915_mm_buddy_block_is_free(struct i915_mm_buddy_block *b)
{
	return !b->node.allocated;
}

static inline u64 i915_mm_buddy_block_offset(struct i915_mm_buddy_block *b)
{
	return b->node.start;
}

static inline u64 i915_mm_buddy_block_size(struct i915_mm_buddy_block *b)
{
	return b->node.size;
}

static inline unsigned int
i915_mm_buddy_block_order(struct i915_mm_buddy_block *b)
{
	return b->order;
}

int i915_mm_buddy_init(struct i915_mm_buddy *mm,
			u64 size,
			u64 min_size);

void i915_mm_buddy_fini(struct i915_mm_buddy *mm);

struct i915_mm_buddy_block *i915_mm_buddy_alloc(struct i915_mm_buddy *mm,
						unsigned int order);

int i915_mm_buddy_alloc_range(struct i915_mm_buddy *mm,
			      struct list_head *blocks,
			      u64 start, u64 size);

void i915_mm_buddy_free(struct i915_mm_buddy *mm,
			struct i915_mm_buddy_block *block);

void i915_mm_buddy_free_list(struct i915_mm_buddy *mm,
			     struct list_head *objects);

#endif
