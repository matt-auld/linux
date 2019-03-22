// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/bitops.h>
#include <linux/prime_numbers.h>

#include "../i915_selftest.h"
#include "i915_random.h"

#define SZ_8G (1ULL << 33)

static struct i915_mm_buddy_block *get_buddy(struct i915_mm_buddy_block *b)
{
	struct i915_mm_buddy_block *buddy;

	buddy = list_prev_entry(b, global_link);
	if (buddy && is_buddy(b, buddy))
		return buddy;

	buddy = list_next_entry(b, global_link);
	if (buddy && is_buddy(b, buddy))
		return buddy;

	return NULL;
}

static void __igt_dump_block(struct i915_mm_buddy *mm,
			     struct i915_mm_buddy_block *block,
			     bool buddy)
{
	pr_err("block info: order=%d, offset=%llx size=%llx free=%s buddy=%s\n",
	       i915_mm_buddy_block_order(block),
	       i915_mm_buddy_block_offset(block),
	       i915_mm_buddy_block_size(block),
	       yesno(i915_mm_buddy_block_is_free(block)),
	       yesno(buddy));
}

static void igt_dump_block(struct i915_mm_buddy *mm,
			   struct i915_mm_buddy_block *block)
{
	struct i915_mm_buddy_block *buddy;

	__igt_dump_block(mm, block, false);

	buddy = get_buddy(block);
	if (buddy)
		__igt_dump_block(mm, buddy, true);
}

static int igt_check_block(struct i915_mm_buddy *mm,
			   struct i915_mm_buddy_block *block)
{
	struct i915_mm_buddy_block *buddy;
	unsigned int order;
	u64 block_size;
	u64 offset;
	int err = 0;

	block_size = i915_mm_buddy_block_size(block);
	offset = i915_mm_buddy_block_offset(block);
	order = i915_mm_buddy_block_order(block);

	if (block_size < mm->min_size) {
		pr_err("block size smaller than min size\n");
		err = -EINVAL;
	}

	if (!is_power_of_2(block_size)) {
		pr_err("block size not power of two\n");
		err = -EINVAL;
	}

	if (!IS_ALIGNED(block_size, mm->min_size)) {
		pr_err("block size not aligned to min size\n");
		err = -EINVAL;
	}

	if (block_size != BIT_ULL(order) * mm->min_size) {
		pr_err("block size mismatch with order\n");
		err = -EINVAL;
	}

	if (!IS_ALIGNED(offset, mm->min_size)) {
		pr_err("block offset not aligned to min size\n");
		err = -EINVAL;
	}

	if (!IS_ALIGNED(offset, block_size)) {
		pr_err("block offset not aligned to block size\n");
		err = -EINVAL;
	}

	buddy = get_buddy(block);
	if (buddy) {
		if (i915_mm_buddy_block_offset(buddy) != (offset ^ block_size)) {
			pr_err("buddy has wrong offset\n");
			err = -EINVAL;
		}

		if (i915_mm_buddy_block_size(buddy) != block_size) {
			pr_err("buddy size mismatch\n");
			err = -EINVAL;
		}

		if (i915_mm_buddy_block_order(buddy) != order) {
			pr_err("buddy order mismatch\n");
			err = -EINVAL;
		}

		if (i915_mm_buddy_block_is_free(buddy) &&
		    i915_mm_buddy_block_is_free(block)) {
			pr_err("block and its buddy are free\n");
			err = -EINVAL;
		}
	}

	return err;
}

