/*
 * Copyright © 2016 Intel Corporation
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

#include <linux/prime_numbers.h>

#include "i915_selftest.h"
#include "i915_random.h"
#include "mock_drm.h"
#include "huge_gem_object.h"
#include "mock_gem_device.h"

static int igt_ppgtt_alloc(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	struct i915_hw_ppgtt *ppgtt;
	u64 size, last;
	int err;

	/* Allocate a ppggt and try to fill the entire range */

	if (!USES_PPGTT(dev_priv))
		return 0;

	ppgtt = kzalloc(sizeof(*ppgtt), GFP_KERNEL);
	if (!ppgtt)
		return -ENOMEM;

	err = __hw_ppgtt_init(ppgtt, dev_priv);
	if (err)
		goto err_ppgtt;

	if (!ppgtt->base.allocate_va_range)
		goto err_ppgtt_cleanup;

	/* Check we can allocate the entire range */
	for (size = 4096;
	     size <= ppgtt->base.total;
	     size <<= 2) {
		err = ppgtt->base.allocate_va_range(&ppgtt->base, 0, size);
		if (err) {
			if (err == -ENOMEM) {
				pr_info("[1] Ran out of memory for va_range [0 + %llx] [bit %d]\n",
					size, ilog2(size));
				err = 0; /* virtual space too large! */
			}
			goto err_ppgtt_cleanup;
		}

		ppgtt->base.clear_range(&ppgtt->base, 0, size);
	}

	/* Check we can incrementally allocate the entire range */
	for (last = 0, size = 4096;
	     size <= ppgtt->base.total;
	     last = size, size <<= 2) {
		err = ppgtt->base.allocate_va_range(&ppgtt->base,
						    last, size - last);
		if (err) {
			if (err == -ENOMEM) {
				pr_info("[2] Ran out of memory for va_range [%llx + %llx] [bit %d]\n",
					last, size - last, ilog2(size));
				err = 0; /* virtual space too large! */
			}
			goto err_ppgtt_cleanup;
		}
	}

err_ppgtt_cleanup:
	ppgtt->base.cleanup(&ppgtt->base);
err_ppgtt:
	kfree(ppgtt);
	return err;
}

static void close_object_list(struct list_head *objects,
			      struct i915_address_space *vm)
{
	struct drm_i915_gem_object *obj, *on;

	list_for_each_entry_safe(obj, on, objects, batch_pool_link) {
		struct i915_vma *vma;

		vma = i915_vma_instance(obj, vm, NULL);
		/* Only ppgtt vma may be closed before the object is freed */
		if (!IS_ERR(vma) && !i915_vma_is_ggtt(vma))
			i915_vma_close(vma);

		list_del(&obj->batch_pool_link);
		i915_gem_object_put(obj);
	}
}

