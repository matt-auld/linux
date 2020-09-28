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
	unsigned short intr;
	unsigned short loop;
};

void i915_gem_ww_ctx_init(struct i915_gem_ww_ctx *ctx, bool intr);
void i915_gem_ww_ctx_fini(struct i915_gem_ww_ctx *ctx);
int __must_check i915_gem_ww_ctx_backoff(struct i915_gem_ww_ctx *ctx);
void i915_gem_ww_unlock_single(struct drm_i915_gem_object *obj);

/* Internal function used by for_i915_gem_ww()! Don't use. */
static inline int __i915_gem_ww_fini(struct i915_gem_ww_ctx *ww, int err)
{
	ww->loop = 0;
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(ww);
		if (!err)
			ww->loop = 1;
	}

	if (!ww->loop)
		i915_gem_ww_ctx_fini(ww);

	return err;
}

/* Internal function used by for_i915_gem_ww()! Don't use. */
static inline void
__i915_gem_ww_init(struct i915_gem_ww_ctx *ww, bool intr)
{
	i915_gem_ww_ctx_init(ww, intr);
	ww->loop = 1;
}

#if 0 /* Kerneldoc for macros? */
/**
 * for_i915_gem_ww - Run code as a ww transaction
 * @_ww: The context representing the transaction.
 * @_err: Error variable used during transaction that will be assigned
 * -EDEADLK if locking fails due to transation collistion.
 * @_intr: Whether to perform transaction locks interruptible.
 *
 * Return: After invoking this macro, the transaction error variable may
 * have been assigned any value during the transaction, but -EDEADLK is
 * filtered out. In particular, any transaction relaxation may have assigned
 * it to -EINTR.
 */
#endif
#define for_i915_gem_ww(_ww, _err, _intr)				\
	GEM_WARN_ON((_err) != 0);					\
	for (__i915_gem_ww_init(_ww, _intr), _err = 0; (_ww)->loop;	\
	     _err = __i915_gem_ww_fini(_ww, _err))

#endif
