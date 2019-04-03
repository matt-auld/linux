// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_gem_object_blt.h"

#include "intel_drv.h"

int i915_gem_object_fill_blt(struct i915_gem_context *ctx,
			     struct drm_i915_gem_object *obj,
			     u32 value)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_engine_cs *engine = i915->engine[BCS0];
	struct i915_address_space *vm;
	struct i915_request *rq;
	struct i915_vma *vma;
	u32 *cs;
	int err;

	lockdep_assert_held(&i915->drm.struct_mutex);

	vm = ctx->ppgtt ? &ctx->ppgtt->vm : &i915->ggtt.vm;

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		return err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err) {
		i915_vma_close(vma);
		return err;
	}

	rq = i915_request_alloc(engine, ctx);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_unpin;
	}

	cs = intel_ring_begin(rq, 8);

	if (INTEL_GEN(i915) >= 8) {
		*cs++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7-2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = obj->base.size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = lower_32_bits(vma->node.start);
		*cs++ = upper_32_bits(vma->node.start);
		*cs++ = value;
		*cs++ = MI_NOOP;
	} else {
		*cs++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (6-2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = obj->base.size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = vma->node.start;
		*cs++ = value;
		*cs++ = MI_NOOP;
		*cs++ = MI_NOOP;
	}

	intel_ring_advance(rq, cs);

	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (err)
		i915_request_skip(rq, err);

	i915_request_add(rq);
err_unpin:
	i915_vma_unpin(vma);
	return err;
}

int i915_gem_object_clear_blt(struct i915_gem_context *ctx,
			      struct drm_i915_gem_object *obj)
{
	return i915_gem_object_fill_blt(ctx, obj, 0);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_object_blt.c"
#endif