static int fill_hole(struct drm_i915_private *i915,
		     struct i915_address_space *vm,
		     u64 hole_start, u64 hole_end,
		     unsigned long end_time)
{
	const u64 hole_size = hole_end - hole_start;
	struct drm_i915_gem_object *obj;
	const unsigned long max_pages =
		min_t(u64, 1 << 20, hole_size/2 >> PAGE_SHIFT);
	unsigned long npages, prime;
	struct i915_vma *vma;
	LIST_HEAD(objects);
	int err;

	for_each_prime_number_from(prime, 2, 13) {
		for (npages = 1; npages <= max_pages; npages *= prime) {
			const u64 full_size = npages << PAGE_SHIFT;
			const struct {
				u64 base;
				s64 step;
				const char *name;
			} phases[] = {
				{
					(hole_end - full_size) | PIN_OFFSET_FIXED | PIN_USER,
					-full_size,
					"top-down",
				},
				{
					hole_start | PIN_OFFSET_FIXED | PIN_USER,
					full_size,
					"bottom-up",
				},
				{ }
			}, *p;

			GEM_BUG_ON(!full_size);
			obj = huge_gem_object(i915, PAGE_SIZE, full_size);
			if (IS_ERR(obj))
				break;

			list_add(&obj->batch_pool_link, &objects);

			/* Align differing sized objects against the edges, and
			 * check we don't walk off into the void when binding
			 * them into the GTT.
			 */
			for (p = phases; p->name; p++) {
				u64 flags;

				flags = p->base;
				list_for_each_entry(obj, &objects, batch_pool_link) {
					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					err = i915_vma_pin(vma, 0, 0, flags);
					if (err) {
						pr_err("Fill %s pin failed with err=%d on size=%lu pages (prime=%lu), flags=%llx\n", p->name, err, npages, prime, flags);
						goto err;
					}

					i915_vma_unpin(vma);

					flags += p->step;
					if (flags < hole_start ||
					    flags > hole_end)
						break;
				}

				flags = p->base;
				list_for_each_entry(obj, &objects, batch_pool_link) {
					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					if (!drm_mm_node_allocated(&vma->node) ||
					    i915_vma_misplaced(vma, 0, 0, flags)) {
						pr_err("Fill %s moved vma.node=%llx + %llx, expected offset %llx\n",
						       p->name, vma->node.start, vma->node.size,
						       flags & PAGE_MASK);
						err = -EINVAL;
						goto err;
					}

					err = i915_vma_unbind(vma);
					if (err) {
						pr_err("Fill %s unbind of vma.node=%llx + %llx failed with err=%d\n",
						       p->name, vma->node.start, vma->node.size,
						       err);
						goto err;
					}

					flags += p->step;
					if (flags < hole_start ||
					    flags > hole_end)
						break;
				}
			}

			if (igt_timeout(end_time, "Fill timed out (npages=%lu, prime=%lu)\n",
					npages, prime)) {
				err = -EINTR;
				goto err;
			}
		}

		close_object_list(&objects, vm);
	}

	return 0;

err:
	close_object_list(&objects, vm);
	return err;
}

static int walk_hole(struct drm_i915_private *i915,
		     struct i915_address_space *vm,
		     u64 hole_start, u64 hole_end,
		     unsigned long end_time)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	u64 addr;
	int err;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	for (addr = hole_start; addr < hole_end; addr += PAGE_SIZE) {
		err = i915_vma_pin(vma, 0, 0,
				   addr | PIN_OFFSET_FIXED | PIN_USER);
		if (err) {
			pr_err("Walk bind failed at %llx with err=%d\n",
			       addr, err);
			break;
		}
		i915_vma_unpin(vma);

		if (!drm_mm_node_allocated(&vma->node) ||
		    i915_vma_misplaced(vma, 0, 0, addr | PIN_OFFSET_FIXED)) {
			pr_err("Walk incorrect at %llx\n", addr);
			err = -EINVAL;
			break;
		}

		err = i915_vma_unbind(vma);
		if (err) {
			pr_err("Walk unbind failed at %llx with err=%d\n",
			       addr, err);
			break;
		}

		if (igt_timeout(end_time, "Walk timed out at %llx\n", addr))
			break;
	}

	if (!i915_vma_is_ggtt(vma))
		i915_vma_close(vma);
err:
	i915_gem_object_put(obj);
	return err;
}

