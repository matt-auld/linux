// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_gem_object_blt.h"

#include "i915_gem_clflush.h"
#include "intel_drv.h"

int intel_emit_vma_fill_blt(struct i915_request *rq,
			    struct i915_vma *vma,
			    u32 value)
{
	u32 *cs;

	cs = intel_ring_begin(rq, 8);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	if (INTEL_GEN(rq->i915) >= 8) {
		*cs++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = vma->size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = lower_32_bits(vma->node.start);
		*cs++ = upper_32_bits(vma->node.start);
		*cs++ = value;
		*cs++ = MI_NOOP;
	} else {
		*cs++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (6 - 2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = vma->size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = vma->node.start;
		*cs++ = value;
		*cs++ = MI_NOOP;
		*cs++ = MI_NOOP;
	}

	intel_ring_advance(rq, cs);

	return 0;
}

int i915_gem_object_fill_blt(struct drm_i915_gem_object *obj,
			     struct intel_context *ce,
			     u32 value)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_gem_context *ctx = ce->gem_context;
	struct i915_address_space *vm = ctx->vm ?: &i915->ggtt.vm;
	struct i915_request *rq;
	struct i915_vma *vma;
	int err;

	/* XXX: ce->vm please */
	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (unlikely(err))
		return err;

	if (obj->cache_dirty & ~obj->cache_coherent) {
		i915_gem_object_lock(obj);
		i915_gem_clflush_object(obj, 0);
		i915_gem_object_unlock(obj);
	}

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_unpin;
	}

	err = i915_request_await_object(rq, obj, true);
	if (unlikely(err))
		goto out_request;

	if (ce->engine->emit_init_breadcrumb) {
		err = ce->engine->emit_init_breadcrumb(rq);
		if (unlikely(err))
			goto out_request;
	}

	i915_vma_lock(vma);
	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	i915_vma_unlock(vma);
	if (unlikely(err))
		goto out_request;

	err = intel_emit_vma_fill_blt(rq, vma, value);
out_request:
	if (unlikely(err))
		i915_request_skip(rq, err);

	i915_request_add(rq);
out_unpin:
	i915_vma_unpin(vma);
	return err;
}

int intel_emit_vma_copy_blt(struct i915_request *rq,
			    struct i915_vma *src,
			    struct i915_vma *dst)
{
	const int gen = INTEL_GEN(rq->i915);
	u32 *cs;

	GEM_BUG_ON(src->size != dst->size);

	cs = intel_ring_begin(rq, 10);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	if (gen >= 9) {
		*cs++ = GEN9_XY_FAST_COPY_BLT_CMD | (10-2);
		*cs++ = BLT_DEPTH_32 | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = src->size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = lower_32_bits(dst->node.start);
		*cs++ = upper_32_bits(dst->node.start);
		*cs++ = 0;
		*cs++ = PAGE_SIZE;
		*cs++ = lower_32_bits(src->node.start);
		*cs++ = upper_32_bits(src->node.start);
	} else if (gen >= 8) {
		*cs++ = XY_SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (10-2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = src->size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = lower_32_bits(dst->node.start);
		*cs++ = upper_32_bits(dst->node.start);
		*cs++ = 0;
		*cs++ = PAGE_SIZE;
		*cs++ = lower_32_bits(src->node.start);
		*cs++ = upper_32_bits(src->node.start);
	} else {
		*cs++ = XY_SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (8-2);
		*cs++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | PAGE_SIZE;
		*cs++ = 0;
		*cs++ = src->size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
		*cs++ = dst->node.start;
		*cs++ = 0;
		*cs++ = PAGE_SIZE;
		*cs++ = src->node.start;
		*cs++ = MI_NOOP;
		*cs++ = MI_NOOP;
	}

	intel_ring_advance(rq, cs);

	return 0;
}

int i915_gem_object_copy_blt(struct drm_i915_gem_object *src,
			     struct drm_i915_gem_object *dst,
			     struct intel_context *ce)
{
	struct drm_i915_private *i915 = to_i915(src->base.dev);
	struct i915_gem_context *ctx = ce->gem_context;
	struct i915_address_space *vm = ctx->vm ?: &i915->ggtt.vm;
	struct drm_gem_object *objs[] = { &src->base, &dst->base };
	struct ww_acquire_ctx acquire;
	struct i915_vma *vma_src, *vma_dst;
	struct i915_request *rq;
	int err;

	vma_src = i915_vma_instance(src, vm, NULL);
	if (IS_ERR(vma_src))
		return PTR_ERR(vma_src);

	err = i915_vma_pin(vma_src, 0, 0, PIN_USER);
	if (unlikely(err))
		return err;

	vma_dst = i915_vma_instance(dst, vm, NULL);
	if (IS_ERR(vma_dst))
		goto out_unpin_src;

	err = i915_vma_pin(vma_dst, 0, 0, PIN_USER);
	if (unlikely(err))
		goto out_unpin_src;

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_unpin_dst;
	}

	err = drm_gem_lock_reservations(objs, ARRAY_SIZE(objs), &acquire);
	if (unlikely(err))
		goto out_request;

	if (src->cache_dirty & ~src->cache_coherent)
		i915_gem_clflush_object(src, 0);

	if (dst->cache_dirty & ~dst->cache_coherent)
		i915_gem_clflush_object(dst, 0);

	err = i915_request_await_object(rq, src, false);
	if (unlikely(err))
		goto out_unlock;

	err = i915_vma_move_to_active(vma_src, rq, 0);
	if (unlikely(err))
		goto out_unlock;

	err = i915_request_await_object(rq, dst, true);
	if (unlikely(err))
		goto out_unlock;

	err = i915_vma_move_to_active(vma_dst, rq, EXEC_OBJECT_WRITE);
	if (unlikely(err))
		goto out_unlock;

	if (ce->engine->emit_init_breadcrumb) {
		err = ce->engine->emit_init_breadcrumb(rq);
		if (unlikely(err))
			goto out_unlock;
	}

	err = intel_emit_vma_copy_blt(rq, vma_src, vma_dst);
out_unlock:
	drm_gem_unlock_reservations(objs, ARRAY_SIZE(objs), &acquire);
out_request:
	if (unlikely(err))
		i915_request_skip(rq, err);

	i915_request_add(rq);
out_unpin_dst:
	i915_vma_unpin(vma_dst);
out_unpin_src:
	i915_vma_unpin(vma_src);
	return err;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_object_blt.c"
#endif
