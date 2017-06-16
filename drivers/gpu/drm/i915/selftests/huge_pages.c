/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "../i915_selftest.h"

#include <linux/prime_numbers.h>

#include "mock_drm.h"

static const unsigned int page_sizes[] = {
	I915_GTT_PAGE_SIZE_1G,
	I915_GTT_PAGE_SIZE_2M,
	I915_GTT_PAGE_SIZE_64K,
	I915_GTT_PAGE_SIZE_4K,
};

static unsigned int get_largest_page_size(struct drm_i915_private *i915,
					  size_t rem)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(page_sizes); ++i) {
		unsigned int page_size = page_sizes[i];

		if (HAS_PAGE_SIZE(i915, page_size) && rem >= page_size)
			return page_size;
	}

	return 0;
}

static int fake_get_huge_pages(struct drm_i915_gem_object *obj)
{
#define GFP (GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY)
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	size_t max_len = rounddown_pow_of_two(UINT_MAX);
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned int sg_mask = 0;
	size_t rem;

	st = kmalloc(sizeof(*st), GFP);
	if (!st)
		return -ENOMEM;

	if (sg_alloc_table(st, obj->base.size >> PAGE_SHIFT, GFP)) {
		kfree(st);
		return -ENOMEM;
	}

	/* Use optimal page sized chunks to fill in the sg table */
	rem = obj->base.size;
	sg = st->sgl;
	st->nents = 0;
	do {
		unsigned int page_size = get_largest_page_size(i915, rem);
		unsigned int len = min(page_size * (rem / page_size), max_len);

		GEM_BUG_ON(!page_size);

		sg->offset = 0;
		sg->length = len;
		sg_dma_len(sg) = len;
		sg_dma_address(sg) = page_size;

		sg_mask |= len;

		st->nents++;

		rem -= len;
		if (!rem) {
			sg_mark_end(sg);
			break;
		}

		sg = sg_next(sg);
	} while (1);

	obj->mm.madv = I915_MADV_DONTNEED;

	__i915_gem_object_set_pages(obj, st, sg_mask);

	return 0;
}

static int fake_get_huge_pages_single(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned int page_size;

	st = kmalloc(sizeof(*st), GFP);
	if (!st)
		return -ENOMEM;

	if (sg_alloc_table(st, 1, GFP)) {
		kfree(st);
		return -ENOMEM;
	}

	sg = st->sgl;
	st->nents = 1;

	page_size = get_largest_page_size(i915, obj->base.size);

	GEM_BUG_ON(!page_size);

	sg->offset = 0;
	sg->length = obj->base.size;
	sg_dma_len(sg) = obj->base.size;
	sg_dma_address(sg) = page_size;

	obj->mm.madv = I915_MADV_DONTNEED;

	__i915_gem_object_set_pages(obj, st, sg->length);

	return 0;
#undef GFP
}

static void fake_free_huge_pages(struct drm_i915_gem_object *obj,
				 struct sg_table *pages)
{
	sg_free_table(pages);
	kfree(pages);
}

static void fake_put_huge_pages(struct drm_i915_gem_object *obj,
				struct sg_table *pages)
{
	fake_free_huge_pages(obj, pages);
	obj->mm.dirty = false;
	obj->mm.madv = I915_MADV_WILLNEED;
}

static const struct drm_i915_gem_object_ops fake_ops = {
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE,
	.get_pages = fake_get_huge_pages,
	.put_pages = fake_put_huge_pages,
};

static const struct drm_i915_gem_object_ops fake_ops_single = {
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE,
	.get_pages = fake_get_huge_pages_single,
	.put_pages = fake_put_huge_pages,
};

static struct drm_i915_gem_object *
fake_huge_pages_object(struct drm_i915_private *i915, u64 size, bool single)
{
	struct drm_i915_gem_object *obj;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, I915_GTT_PAGE_SIZE));

	if (size >> PAGE_SHIFT > UINT_MAX)
		return ERR_PTR(-E2BIG);

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc(i915);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, size);

	if (single)
		i915_gem_object_init(obj, &fake_ops_single);
	else
		i915_gem_object_init(obj, &fake_ops);

	obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	obj->base.read_domains = I915_GEM_DOMAIN_CPU;
	obj->cache_level = I915_CACHE_NONE;

	return obj;
}