static int drunk_hole(struct drm_i915_private *i915,
		      struct i915_address_space *vm,
		      u64 hole_start, u64 hole_end,
		      unsigned long end_time)
{
	I915_RND_STATE(seed_prng);
	unsigned int size;

	/* Keep creating larger objects until one cannot fit into the hole */
	for (size = 12; (hole_end - hole_start) >> size; size++) {
		I915_RND_SUBSTATE(prng, seed_prng);
		struct drm_i915_gem_object *obj;
		unsigned int *order, count, n;
		u64 hole_size;

		hole_size = (hole_end - hole_start) >> size;
		if (hole_size > KMALLOC_MAX_SIZE / sizeof(u32))
			hole_size = KMALLOC_MAX_SIZE / sizeof(u32);
		count = hole_size;
		do {
			count >>= 1;
			order = i915_random_order(count, &prng);
		} while (!order && count);
		if (!order)
			break;

		/* Ignore allocation failures (i.e. don't report them as
		 * a test failure) as we are purposefully allocating very
		 * large objects without checking that we have sufficient
		 * memory. We expect to hit -ENOMEM.
		 */

		obj = huge_gem_object(i915, PAGE_SIZE, BIT_ULL(size));
		if (IS_ERR(obj)) {
			kfree(order);
			break;
		}

		GEM_BUG_ON(obj->base.size != BIT_ULL(size));

		if (i915_gem_object_pin_pages(obj)) {
			i915_gem_object_put(obj);
			kfree(order);
			break;
		}

		for (n = 0; n < count; n++) {
			if (vm->allocate_va_range &&
			    vm->allocate_va_range(vm,
						  order[n] * BIT_ULL(size),
						  BIT_ULL(size)))
				break;

			vm->insert_entries(vm, obj->mm.pages,
					   order[n] * BIT_ULL(size),
					   I915_GTT_PAGE_SIZE,
					   I915_CACHE_NONE, 0);
			if (igt_timeout(end_time,
					"%s timed out after %d/%d\n",
					__func__, n, count)) {
				hole_start = hole_end; /* quit */
				break;
			}
		}
		count = n;

		i915_random_reorder(order, count, &prng);
		for (n = 0; n < count; n++)
			vm->clear_range(vm,
					order[n]* BIT_ULL(size),
					BIT_ULL(size));

		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);

		kfree(order);
	}

	return 0;
}

static int igt_ppgtt_fill(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	struct drm_file *file;
	struct i915_hw_ppgtt *ppgtt;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding many VMA working outwards from either edge */

	if (!USES_FULL_PPGTT(dev_priv))
		return 0;

	file = mock_file(dev_priv);
	if (IS_ERR(file))
		return PTR_ERR(file);

	mutex_lock(&dev_priv->drm.struct_mutex);
	ppgtt = i915_ppgtt_create(dev_priv, file->driver_priv, "mock");
	if (IS_ERR(ppgtt)) {
		err = PTR_ERR(ppgtt);
		goto out_unlock;
	}
	GEM_BUG_ON(offset_in_page(ppgtt->base.total));

	err = fill_hole(dev_priv, &ppgtt->base, 0, ppgtt->base.total, end_time);

	i915_ppgtt_close(&ppgtt->base);
	i915_ppgtt_put(ppgtt);
out_unlock:
	mutex_unlock(&dev_priv->drm.struct_mutex);

	mock_file_free(dev_priv, file);
	return err;
}

static int igt_ppgtt_walk(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	struct drm_file *file;
	struct i915_hw_ppgtt *ppgtt;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding a single VMA in different positions along the ppgtt */

	if (!USES_FULL_PPGTT(dev_priv))
		return 0;

	file = mock_file(dev_priv);
	if (IS_ERR(file))
		return PTR_ERR(file);

	mutex_lock(&dev_priv->drm.struct_mutex);
	ppgtt = i915_ppgtt_create(dev_priv, file->driver_priv, "mock");
	if (IS_ERR(ppgtt)) {
		err = PTR_ERR(ppgtt);
		goto err_unlock;
	}
	GEM_BUG_ON(offset_in_page(ppgtt->base.total));

	err = walk_hole(dev_priv, &ppgtt->base, 0, ppgtt->base.total, end_time);

	i915_ppgtt_close(&ppgtt->base);
	i915_ppgtt_put(ppgtt);
err_unlock:
	mutex_unlock(&dev_priv->drm.struct_mutex);

	mock_file_free(dev_priv, file);
	return err;
}

