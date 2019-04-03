// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "../i915_selftest.h"

#include "mock_drm.h"
#include "mock_context.h"

static int igt_fill_blt(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	struct rnd_state prng;
	IGT_TIMEOUT(end);
	u32 *vaddr;
	int err = 0;

	obj = i915_gem_object_create_internal(i915, SZ_2M);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vaddr = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto err_put;
	}

	prandom_seed_state(&prng, i915_selftest.random_seed);

	do {
		u32 val = prandom_u32_state(&prng);
		u32 i;

		mutex_lock(&i915->drm.struct_mutex);
		err = i915_gem_object_fill_blt(ctx, obj, val);
		mutex_unlock(&i915->drm.struct_mutex);
		if (err)
			break;

		mutex_lock(&i915->drm.struct_mutex);
		err = i915_gem_object_set_to_cpu_domain(obj, false);
		mutex_unlock(&i915->drm.struct_mutex);
		if (err)
			break;

		for (i = 0; i < obj->base.size / sizeof(u32); ++i) {
			if (vaddr[i] != val) {
				pr_err("vaddr[%d]=%u, expected=%u\n", i,
				       val, vaddr[i]);
				err = -EINVAL;
				break;
			}
		}
	} while (!time_after(jiffies, end));

	i915_gem_object_unpin_map(obj);
err_put:
	i915_gem_object_put(obj);
	return err;
}

int i915_gem_object_blt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_fill_blt),
	};
	struct drm_file *file;
	struct i915_gem_context *ctx;
	intel_wakeref_t wakeref;
	int err;

	if (i915_terminally_wedged(i915))
		return 0;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	wakeref = intel_runtime_pm_get(i915);

	ctx = live_context(i915, file);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto out_unlock;
	}

	err = i915_subtests(tests, ctx);

out_unlock:
	intel_runtime_pm_put(i915, wakeref);

	mock_file_free(i915, file);
	return err;
}