static int igt_mock_exhaust_device_supported_pages(void *arg)
{
	struct i915_hw_ppgtt *ppgtt = arg;
	struct drm_i915_private *i915 = ppgtt->base.i915;
	unsigned int saved_mask = INTEL_INFO(i915)->page_size_mask;
	struct drm_i915_gem_object *obj;
	int i, j, single;
	int err;

	/* Sanity check creating objects with every valid page support
	 * combination for our mock device.
	 */

	for (i = 1; i < BIT(ARRAY_SIZE(page_sizes)); i++) {
		unsigned int combination = 0;
		struct i915_vma *vma;

		for (j = 0; j < ARRAY_SIZE(page_sizes); j++) {
			if (i & BIT(j))
				combination |= page_sizes[j];
		}

		mkwrite_device_info(i915)->page_size_mask = combination;

		for (single = 0; single <= 1; ++single) {
			obj = fake_huge_pages_object(i915, combination, !!single);
			if (IS_ERR(obj)) {
				err = PTR_ERR(obj);
				goto out_device;
			}

			GEM_BUG_ON(obj->base.size != combination);

			err = i915_gem_object_pin_pages(obj);
			if (err)
				goto out_put;

			GEM_BUG_ON(obj->mm.page_sizes.sg != combination);

			vma = i915_vma_instance(obj, &ppgtt->base, NULL);
			if (IS_ERR(vma)) {
				err = PTR_ERR(vma);
				goto out_unpin;
			}

			err = i915_vma_pin(vma, 0, 0, PIN_USER);
			if (err) {
				i915_vma_close(vma);
				goto out_unpin;
			}

			GEM_BUG_ON(obj->mm.page_sizes.gtt);
			GEM_BUG_ON(!vma->page_sizes.sg);
			GEM_BUG_ON(!vma->page_sizes.phys);

			GEM_BUG_ON(vma->page_sizes.gtt & ~combination);

			i915_vma_unpin(vma);
			i915_vma_close(vma);

			i915_gem_object_unpin_pages(obj);
			i915_gem_object_put(obj);
		}
	}

	goto out_device;

out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);
out_device:
	mkwrite_device_info(i915)->page_size_mask = saved_mask;

	return err;
}