static int igt_ppgtt_drunk(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	struct drm_file *file;
	struct i915_hw_ppgtt *ppgtt;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding many VMA in a random pattern within the ppgtt */

	if (!USES_FULL_PPGTT(dev_priv))
		return 0;

	file = mock_file(dev_priv);
	if (IS_ERR(file))
		return PTR_ERR(file);

	mutex_lock(&dev_priv->drm.struct_mutex);
	ppgtt = i915_ppgtt_create(dev_priv, file->driver_priv, "mock");
	if (IS_ERR(ppgtt)) {
		err = PTR_ERR(ppgtt);
		goto err_unlock;
	}
	GEM_BUG_ON(offset_in_page(ppgtt->base.total));

	err = drunk_hole(dev_priv, &ppgtt->base,
			 0, ppgtt->base.total,
			 end_time);

	i915_ppgtt_close(&ppgtt->base);
	i915_ppgtt_put(ppgtt);
err_unlock:
	mutex_unlock(&dev_priv->drm.struct_mutex);

	mock_file_free(dev_priv, file);
	return err;
}

static int igt_ggtt_fill(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_ggtt *ggtt = &i915->ggtt;
	u64 hole_start, hole_end;
	struct drm_mm_node *node;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding many VMA working outwards from either edge */

	mutex_lock(&i915->drm.struct_mutex);
	drm_mm_for_each_hole(node, &ggtt->base.mm, hole_start, hole_end) {
		if (ggtt->base.mm.color_adjust)
			ggtt->base.mm.color_adjust(node, 0,
						   &hole_start, &hole_end);
		if (hole_start >= hole_end)
			continue;

		err = fill_hole(i915, &ggtt->base,
				hole_start, hole_end,
				end_time);
		if (err)
			break;
	}
	mutex_unlock(&i915->drm.struct_mutex);

	return err;
}

static int igt_ggtt_walk(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_ggtt *ggtt = &i915->ggtt;
	u64 hole_start, hole_end;
	struct drm_mm_node *node;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding a single VMA in different positions along the ggtt */

	mutex_lock(&i915->drm.struct_mutex);
	drm_mm_for_each_hole(node, &ggtt->base.mm, hole_start, hole_end) {
		if (ggtt->base.mm.color_adjust)
			ggtt->base.mm.color_adjust(node, 0,
						   &hole_start, &hole_end);
		if (hole_end <= hole_start)
			continue;

		err = walk_hole(i915, &ggtt->base,
				hole_start, hole_end,
				end_time);
		if (err)
			break;
	}
	mutex_unlock(&i915->drm.struct_mutex);

	return err;
}

static int igt_ggtt_drunk(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_ggtt *ggtt = &i915->ggtt;
	u64 hole_start, hole_end;
	struct drm_mm_node *node;
	IGT_TIMEOUT(end_time);
	int err;

	/* Try binding many VMA in a random pattern within the ggtt */

	mutex_lock(&i915->drm.struct_mutex);
	drm_mm_for_each_hole(node, &ggtt->base.mm, hole_start, hole_end) {
		if (ggtt->base.mm.color_adjust)
			ggtt->base.mm.color_adjust(node, 0,
						   &hole_start, &hole_end);
		if (hole_start >= hole_end)
			continue;

		err = drunk_hole(i915, &ggtt->base,
				 hole_start, hole_end,
				 end_time);
		if (err)
			break;
	}
	mutex_unlock(&i915->drm.struct_mutex);

	return err;
}

static void track_vma_bind(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;

	obj->bind_count++; /* track for eviction later */
	__i915_gem_object_pin_pages(obj);

	vma->pages = obj->mm.pages;
	list_move_tail(&vma->vm_link, &vma->vm->inactive_list);
}

