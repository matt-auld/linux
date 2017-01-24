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
#include "mock_drm.h"
#include "huge_gem_object.h"

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
		if (!IS_ERR(vma))
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

int i915_gem_gtt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ppgtt_alloc),
		SUBTEST(igt_ppgtt_fill),
	};

	return i915_subtests(tests, i915);
}