static int igt_mock_ppgtt_misaligned_dma(void *arg)
{
	struct i915_hw_ppgtt *ppgtt = arg;
	struct drm_i915_private *i915 = ppgtt->base.i915;
	unsigned long supported = INTEL_INFO(i915)->page_size_mask;
	struct drm_i915_gem_object *obj;
	int bit;
	int err;

	/* Sanity check dma misalignment for huge pages -- the dma addresses we
	 * insert into the paging structures need to always respect the page
	 * size alignment.
	 */

	bit = ilog2(I915_GTT_PAGE_SIZE_64K);

	for_each_set_bit_from(bit, &supported, BITS_PER_LONG) {
		IGT_TIMEOUT(end_time);
		unsigned int page_size = BIT(bit);
		unsigned int flags = PIN_USER | PIN_OFFSET_FIXED;
		struct i915_vma *vma;
		unsigned int offset;
		unsigned int size =
			round_up(page_size, I915_GTT_PAGE_SIZE_2M) << 1;

		obj = fake_huge_pages_object(i915, size, true);
		if (IS_ERR(obj))
			return PTR_ERR(obj);

		GEM_BUG_ON(obj->base.size != size);

		err = i915_gem_object_pin_pages(obj);
		if (err)
			goto out_put;

		GEM_BUG_ON(!(obj->mm.page_sizes.sg & page_size));

		/* Force the page size for this object */
		obj->mm.page_sizes.sg = page_size;

		vma = i915_vma_instance(obj, &ppgtt->base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out_unpin;
		}

		err = i915_vma_pin(vma, 0, 0, flags);
		if (err) {
			i915_vma_close(vma);
			goto out_unpin;
		}

		GEM_BUG_ON(vma->page_sizes.gtt != page_size);

		i915_vma_unpin(vma);
		err = i915_vma_unbind(vma);
		if (err) {
			i915_vma_close(vma);
			goto out_unpin;
		}

		/* Try all the other valid offsets until the next
		 * boundary -- should always fall back to using 4K
		 * pages.
		 */
		for (offset = 4096; offset < page_size; offset += 4096) {
			err = i915_vma_pin(vma, 0, 0, flags | offset);
			if (err) {
				i915_vma_close(vma);
				goto out_unpin;
			}

			GEM_BUG_ON(vma->page_sizes.gtt !=
				   I915_GTT_PAGE_SIZE_4K);

			i915_vma_unpin(vma);
			err = i915_vma_unbind(vma);
			if (err) {
				i915_vma_close(vma);
				goto out_unpin;
			}

			if (igt_timeout(end_time,
					"%s timed out at offset %x with page-size %x\n",
					__func__, offset, page_size))
				break;
		}

		i915_vma_close(vma);

		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}

	return 0;

out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static void close_object_list(struct list_head *objects,
			      struct i915_hw_ppgtt *ppgtt)
{
	struct drm_i915_gem_object *obj, *on;

	list_for_each_entry_safe(obj, on, objects, st_link) {
		struct i915_vma *vma;

		vma = i915_vma_instance(obj, &ppgtt->base, NULL);
		if (!IS_ERR(vma))
			i915_vma_close(vma);

		list_del(&obj->st_link);
		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}
}

static int igt_mock_ppgtt_huge_fill(void *arg)
{
	struct i915_hw_ppgtt *ppgtt = arg;
	struct drm_i915_private *i915 = ppgtt->base.i915;
	unsigned long max_pages = ppgtt->base.total >> PAGE_SHIFT;
	unsigned long page_num;
	bool single = false;
	LIST_HEAD(objects);
	IGT_TIMEOUT(end_time);
	int err;

	for_each_prime_number_from(page_num, 1, max_pages) {
		struct drm_i915_gem_object *obj;
		size_t size = page_num << PAGE_SHIFT;
		struct i915_vma *vma;
		unsigned int expected_gtt = 0;
		int i;

		obj = fake_huge_pages_object(i915, size, single);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			break;
		}

		GEM_BUG_ON(obj->base.size != size);

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			break;
		}

		list_add(&obj->st_link, &objects);

		GEM_BUG_ON(!obj->mm.page_sizes.sg);

		vma = i915_vma_instance(obj, &ppgtt->base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			break;
		}

		err = i915_vma_pin(vma, 0, 0, PIN_USER);
		if (err)
			break;

		GEM_BUG_ON(obj->mm.page_sizes.gtt);
		GEM_BUG_ON(!vma->page_sizes.sg);
		GEM_BUG_ON(!vma->page_sizes.phys);

		/* Figure out the expected gtt page size knowing that we go from
		 * largest to smallest page size sg chunks, and that we align to
		 * the largest page size.
		 */
		for (i = 0; i < ARRAY_SIZE(page_sizes); ++i) {
			unsigned int page_size = page_sizes[i];

			if (HAS_PAGE_SIZE(i915, page_size) &&
			    size >= page_size) {
				expected_gtt |= page_size;
				size &= page_size-1;
			}
		}

		GEM_BUG_ON(!expected_gtt);
		GEM_BUG_ON(size);

		if (expected_gtt & I915_GTT_PAGE_SIZE_4K)
			expected_gtt &= ~I915_GTT_PAGE_SIZE_64K;

		if (vma->page_sizes.sg & I915_GTT_PAGE_SIZE_64K) {
			GEM_BUG_ON(!IS_ALIGNED(vma->node.start,
					       I915_GTT_PAGE_SIZE_2M));
			GEM_BUG_ON(!IS_ALIGNED(vma->node.size,
					       I915_GTT_PAGE_SIZE_2M));
		}

		i915_vma_unpin(vma);

		if (vma->page_sizes.gtt != expected_gtt) {
			pr_err("gtt=%u, expected=%u, size=%zd, single=%s\n",
			       vma->page_sizes.gtt, expected_gtt,
			       obj->base.size, yesno(!!single));
			err = -EINVAL;
			break;
		}

		if (igt_timeout(end_time,
				"%s timed out at size %zd\n",
				__func__, obj->base.size))
			break;

		single = !single;
	}

	close_object_list(&objects, ppgtt);

	if (err == -ENOMEM || err == -ENOSPC)
		err = 0;

	return err;
}

