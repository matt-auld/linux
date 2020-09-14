/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */
#ifndef __I915_GEM_WW_H__
#define __I915_GEM_WW_H__

#include <drm/drm_drv.h>

/**
 * struct i915_gem_ww_ctx - A context representing a ww transaction
 * @ctx: The ww_acquire_ctx used for dma_resv locking.
 * @obj_list: A list holding a list of refcounted objects.
 * @contended: If we hit an EDEADLK when locking, a refcounted pointer to
 * the object we were trying to lock.
 * @intr: Perform locks interruptible.
 */
struct i915_gem_ww_ctx {
	struct ww_acquire_ctx ctx;
	struct list_head obj_list;
	struct drm_i915_gem_object *contended;
	bool intr;
};

void i915_gem_ww_ctx_init(struct i915_gem_ww_ctx *ctx, bool intr);
void i915_gem_ww_ctx_fini(struct i915_gem_ww_ctx *ctx);
int __must_check i915_gem_ww_ctx_backoff(struct i915_gem_ww_ctx *ctx);
void i915_gem_ww_unlock_single(struct drm_i915_gem_object *obj);
#endif
