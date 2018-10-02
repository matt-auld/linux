/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/list.h>

#include "i915_gem_buddy.h"
#include "i915_gem.h"

int i915_gem_buddy_init(struct i915_gem_buddy_mm *mm, u64 size, u64 min_size)
{
	unsigned int i;

	/*
	 * XXX: if not a power of 2, maybe split into power of 2 blocks,
	 * effectively having multiple roots, similar to if we had a global
	 * MAX_ORDER.
	 */
	size = rounddown_pow_of_two(size);
	min_size = roundup_pow_of_two(min_size);

	if (size < min_size)
		return -EINVAL;

	if (min_size < PAGE_SIZE)
		return -EINVAL;

	mm->max_order = ilog2(size) - ilog2(min_size);
	mm->min_size = min_size;

	mm->free_list = kmalloc_array(mm->max_order + 1,
				      sizeof(struct list_head),
				      GFP_KERNEL);
	if (!mm->free_list)
		return -ENOMEM;

	for (i = 0; i <= mm->max_order; ++i)
		INIT_LIST_HEAD(&mm->free_list[i]);

	mm->blocks = KMEM_CACHE(i915_gem_buddy_block, SLAB_HWCACHE_ALIGN);
	if (!mm->blocks)
		goto out_free_list;

	mm->root = kmem_cache_zalloc(mm->blocks, GFP_KERNEL);
	if (!mm->root)
		goto out_free_blocks;

	mm->root->header = mm->max_order;

	list_add(&mm->root->link, &mm->free_list[mm->max_order]);

	return 0;

out_free_blocks:
	kmem_cache_destroy(mm->blocks);
out_free_list:
	kfree(mm->free_list);

	return -ENOMEM;
}

void i915_gem_buddy_fini(struct i915_gem_buddy_mm *mm)
{
	if (WARN_ON(i915_gem_buddy_block_allocated(mm->root)))
		return;

	kfree(mm->free_list);
	kmem_cache_free(mm->blocks, mm->root);
	kmem_cache_destroy(mm->blocks);
}

/*
 * The 'order' here means:
 *
 * 0 = 2^0 * mm->min_size
 * 1 = 2^1 * mm->min_size
 * 2 = 2^2 * mm->min_size
 * ...
 */
struct i915_gem_buddy_block *
i915_gem_buddy_alloc(struct i915_gem_buddy_mm *mm, unsigned int order)
{
	struct i915_gem_buddy_block *block = NULL;
	struct i915_gem_buddy_block *root;
	unsigned int i;

	for (i = order; i <= mm->max_order; ++i) {
		block = list_first_entry_or_null(&mm->free_list[i],
						 struct i915_gem_buddy_block,
						 link);
		if (block)
			break;
	}

	if (!block)
		return ERR_PTR(-ENOSPC);

	GEM_BUG_ON(i915_gem_buddy_block_allocated(block));

	root = block;

	while (i != order) {
		u64 offset = i915_gem_buddy_block_offset(block);

		block->left = kmem_cache_zalloc(mm->blocks, GFP_KERNEL);
		if (!block->left)
			goto out_free_blocks;

		block->left->header = offset;
		block->left->header |= I915_GEM_BUDDY_HEADER_ALLOCATED;
		block->left->header |= i - 1;
		block->left->parent = block;

		INIT_LIST_HEAD(&block->left->link);

		block->right = kmem_cache_zalloc(mm->blocks, GFP_KERNEL);
		if (!block->right) {
			kmem_cache_free(mm->blocks, block->left);
			goto out_free_blocks;
		}

		block->right->header = offset + (BIT(i - 1) * mm->min_size);
		block->right->header |= i - 1;
		block->right->parent = block;

		list_add(&block->right->link, &mm->free_list[i - 1]);

		block = block->left;
		i--;
	}

	root->header |= I915_GEM_BUDDY_HEADER_ALLOCATED;
	list_del(&root->link);

	return block;

out_free_blocks:
	while (block != root) {
		if (block->right)
			list_del(&block->right->link);

		kmem_cache_free(mm->blocks, block->left);
		kmem_cache_free(mm->blocks, block->right);

		block = block->parent;
	}

	return ERR_PTR(-ENOMEM);
}

void i915_gem_buddy_free(struct i915_gem_buddy_mm *mm,
			 struct i915_gem_buddy_block *block)
{
	GEM_BUG_ON(!i915_gem_buddy_block_allocated(block));

	while (block->parent) {
		struct i915_gem_buddy_block *buddy;
		struct i915_gem_buddy_block *parent;

		parent = block->parent;

		if (parent->left == block)
			buddy = parent->right;
		else
			buddy = parent->left;

		if (i915_gem_buddy_block_allocated(buddy))
			break;

		list_del(&buddy->link);

		kmem_cache_free(mm->blocks, block);
		kmem_cache_free(mm->blocks, buddy);

		block = parent;
	}

	block->header &= ~I915_GEM_BUDDY_HEADER_ALLOCATED;
	list_add(&block->link,
		 &mm->free_list[i915_gem_buddy_block_order(block)]);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_buddy.c"
#endif