static int igt_mock_ppgtt_64K(void *arg)
{
	struct i915_hw_ppgtt *ppgtt = arg;
	struct drm_i915_private *i915 = ppgtt->base.i915;
	struct drm_i915_gem_object *obj;
	const struct object_info {
		unsigned int size;
		unsigned int gtt;
		unsigned int offset;
	} objects[] = {
		/* Cases with forced padding/alignment */
		{
			.size = SZ_64K,
			.gtt = I915_GTT_PAGE_SIZE_64K,
			.offset = 0,
		},
		{
			.size = SZ_64K + SZ_4K,
			.gtt = I915_GTT_PAGE_SIZE_4K,
			.offset = 0,
		},
		{
			.size = SZ_64K - SZ_4K,
			.gtt = I915_GTT_PAGE_SIZE_4K,
			.offset = 0,
		},
		{
			.size = SZ_2M,
			.gtt = I915_GTT_PAGE_SIZE_64K,
			.offset = 0,
		},
		{
			.size = SZ_2M - SZ_4K,
			.gtt = I915_GTT_PAGE_SIZE_4K,
			.offset = 0,
		},
		{
			.size = SZ_2M + SZ_4K,
			.gtt = I915_GTT_PAGE_SIZE_64K | I915_GTT_PAGE_SIZE_4K,
			.offset = 0,
		},
		{
			.size = SZ_2M + SZ_64K,
			.gtt = I915_GTT_PAGE_SIZE_64K,
			.offset = 0,
		},
		{
			.size = SZ_2M - SZ_64K,
			.gtt = I915_GTT_PAGE_SIZE_64K,
			.offset = 0,
		},
		/* Try without any forced padding/alignment */
		{
			.size = SZ_64K,
			.offset = SZ_2M,
			.gtt = I915_GTT_PAGE_SIZE_4K,
		},
		{
			.size = SZ_128K,
			.offset = SZ_2M - SZ_64K,
			.gtt = I915_GTT_PAGE_SIZE_4K,
		},
	};
	struct i915_vma *vma;
	int i, single;
	int err;

	/* Sanity check some of the trickiness with 64K pages -- either we can
	 * safely mark the whole page-table(2M block) as 64K, or we have to
	 * always fallback to 4K.
	 */

	if (!HAS_PAGE_SIZE(i915, I915_GTT_PAGE_SIZE_64K))
		return 0;

	for (i = 0; i < ARRAY_SIZE(objects); ++i) {
		unsigned int size = objects[i].size;
		unsigned int expected_gtt = objects[i].gtt;
		unsigned int offset = objects[i].offset;
		unsigned int flags = PIN_USER;

		for (single = 0; single <= 1; single++) {
			obj = fake_huge_pages_object(i915, size, !!single);
			if (IS_ERR(obj))
				return PTR_ERR(obj);

			err = i915_gem_object_pin_pages(obj);
			if (err)
				goto out_put;

			GEM_BUG_ON(!obj->mm.page_sizes.sg);

			/* Disable 2M pages -- We only want to use 64K/4K pages
			 * for this test.
			 */
			obj->mm.page_sizes.sg &= ~I915_GTT_PAGE_SIZE_2M;

			vma = i915_vma_instance(obj, &ppgtt->base, NULL);
			if (IS_ERR(vma)) {
				err = PTR_ERR(vma);
				goto out_unpin;
			}

			if (offset)
				flags |= PIN_OFFSET_FIXED | offset;

			err = i915_vma_pin(vma, 0, 0, flags);
			if (err) {
				i915_vma_close(vma);
				goto out_unpin;
			}

			GEM_BUG_ON(obj->mm.page_sizes.gtt);
			GEM_BUG_ON(!vma->page_sizes.sg);
			GEM_BUG_ON(!vma->page_sizes.phys);

			if (!offset && vma->page_sizes.sg & I915_GTT_PAGE_SIZE_64K) {
				GEM_BUG_ON(!IS_ALIGNED(vma->node.start,
							I915_GTT_PAGE_SIZE_2M));
				GEM_BUG_ON(!IS_ALIGNED(vma->node.size,
							I915_GTT_PAGE_SIZE_2M));
			}

			if (vma->page_sizes.gtt != expected_gtt) {
				pr_err("gtt=%u, expected=%u, i=%d, single=%s\n",
				       vma->page_sizes.gtt, expected_gtt, i,
				       yesno(!!single));
				err = -EINVAL;
				goto out_vma;
			}

			i915_vma_unpin(vma);
			i915_vma_close(vma);

			i915_gem_object_unpin_pages(obj);
			i915_gem_object_put(obj);
		}
	}

	return 0;

out_vma:
	i915_vma_unpin(vma);
	i915_vma_close(vma);
out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static struct i915_vma *
gpu_write_dw(struct i915_vma *vma, u64 offset, u32 val)
{
	struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);
	const int gen = INTEL_GEN(vma->vm->i915);
	unsigned int count = vma->size >> PAGE_SHIFT;
	struct drm_i915_gem_object *obj;
	struct i915_vma *batch;
	unsigned int size;
	u32 *cmd;
	int n;
	int err;

	size = 1 + 4 * count * sizeof(u32);
	size = round_up(size, PAGE_SIZE);
	obj = i915_gem_object_create_internal(i915, size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	offset += vma->node.start;

	for (n = 0; n < count; n++) {
		if (gen >= 8) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = val;
		} else if (gen >= 4) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4 |
				(gen < 6 ? 1 << 22 : 0);
			*cmd++ = 0;
			*cmd++ = offset;
			*cmd++ = val;
		} else {
			*cmd++ = MI_STORE_DWORD_IMM | 1 << 22;
			*cmd++ = offset;
			*cmd++ = val;
		}

		offset += PAGE_SIZE;
	}

	*cmd = MI_BATCH_BUFFER_END;

	i915_gem_object_unpin_map(obj);

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		goto err;

	batch = i915_vma_instance(obj, vma->vm, NULL);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto err;
	}

	err = i915_vma_pin(batch, 0, 0, PIN_USER);
	if (err)
		goto err;

	return batch;