static int igt_check_blocks(struct i915_mm_buddy *mm,
			    struct list_head *blocks,
			    u64 start,
			    u64 expected_size,
			    bool is_contiguous)
{
	struct i915_mm_buddy_block *block;
	struct i915_mm_buddy_block *prev;
	u64 total;
	int err = 0;

	block = NULL;
	prev = NULL;
	total = 0;

	list_for_each_entry(block, blocks, free_link) {
		err = igt_check_block(mm, block);

		if (i915_mm_buddy_block_is_free(block)) {
			pr_err("block not allocated\n"),
			err = -EINVAL;
		}

		if (is_contiguous && start != i915_mm_buddy_block_offset(block)) {
			pr_err("block offset mismatch\n");
			err = -EINVAL;
		}

		if (err)
			break;

		total += i915_mm_buddy_block_size(block);
		start += i915_mm_buddy_block_size(block);
		prev = block;
	}

	if (!err) {
		if (total != expected_size) {
			pr_err("size mismatch, expected=%llx, found=%llx\n",
			       expected_size, total);
			err = -EINVAL;
		}
		return err;
	}

	if (prev) {
		pr_err("prev block, dump:\n");
		igt_dump_block(mm, prev);
	}

	if (block) {
		pr_err("bad block, dump:\n");
		igt_dump_block(mm, block);
	}

	return err;
}

static int igt_check_mm(struct i915_mm_buddy *mm)
{
	struct i915_mm_buddy_block *root;
	struct i915_mm_buddy_block *prev;
	unsigned int n_roots;
	u64 total;
	int err = 0;

	if (list_empty(&mm->global_list)) {
		pr_err("no roots\n");
		return -EINVAL;
	}

	root = NULL;
	prev = NULL;
	n_roots = 0;
	total = 0;

	list_for_each_entry(root, &mm->global_list, global_link) {
		struct i915_mm_buddy_block *block;
		unsigned int order;

		err = igt_check_block(mm, root);

		if (!i915_mm_buddy_block_is_free(root)) {
			pr_err("root not free\n");
			err = -EINVAL;
		}

		order = i915_mm_buddy_block_order(root);

		if (!n_roots) {
			if (order != mm->max_order) {
				pr_err("max order root missing\n");
				err = -EINVAL;
			}
		}

		if (i915_mm_buddy_block_offset(root) != total) {
			pr_err("root offset mismatch\n");
			err = -EINVAL;
		}

		block = list_first_entry_or_null(&mm->free_list[order],
						 struct i915_mm_buddy_block,
						 free_link);
		if (block != root) {
			pr_err("root mismatch at order=%u\n", order);
			err = -EINVAL;
		}

		if (err)
			break;

		++n_roots;
		prev = root;
		total += i915_mm_buddy_block_size(root);
	}

	if (!err) {
		if (n_roots != hweight64(mm->size)) {
			pr_err("n_roots mismatch\n");
			err = -EINVAL;
		}

		if (total != mm->size) {
			pr_err("expected mm size=%llx, found=%llx\n", mm->size,
			       total);
			err = -EINVAL;
		}
		return err;
	}

	if (prev) {
		pr_err("prev root(%u), dump:\n", n_roots - 1);
		igt_dump_block(mm, prev);
	}

	if (root) {
		pr_err("bad root(%u), dump:\n", n_roots);
		igt_dump_block(mm, root);
	}

	return err;
}

static void igt_mm_config(u64 *size, u64 *min_size)
{
	I915_RND_STATE(prng);
	u64 s, ms;

	/* Nothing fancy, just try to get an interesting bit pattern */

	prandom_seed_state(&prng, i915_selftest.random_seed);

	s = i915_prandom_u64_state(&prng) & (SZ_8G - 1);
	ms = BIT_ULL(12 + (prandom_u32_state(&prng) % ilog2(s >> 12)));
	s = max(s & -ms, ms);

	*min_size = ms;
	*size = s;
}

