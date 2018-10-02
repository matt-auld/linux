/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "../i915_selftest.h"

#define SZ_64G (1ULL << 36)

static int igt_buddy_init(void *arg)
{
	struct i915_gem_buddy_mm mm;
	u64 size;
	int err = 0;

	for (size = PAGE_SIZE; size <= SZ_64G; size <<= 1) {
		struct i915_gem_buddy_block *block;

		err = i915_gem_buddy_init(&mm, size, PAGE_SIZE);
		if (err) {
			pr_err("buddy_init with size=%llx\n failed(%d)\n",
			       size, err);
			return err;
		}

		block = i915_gem_buddy_alloc(&mm, mm.max_order);
		if (IS_ERR(block)) {
			err = PTR_ERR(block);
			pr_err("buddy_alloc with size=%llx\n failed(%d)\n",
			       size, err);
			goto out_buddy_fini;
		}

		if (i915_gem_buddy_block_order(block) != mm.max_order) {
			pr_err("buddy_alloc size mismatch\n");
			err = -EINVAL;
		}

		if (i915_gem_buddy_block_offset(block) != 0) {
			pr_err("buddy_alloc offset mismatch\n");
			err = -EINVAL;
		}

		if (!list_empty(&mm.free_list[mm.max_order])) {
			pr_err("buddy_alloc state mismatch, block is on free list\n");
			err = -EINVAL;
		}

		if (block != mm.root) {
			pr_err("buddy_alloc state mismatch, block != root\n");
			err = -EINVAL;
		}

		if (block->left || block->right) {
			pr_err("buddy_alloc state mismatch, block is not leaf\n");
			err = -EINVAL;
		}

		i915_gem_buddy_free(&mm, block);

		if (list_empty(&mm.free_list[mm.max_order])) {
			pr_err("buddy_free state mismatch, block not on free list\n");
			err = -EINVAL;
		}

		i915_gem_buddy_fini(&mm);

		if (err)
			return err;
	}

	return 0;

out_buddy_fini:
	i915_gem_buddy_fini(&mm);

	return err;
}

static int igt_buddy_alloc(void *arg)
{
	struct i915_gem_buddy_mm mm;
	u64 size;
	int order;
	int err;

	err = i915_gem_buddy_init(&mm, SZ_64G, PAGE_SIZE);
	if (err) {
		pr_err("buddy_init with size=%llx\n failed(%d)\n", SZ_64G, err);
		return err;
	}

	for (order = mm.max_order; order >= 0; order--) {
		struct i915_gem_buddy_block *block, *on;
		struct list_head blocks;
		u64 block_size;
		u64 offset;
		u64 prev_offset;
		u64 n_blocks;

		size = i915_gem_buddy_size(&mm);
		if (size != SZ_64G) {
			pr_err("buddy_size mismatch\n");
			err = -EINVAL;
			break;
		}

		if (list_empty(&mm.free_list[mm.max_order])) {
			pr_err("root not on the free list\n");
			err = -EINVAL;
			break;
		}

		pr_info("filling address space(%llx) with order=%d\n",
			i915_gem_buddy_size(&mm), order);

		prev_offset = 0;
		n_blocks = div64_u64(size, BIT(order) * mm.min_size);
		INIT_LIST_HEAD(&blocks);

		while (n_blocks--) {
			block = i915_gem_buddy_alloc(&mm, order);
			if (IS_ERR(block)) {
				err = PTR_ERR(block);
				if (err == -ENOMEM) {
					pr_info("buddy_alloc hit -ENOMEM with order=%d\n",
						order);
				} else {
					pr_err("buddy_alloc with order=%d failed(%d)\n",
					       order, err);
				}

				break;
			}

			list_add(&block->link, &blocks);

			if (i915_gem_buddy_block_order(block) != order) {
				pr_err("buddy_alloc order mismatch\n");
				err = -EINVAL;
				break;
			}

			block_size = i915_gem_buddy_block_size(&mm, block);
			offset = i915_gem_buddy_block_offset(block);

			if (!IS_ALIGNED(offset, block_size)) {
				pr_err("buddy_alloc offset misaligned, offset=%llx, block_size=%llu\n",
				       offset, block_size);
				err = -EINVAL;
				break;
			}

			if (offset && offset != (prev_offset + block_size)) {
				pr_err("buddy_alloc offset mismatch, prev_offset=%llx, offset=%llx\n",
				       prev_offset, offset);
				err = -EINVAL;
				break;
			}

			prev_offset = offset;
		}

		list_for_each_entry_safe(block, on, &blocks, link) {
			list_del(&block->link);
			i915_gem_buddy_free(&mm, block);
		}

		if (err)
			break;
	}

	i915_gem_buddy_fini(&mm);

	if (err == -ENOMEM)
		err = 0;

	return err;
}

int i915_gem_buddy_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_buddy_init),
		SUBTEST(igt_buddy_alloc),
	};

	return i915_subtests(tests, NULL);
}

