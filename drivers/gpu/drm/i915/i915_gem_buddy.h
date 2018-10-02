/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_BUDDY_H__
#define __I915_GEM_BUDDY_H__

#include <linux/bitops.h>

struct list_head;

struct i915_gem_buddy_block {
#define I915_GEM_BUDDY_HEADER_OFFSET (~(BIT(12) - 1))
#define I915_GEM_BUDDY_HEADER_ALLOCATED BIT(11)
#define I915_GEM_BUDDY_HEADER_ORDER (BIT(11) - 1)
	u64 header;

	struct i915_gem_buddy_block *left;
	struct i915_gem_buddy_block *right;
	struct i915_gem_buddy_block *parent;

	/* Always safe to reuse once the block is allocated */
	struct list_head link;
};

/*
 * Binary Buddy System
 *
 * XXX: Idealy we would just use the intrusive version of the buddy system i.e
 * the classical version, where we store the block info in the free blocks, but
 * if we are dealing with something like stolen memory, this would very quickly
 * turn into a dumbster fire with having to memremap/ioremap parts of stolen in
 * the allocator.
 */
struct i915_gem_buddy_mm {
	unsigned int max_order;
	/* Must be at least PAGE_SIZE */
	u64 min_size;

	struct kmem_cache *blocks;

	/* Maintain a free list for each order. */
	struct list_head *free_list;

	/*
	 * Maintain an explicit binary tree to track the allocation of the
	 * address space. This gives us a simple way of finding a buddy block
	 * and performing the potentially recursive merge step when freeing a
	 * block.  Nodes are either allocated or free, in which case they will
	 * also exist on the respective free list.
	 */
	struct i915_gem_buddy_block *root;
};

static inline u64
i915_gem_buddy_block_offset(struct i915_gem_buddy_block *block)
{
	return block->header & I915_GEM_BUDDY_HEADER_OFFSET;
}

static inline unsigned int
i915_gem_buddy_block_order(struct i915_gem_buddy_block *block)
{
	return block->header & I915_GEM_BUDDY_HEADER_ORDER;
}

static inline bool
i915_gem_buddy_block_allocated(struct i915_gem_buddy_block *block)
{
	return block->header & I915_GEM_BUDDY_HEADER_ALLOCATED;
}

static inline u64
i915_gem_buddy_block_size(struct i915_gem_buddy_mm *mm,
			  struct i915_gem_buddy_block *block)
{
	return BIT(i915_gem_buddy_block_order(block)) * mm->min_size;
}

static inline u64 i915_gem_buddy_size(struct i915_gem_buddy_mm *mm)
{
	return i915_gem_buddy_block_size(mm, mm->root);
}

int i915_gem_buddy_init(struct i915_gem_buddy_mm *mm,
			u64 size,
			u64 min_size);

void i915_gem_buddy_fini(struct i915_gem_buddy_mm *mm);

struct i915_gem_buddy_block *
i915_gem_buddy_alloc(struct i915_gem_buddy_mm *mm,
		     unsigned int order);

void i915_gem_buddy_free(struct i915_gem_buddy_mm *mm,
			 struct i915_gem_buddy_block *block);

#endif