static int igt_gtt_reserve(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj, *on;
	LIST_HEAD(objects);
	u64 total;
	int err;

	/* i915_gem_gtt_reserve() tries to reserve the precise range
	 * for the node, and evicts if it has to. So our test checks that
	 * it can give us the requsted space and prevent overlaps.
	 */

	/* Start by filling the GGTT */
	for (total = 0;
	     total + 2*I915_GTT_PAGE_SIZE <= i915->ggtt.base.total;
	     total += 2*I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(i915, 2*PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto err;
		}

		list_add(&obj->batch_pool_link, &objects);

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		err = i915_gem_gtt_reserve(&i915->ggtt.base, &vma->node,
					   obj->base.size,
					   total,
					   obj->cache_level,
					   0);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 1) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != total ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 1) placement failed, found (%llx + %llx), expected (%llx + %lx)\n",
			       vma->node.start, vma->node.size,
			       total, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto err;
		}
	}

	/* Now we start forcing evictions */
	for (total = I915_GTT_PAGE_SIZE;
	     total + 2*I915_GTT_PAGE_SIZE <= i915->ggtt.base.total;
	     total += 2*I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(i915, 2*PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto err;
		}

		list_add(&obj->batch_pool_link, &objects);

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		err = i915_gem_gtt_reserve(&i915->ggtt.base, &vma->node,
					   obj->base.size,
					   total,
					   obj->cache_level,
					   0);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 2) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != total ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 2) placement failed, found (%llx + %llx), expected (%llx + %lx)\n",
			       vma->node.start, vma->node.size,
			       total, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto err;
		}
	}

	/* And then try at random */
	list_for_each_entry_safe(obj, on, &objects, batch_pool_link) {
		struct i915_vma *vma;
		u64 offset;

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		err = i915_vma_unbind(vma);
		if (err) {
			pr_err("i915_vma_unbind failed with err=%d!\n", err);
			goto err;
		}

		offset = random_offset(0, i915->ggtt.base.total,
				       2*I915_GTT_PAGE_SIZE,
				       I915_GTT_MIN_ALIGNMENT);

		err = i915_gem_gtt_reserve(&i915->ggtt.base, &vma->node,
					   obj->base.size,
					   offset,
					   obj->cache_level,
					   0);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 3) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != offset ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 3) placement failed, found (%llx + %llx), expected (%llx + %lx)\n",
			       vma->node.start, vma->node.size,
			       offset, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto err;
		}
	}

err:
	list_for_each_entry_safe(obj, on, &objects, batch_pool_link) {
		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}
	return err;
}

