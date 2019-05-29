/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_OBJECT_BLT_H__
#define __I915_GEM_OBJECT_BLT_H__

#include <linux/types.h>

struct drm_i915_gem_object;
struct intel_context;
struct i915_request;
struct i915_vma;

int intel_emit_vma_fill_blt(struct i915_request *rq,
			    struct i915_vma *vma,
			    u32 value);

int i915_gem_object_fill_blt(struct drm_i915_gem_object *obj,
			     struct intel_context *ce,
			     u32 value);

int intel_emit_vma_copy_blt(struct i915_request *rq,
			    struct i915_vma *src,
			    struct i915_vma *dst);

int i915_gem_object_copy_blt(struct drm_i915_gem_object *src,
			     struct drm_i915_gem_object *dst,
			     struct intel_context *ce);

#endif
