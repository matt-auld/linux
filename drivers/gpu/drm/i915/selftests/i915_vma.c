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

#include "mock_gem_device.h"
#include "mock_context.h"

static bool assert_vma(struct i915_vma *vma,
		       struct drm_i915_gem_object *obj,
		       struct i915_gem_context *ctx)
{
	if (vma->vm != &ctx->ppgtt->base) {
		pr_err("VMA created with wrong VM\n");
		return false;
	}

	if (vma->size != obj->base.size) {
		pr_err("VMA created with wrong size, found %llu, expected %zu\n",
		       vma->size, obj->base.size);
		return false;
	}

	if (vma->ggtt_view.type != I915_GGTT_VIEW_NORMAL) {
		pr_err("VMA created with wrong type [%d]\n",
		       vma->ggtt_view.type);
		return false;
	}

	return true;
}

static int create_vmas(struct drm_i915_private *i915,
		       struct list_head *objects,
		       struct list_head *contexts)
{
	struct drm_i915_gem_object *obj;
	struct i915_gem_context *ctx;
	int pinned;

	list_for_each_entry(obj, objects, batch_pool_link) {
		for (pinned = 0; pinned <= 1; pinned++) {
			list_for_each_entry(ctx, contexts, link) {
				struct i915_address_space *vm =
					&ctx->ppgtt->base;
				struct i915_vma *vma;
				int err;

				vma = i915_vma_instance(obj, vm, NULL);
				if (IS_ERR(vma))
					return PTR_ERR(vma);

				if (i915_vma_compare(vma, vm, NULL)) {
					pr_err("i915_vma_compare failed!\n");
					return -EINVAL;
				}

				if (!assert_vma(vma, obj, ctx)) {
					pr_err("VMA lookup/create failed\n");
					return -EINVAL;
				}

				if (!pinned) {
					err = i915_vma_pin(vma, 0, 0, PIN_USER);
					if (err) {
						pr_err("Failed to pin VMA\n");
						return err;
					}
				} else {
					i915_vma_unpin(vma);
				}
			}
		}
	}

	return 0;
}

static int igt_vma_create(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj, *on;
	struct i915_gem_context *ctx, *cn;
	unsigned long num_obj, num_ctx;
	unsigned long no, nc;
	IGT_TIMEOUT(end_time);
	LIST_HEAD(contexts);
	LIST_HEAD(objects);
	int err;

	/* Exercise creating many vma amonst many objections, checking the
	 * vma creation and lookup routines.
	 */

	no = 0;
	for_each_prime_number(num_obj, ULONG_MAX - 1) {
		for (; no < num_obj; no++) {
			obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
			if (IS_ERR(obj))
				goto err;

			list_add(&obj->batch_pool_link, &objects);
		}

		nc = 0;
		for_each_prime_number(num_ctx, MAX_CONTEXT_HW_ID) {
			for (; nc < num_ctx; nc++) {
				ctx = mock_context(i915, "mock");
				if (!ctx)
					goto err;

				list_move(&ctx->link, &contexts);
			}

			err = create_vmas(i915, &objects, &contexts);
			if (err)
				goto err;

			if (igt_timeout(end_time,
					"%s timed out: after %lu objects\n",
					__func__, no))
				goto out;
		}

		list_for_each_entry_safe(ctx, cn, &contexts, link)
			mock_context_close(ctx);
	}

out:
	/* Final pass to lookup all created contexts */
	err = create_vmas(i915, &objects, &contexts);
err:
	list_for_each_entry_safe(ctx, cn, &contexts, link)
		mock_context_close(ctx);

	list_for_each_entry_safe(obj, on, &objects, batch_pool_link)
		i915_gem_object_put(obj);
	return err;
}

int i915_vma_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_vma_create),
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