static int igt_gtt_insert(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj, *on;
	struct drm_mm_node tmp = {};
	const struct invalid_insert {
		u64 size;
		u64 alignment;
		u64 start, end;
	} invalid_insert[] = {
		{
			i915->ggtt.base.total + I915_GTT_PAGE_SIZE, 0,
			0, i915->ggtt.base.total,
		},
		{
			2*I915_GTT_PAGE_SIZE, 0,
			0, I915_GTT_PAGE_SIZE,
		},
		{
			-(u64)I915_GTT_PAGE_SIZE, 0,
			0, 4*I915_GTT_PAGE_SIZE,
		},
		{
			-(u64)2*I915_GTT_PAGE_SIZE, 2*I915_GTT_PAGE_SIZE,
			0, 4*I915_GTT_PAGE_SIZE,
		},
		{
			I915_GTT_PAGE_SIZE, I915_GTT_MIN_ALIGNMENT << 1,
			I915_GTT_MIN_ALIGNMENT, I915_GTT_MIN_ALIGNMENT << 1,
		},
		{}
	}, *ii;
	LIST_HEAD(objects);
	u64 total;
	int err;

	/* i915_gem_gtt_insert() tries to allocate some free space in the GTT
	 * to the node, evicting if required.
	 */

	/* Check a couple of obviously invalid requests */
	for (ii = invalid_insert; ii->size; ii++) {
		err = i915_gem_gtt_insert(&i915->ggtt.base, &tmp,
					  ii->size, ii->alignment,
					  I915_COLOR_UNEVICTABLE,
					  ii->start, ii->end,
					  0);
		if (err != -ENOSPC) {
			pr_err("Invalid i915_gem_gtt_insert(.size=%llx, .alignment=%llx, .start=%llx, .end=%llx) succeeded (err=%d)\n",
			       ii->size, ii->alignment, ii->start, ii->end,
			       err);
			return -EINVAL;
		}
	}

	/* Start by filling the GGTT */
	for (total = 0;
	     total + I915_GTT_PAGE_SIZE <= i915->ggtt.base.total;
	     total += I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(i915, I915_GTT_PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto err;
		}

		list_add(&obj->batch_pool_link, &objects);

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		err = i915_gem_gtt_insert(&i915->ggtt.base, &vma->node,
					   obj->base.size, 0, obj->cache_level,
					   0, i915->ggtt.base.total,
					   0);
		if (err == -ENOSPC) {
			/* maxed out the GGTT space */
			i915_gem_object_put(obj);
			break;
		}
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 1) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);
		__i915_vma_pin(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
	}

	list_for_each_entry(obj, &objects, batch_pool_link) {
		struct i915_vma *vma;

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		if (!drm_mm_node_allocated(&vma->node)) {
			pr_err("VMA was unexpectedly evicted!\n");
			err = -EINVAL;
			goto err;
		}

		__i915_vma_unpin(vma);
	}

	/* If we then reinsert, we should find the same hole */
	list_for_each_entry_safe(obj, on, &objects, batch_pool_link) {
		struct i915_vma *vma;
		u64 offset;

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		offset = vma->node.start;

		err = i915_vma_unbind(vma);
		if (err) {
			pr_err("i915_vma_unbind failed with err=%d!\n", err);
			goto err;
		}

		err = i915_gem_gtt_insert(&i915->ggtt.base, &vma->node,
					   obj->base.size, 0, obj->cache_level,
					   0, i915->ggtt.base.total,
					   0);
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 2) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != offset) {
			pr_err("i915_gem_gtt_insert did not return node to its previous location (the only hole), expected address %llx, found %llx\n",
			       offset, vma->node.start);
			err = -EINVAL;
			goto err;
		}
	}

	/* And then force evictions */
	for (total = 0;
	     total + 2*I915_GTT_PAGE_SIZE <= i915->ggtt.base.total;
	     total += 2*I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(i915, 2*I915_GTT_PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto err;
		}

		list_add(&obj->batch_pool_link, &objects);

		vma = i915_vma_instance(obj, &i915->ggtt.base, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err;
		}

		err = i915_gem_gtt_insert(&i915->ggtt.base, &vma->node,
					   obj->base.size, 0, obj->cache_level,
					   0, i915->ggtt.base.total,
					   0);
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 1) failed at %llu/%llu with err=%d\n",
			       total, i915->ggtt.base.total, err);
			goto err;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
	}

err:
	list_for_each_entry_safe(obj, on, &objects, batch_pool_link) {
		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}
	return err;
}

int i915_gem_gtt_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_gtt_reserve),
		SUBTEST(igt_gtt_insert),
	};
	struct drm_i915_private *i915;
	int err;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	mutex_lock(&i915->drm.struct_mutex);
	err = i915_subtests(tests, i915);
	mutex_unlock(&i915->drm.struct_mutex);

	drm_dev_unref(&i915->drm);
	return err;
}

int i915_gem_gtt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ppgtt_alloc),
		SUBTEST(igt_ppgtt_drunk),
		SUBTEST(igt_ppgtt_walk),
		SUBTEST(igt_ppgtt_fill),
		SUBTEST(igt_ggtt_drunk),
		SUBTEST(igt_ggtt_walk),
		SUBTEST(igt_ggtt_fill),
	};

	GEM_BUG_ON(offset_in_page(i915->ggtt.base.total));

	return i915_subtests(tests, i915);
}