err:
	i915_gem_object_put(obj);

	return ERR_PTR(err);
}

static int gpu_write(struct i915_vma *vma,
		     struct i915_gem_context *ctx,
		     u32 dword,
		     u32 value)
{
	struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);
	struct drm_i915_gem_request *rq;
	struct i915_vma *batch;
	int flags = 0;
	int err;

	GEM_BUG_ON(!intel_engine_can_store_dword(i915->engine[RCS]));

	err = i915_gem_object_set_to_gtt_domain(vma->obj, true);
	if (err)
		return err;

	rq = i915_gem_request_alloc(i915->engine[RCS], ctx);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	batch = gpu_write_dw(vma, dword * sizeof(u32), value);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto err_request;
	}

	i915_vma_move_to_active(batch, rq, 0);
	i915_gem_object_set_active_reference(batch->obj);
	i915_vma_unpin(batch);
	i915_vma_close(batch);

	err = rq->engine->emit_flush(rq, EMIT_INVALIDATE);
	if (err)
		goto err_request;

	err = i915_switch_context(rq);
	if (err)
		goto err_request;

	err = rq->engine->emit_bb_start(rq,
			batch->node.start, batch->node.size,
			flags);
	if (err)
		goto err_request;

	i915_vma_move_to_active(vma, rq, 0);

	reservation_object_lock(vma->resv, NULL);
	reservation_object_add_excl_fence(vma->resv, &rq->fence);
	reservation_object_unlock(vma->resv);

err_request:
	__i915_add_request(rq, err == 0);

	return err;
}

static int unmap_mapping(struct drm_i915_gem_object *obj)
{
	void *ptr;
	int err;

	err = mutex_lock_interruptible(&obj->mm.lock);
	if (err)
		return err;

	ptr = page_mask_bits(obj->mm.mapping);
	if (ptr) {
		if (is_vmalloc_addr(ptr))
			vunmap(ptr);
		else
			kunmap(kmap_to_page(ptr));

		obj->mm.mapping = NULL;
	}

	mutex_unlock(&obj->mm.lock);

	return 0;
}

#define DWORDS_PER_PAGE (PAGE_SIZE/sizeof(u32))

static int cpu_check(struct drm_i915_gem_object *obj, u32 dword, u32 val)
{
	enum i915_map_type level;
	int err;

	for (level = I915_MAP_WB; level <= I915_MAP_WC; level++) {
		u32 *map, offset;

		if (level == I915_MAP_WB)
			err = i915_gem_object_set_to_cpu_domain(obj, false);
		else
			err = i915_gem_object_set_to_wc_domain(obj, false);
		if (err)
			return err;

		unmap_mapping(obj);
		map = i915_gem_object_pin_map(obj, level);
		if (IS_ERR(map))
			return PTR_ERR(map);

		for (offset = dword; offset < obj->base.size/sizeof(u32);
		     offset += DWORDS_PER_PAGE) {
			if (map[offset] != val) {
				pr_err("map[%u] = %u, expected %u\n",
				       offset, map[offset], val);
				err = -EINVAL;
				goto out_close;
			}
		}

		i915_gem_object_unpin_map(obj);
	}

	return 0;

out_close:
	i915_gem_object_unpin_map(obj);

	return err;
}