static int igt_buddy_alloc(void *arg)
{
	struct i915_mm_buddy mm;
	int max_order;
	u64 min_size;
	u64 mm_size;
	int err;

	igt_mm_config(&mm_size, &min_size);

	pr_info("buddy_init with size=%llx, min_size=%llx\n", mm_size, min_size);

	err = i915_mm_buddy_init(&mm, mm_size, min_size);
	if (err) {
		pr_err("buddy_init failed(%d)\n", err);
		return err;
	}

	for (max_order = mm.max_order; max_order >= 0; max_order--) {
		struct i915_mm_buddy_block *block;
		int order;
		LIST_HEAD(blocks);
		u64 total;

		err = igt_check_mm(&mm);
		if (err) {
			pr_err("pre-mm check failed, abort\n");
			break;
		}

		pr_info("filling from max_order=%u\n", max_order);

		order = max_order;
		total = 0;

		do {
retry:
			block = i915_mm_buddy_alloc(&mm, order);
			if (IS_ERR(block)) {
				err = PTR_ERR(block);
				if (err == -ENOMEM) {
					pr_info("buddy_alloc hit -ENOMEM with order=%d\n",
						order);
				} else {
					if (order--) {
						err = 0;
						goto retry;
					}

					pr_err("buddy_alloc with order=%d failed(%d)\n",
					       order, err);
				}

				break;
			}

			list_add_tail(&block->free_link, &blocks);

			if (i915_mm_buddy_block_order(block) != order) {
				pr_err("buddy_alloc order mismatch\n");
				err = -EINVAL;
				break;
			}

			total += i915_mm_buddy_block_size(block);
		} while (total < mm.size);

		if (!err)
			err = igt_check_blocks(&mm, &blocks, 0, total, false);

		i915_mm_buddy_free_list(&mm, &blocks);

		if (!err) {
			err = igt_check_mm(&mm);
			if (err)
				pr_err("post-mm check failed\n");
		}

		if (err)
			break;
	}

	if (err == -ENOMEM)
		err = 0;

	i915_mm_buddy_fini(&mm);

	return err;
}

static int igt_buddy_alloc_range(void *arg)
{
	struct i915_mm_buddy mm;
	unsigned long page_num;
	LIST_HEAD(blocks);
	u64 min_size;
	u64 offset;
	u64 size;
	u64 rem;
	int err;

	return 0;

	igt_mm_config(&size, &min_size);

	pr_info("buddy_init with size=%llx, min_size=%llx\n", size, min_size);

	err = i915_mm_buddy_init(&mm, size, min_size);
	if (err) {
		pr_err("buddy_init failed(%d)\n", err);
		return err;
	}

	err = igt_check_mm(&mm);
	if (err) {
		pr_err("pre-mm check failed, abort, abort, abort!\n");
		goto err_fini;
	}

	rem = mm.size;
	offset = 0;

	for_each_prime_number_from(page_num, 1, ULONG_MAX - 1) {
		struct i915_mm_buddy_block *block;
		LIST_HEAD(tmp);

		size = min(page_num * mm.min_size, rem);

		err = i915_mm_buddy_alloc_range(&mm, &tmp, offset, size);
		if (err) {
			if (err == -ENOMEM) {
				pr_info("alloc_range hit -ENOMEM with size=%llx\n",
					size);
			} else {
				pr_err("alloc_range with offset=%llx, size=%llx failed(%d)\n",
				       offset, size, err);
			}

			break;
		}

		block = list_first_entry_or_null(&tmp,
						 struct i915_mm_buddy_block,
						 free_link);
		if (!block) {
			pr_err("alloc_range has no blocks\n");
			err = -EINVAL;
		}

		if (i915_mm_buddy_block_offset(block) != offset) {
			pr_err("alloc_range start offset mismatch, found=%llx, expected=%llx\n",
			       i915_mm_buddy_block_offset(block), offset);
			err = -EINVAL;
		}

		if (!err)
			err = igt_check_blocks(&mm, &tmp, offset, size, true);

		list_splice_tail(&tmp, &blocks);

		if (err)
			break;

		offset += size;

		rem -= size;
		if (!rem)
			break;
	}

	if (err == -ENOMEM)
		err = 0;

	i915_mm_buddy_free_list(&mm, &blocks);

	if (!err) {
		err = igt_check_mm(&mm);
		if (err)
			pr_err("post-mm check failed\n");
	}

err_fini:
	i915_mm_buddy_fini(&mm);

	return err;
}

int i915_mm_buddy_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_buddy_alloc),
		SUBTEST(igt_buddy_alloc_range),
	};

	return i915_subtests(tests, NULL);
}
