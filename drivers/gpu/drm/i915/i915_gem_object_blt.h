/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_OBJECT_BLT_H__
#define __I915_GEM_OBJECT_BLT_H__

#include <linux/types.h>

struct drm_i915_gem_object;
struct i915_gem_context;

int i915_gem_object_clear_blt(struct i915_gem_context *ctx,
			      struct drm_i915_gem_object *obj);

int i915_gem_object_fill_blt(struct i915_gem_context *ctx,
			     struct drm_i915_gem_object *obj,
			     u32 value);

#endif