static int igt_write_huge(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	unsigned long supported = INTEL_INFO(i915)->page_size_mask;
	struct i915_gem_context *ctx = i915->kernel_context;
	struct i915_address_space *vm = ctx->ppgtt ? &ctx->ppgtt->base : &i915->ggtt.base;
	unsigned int flags = PIN_USER | PIN_OFFSET_FIXED;
	struct i915_vma *vma;
	int bit, last;
	int err;

	GEM_BUG_ON(obj->base.size != SZ_2M);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		return err;

	/* We want to run the test even if the platform doesn't support huge gtt
	 * pages -- our only requirement is that we were able to allocate a
	 * "huge-page".
	 */
	if (obj->mm.page_sizes.phys < I915_GTT_PAGE_SIZE_2M) {
		pr_info("Unable to allocate huge-page, finishing test early\n");
		goto out_unpin;
	}

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto out_unpin;
	}

	last = ilog2(I915_GTT_PAGE_SIZE_2M);

	for_each_set_bit(bit, &supported, last + 1) {
		IGT_TIMEOUT(end_time);
		unsigned int page_size = BIT(bit);
		u32 max = vm->total / I915_GTT_PAGE_SIZE_2M - 1;
		u32 num;

		/* Force the page size */
		vma->page_sizes.sg = obj->mm.page_sizes.sg = page_size;

		/* Try various offsets until we timeout -- we want to avoid
		 * issues hidden by effectively always using offset = 0.
		 */
		for_each_prime_number_from(num, 0, max) {
			u64 offset = num * I915_GTT_PAGE_SIZE_2M;
			u32 dword;

			err = i915_vma_unbind(vma);
			if (err)
				goto out_close;

			err = i915_vma_pin(vma, 0, 0, flags | offset);
			if (err) {
				/* The ggtt may have some pages reserved so
				 * refrain from erroring out.
				 */
				if (err == -ENOSPC && i915_is_ggtt(vm))
					continue;

				goto out_close;
			}

			GEM_BUG_ON(obj->mm.page_sizes.gtt);
			GEM_BUG_ON(vma->page_sizes.sg != page_size);
			GEM_BUG_ON(!vma->page_sizes.phys);

			if (vma->page_sizes.gtt != page_size) {
				dma_addr_t addr =
					i915_gem_object_get_dma_address(obj, 0);

				/* The only valid reason for this to ever fail
				 * would be if the dma-mapper screwed us over
				 * when we did the dma_map_sg(), since it has
				 * the final say over the dma address.
				 */
				GEM_BUG_ON(IS_ALIGNED(addr,
						      I915_GTT_PAGE_SIZE_2M));

				pr_info("dma address misaligned, hobbling on\n");
			}

			for (dword = 0; dword < DWORDS_PER_PAGE; ++dword) {
				err = gpu_write(vma, ctx, dword, num + 1);
				if (err) {
					pr_err("gpu_write failed with page-size %x\n",
					       page_size);
					i915_vma_unpin(vma);
					goto out_close;
				}

				err = cpu_check(obj, dword, num + 1);
				if (err) {
					pr_err("cpu_check failed with page-size %x\n",
					       page_size);
					i915_vma_unpin(vma);
					goto out_close;
				}
			}

			i915_vma_unpin(vma);

			if (num > 0 &&
			    igt_timeout(end_time,
					"%s timed out at offset %llx with ps %x\n",
					__func__, offset, page_size))
				break;
		}
	}

out_close:
	i915_vma_close(vma);
out_unpin:
	i915_gem_object_unpin_pages(obj);

	return err;
}

static int igt_ppgtt_write_huge(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj;
	int err;

	/* Sanity check that the HW uses huge pages correctly -- ensure that
	 * our writes land in the right place
	 */

	if (!intel_engine_can_store_dword(i915->engine[RCS])) {
		pr_info("store-dword-imm not supported, skipping\n");
		return 0;
	}

	/* Try without thp */
	obj = i915_gem_object_create_internal(i915, SZ_2M);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = igt_write_huge(obj);
	i915_gem_object_put(obj);
	if (err) {
		pr_err("write-huge failed with internal allocator\n");
		return err;
	}

	if (!has_transparent_hugepage()) {
		pr_info("thp not supported, skipping\n");
		return 0;
	}

	/* Try with thp through gemfs */
	obj = i915_gem_object_create(i915, SZ_2M);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = igt_write_huge(obj);
	i915_gem_object_put(obj);
	if (err)
		pr_err("write-huge failed with thp\n");

	return err;
}

static int igt_ppgtt_pin_update(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	unsigned long supported = INTEL_INFO(dev_priv)->page_size_mask;
	struct i915_hw_ppgtt *ppgtt = dev_priv->kernel_context->ppgtt;
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	unsigned int flags = PIN_USER | PIN_OFFSET_FIXED;
	unsigned int needs_flush;
	int bit, last;
	u32 *ptr;
	int err;

	/* Make sure there's no funny business when doing a PIN_UPDATE -- in the
	 * past we had a subtle issue with being able to incorrectly do multiple
	 * alloc va ranges on the same object when doing a PIN_UPDATE, which
	 * resulted in some pretty nasty bugs, though only when using
	 * huge-gtt-pages.
	 */

	if (!USES_FULL_48BIT_PPGTT(dev_priv)) {
		pr_info("48b PPGTT not supported, skipping\n");
		return 0;
	}

	bit = ilog2(I915_GTT_PAGE_SIZE_64K);
	last = ilog2(I915_GTT_PAGE_SIZE_2M);

	for_each_set_bit_from(bit, &supported, last + 1) {
		unsigned int page_size = BIT(bit);

		obj = i915_gem_object_create_internal(dev_priv, SZ_2M);
		if (IS_ERR(obj))
			return PTR_ERR(obj);

		vma = i915_vma_instance(obj, &ppgtt->base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out_put;
		}

		err = i915_vma_pin(vma, 0, 0, flags);
		if (err)
			goto out_close;

		if (vma->page_sizes.sg < page_size) {
			pr_info("Unable to allocate page-size %x, finishing test early\n",
				page_size);
			goto out_unpin;
		}

		if (vma->page_sizes.gtt != page_size) {
			dma_addr_t addr = i915_gem_object_get_dma_address(obj, 0);

			/* The only valid reason for this to ever fail would be
			 * if the dma-mapper screwed us over when we did the
			 * dma_map_sg(), since it has the final say over the dma
			 * address.
			 */
			GEM_BUG_ON(IS_ALIGNED(addr, page_size));

			pr_info("dma address misaligned, finishing test early\n");
			goto out_unpin;
		}

		err = i915_vma_bind(vma, I915_CACHE_NONE, PIN_UPDATE);
		if (err)
			goto out_unpin;

		i915_vma_unpin(vma);
		i915_vma_close(vma);

		i915_gem_object_put(obj);
	}

	obj = i915_gem_object_create_internal(dev_priv, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vma = i915_vma_instance(obj, &ppgtt->base, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto out_put;
	}

	err = i915_vma_pin(vma, 0, 0, flags);
	if (err)
		goto out_close;

	/* Make sure we don't end up with something like where the pde is still
	 * pointing to the 2M page, and the pt we just filled-in is dangling --
	 * we can check this by writing to the first page where it would then
	 * land in the now stale 2M page.
	 */

	err = gpu_write(vma, dev_priv->kernel_context, 0, 0xdeadbeaf);
	if (err)
		goto out_unpin;

	err = i915_gem_obj_prepare_shmem_read(obj, &needs_flush);
	if (err)
		goto out_unpin;

	ptr = kmap_atomic(i915_gem_object_get_page(obj, 0));
	if (needs_flush & CLFLUSH_BEFORE)
		drm_clflush_virt_range(ptr, PAGE_SIZE);

	if (*ptr != 0xdeadbeaf) {
		pr_err("ptr = %x, expected %x\n", *ptr, 0xdeadbeaf);
		err = -EINVAL;
	}

	kunmap_atomic(ptr);

	i915_gem_obj_finish_shmem_access(obj);

out_unpin:
	i915_vma_unpin(vma);
out_close:
	i915_vma_close(vma);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_ppgtt_gemfs_huge(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx = i915->kernel_context;
	struct i915_address_space *vm = ctx->ppgtt ? &ctx->ppgtt->base : &i915->ggtt.base;
	struct drm_i915_gem_object *obj;
	const unsigned int object_sizes[] = {
		I915_GTT_PAGE_SIZE_2M,
		I915_GTT_PAGE_SIZE_2M + I915_GTT_PAGE_SIZE_4K,
	};
	unsigned int flags = PIN_USER;
	int err;
	int i;

	/* Sanity check THP through gemfs */

	if (!has_transparent_hugepage()) {
		pr_info("thp not supported, skipping\n");
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(object_sizes); ++i) {
		unsigned int size = object_sizes[i];
		struct i915_vma *vma;

		obj = i915_gem_object_create(i915, size);
		if (IS_ERR(obj))
			return PTR_ERR(obj);

		err = i915_gem_object_pin_pages(obj);
		if (err)
			goto out_put;

		GEM_BUG_ON(!obj->mm.page_sizes.sg);

		if (obj->mm.page_sizes.phys < I915_GTT_PAGE_SIZE_2M) {
			pr_info("Unable to allocate thp, finishing test early\n");
			goto out_unpin;
		}

		vma = i915_vma_instance(obj, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out_unpin;
		}

		err = i915_vma_pin(vma, 0, 0, flags);
		if (err) {
			i915_vma_close(vma);
			goto out_unpin;
		}

		GEM_BUG_ON(obj->mm.page_sizes.gtt);
		GEM_BUG_ON(!vma->page_sizes.sg);
		GEM_BUG_ON(!vma->page_sizes.phys);

		if (vma->page_sizes.sg & I915_GTT_PAGE_SIZE_2M) {
			if (vma->page_sizes.gtt != size) {
				dma_addr_t addr =
					i915_gem_object_get_dma_address(obj, 0);

				/* The only valid reason for this to ever fail
				 * would be if the dma-mapper screwed us over
				 * when we did the dma_map_sg(), since it has
				 * the final say over the dma address.
				 */
				GEM_BUG_ON(IS_ALIGNED(addr,
						      I915_GTT_PAGE_SIZE_2M));
				pr_info("dma address unaligned\n");
			}

			GEM_BUG_ON(!IS_ALIGNED(vma->node.start,
					       I915_GTT_PAGE_SIZE_2M));
		}

		if (vma->page_sizes.sg & I915_GTT_PAGE_SIZE_64K) {
			GEM_BUG_ON(!IS_ALIGNED(vma->node.size,
					       I915_GTT_PAGE_SIZE_2M));
		}

		i915_vma_unpin(vma);
		i915_vma_close(vma);

		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}

	return 0;

out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

int i915_gem_huge_page_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_mock_exhaust_device_supported_pages),
		SUBTEST(igt_mock_ppgtt_misaligned_dma),
		SUBTEST(igt_mock_ppgtt_huge_fill),
		SUBTEST(igt_mock_ppgtt_64K),
	};
	int saved_ppgtt = i915.enable_ppgtt;
	struct drm_i915_private *dev_priv;
	struct pci_dev *pdev;
	struct i915_hw_ppgtt *ppgtt;
	int err;

	dev_priv = mock_gem_device();
	if (!dev_priv)
		return -ENOMEM;

	/* Pretend to be a device which supports the 48b PPGTT */
	i915.enable_ppgtt = 3;

	pdev = dev_priv->drm.pdev;
	dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(39));

	mutex_lock(&dev_priv->drm.struct_mutex);
	ppgtt = i915_ppgtt_create(dev_priv, ERR_PTR(-ENODEV), "mock");
	if (IS_ERR(ppgtt)) {
		err = PTR_ERR(ppgtt);
		goto out_unlock;
	}

	GEM_BUG_ON(!i915_vm_is_48bit(&ppgtt->base));

	err = i915_subtests(tests, ppgtt);

	i915_ppgtt_close(&ppgtt->base);
	i915_ppgtt_put(ppgtt);

out_unlock:
	mutex_unlock(&dev_priv->drm.struct_mutex);

	i915.enable_ppgtt = saved_ppgtt;

	drm_dev_unref(&dev_priv->drm);

	return err;
}

int i915_gem_huge_page_live_selftests(struct drm_i915_private *dev_priv)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ppgtt_gemfs_huge),
		SUBTEST(igt_ppgtt_pin_update),
		SUBTEST(igt_ppgtt_write_huge),
	};
	int err;

	if (!USES_PPGTT(dev_priv)) {
		pr_info("PPGTT not supported, skipping live-selftests\n");
		return 0;
	}

	mutex_lock(&dev_priv->drm.struct_mutex);
	err = i915_subtests(tests, dev_priv);
	mutex_unlock(&dev_priv->drm.struct_mutex);

	return err;
}
