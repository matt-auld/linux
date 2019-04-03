/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */
#ifndef __I915_GEM_CLIENT_BLT_H__
#define __I915_GEM_CLIENT_BLT_H__

#include <linux/types.h>

struct drm_i915_gem_object;
struct i915_gem_context;
struct sg_table;

int i915_gem_schedule_fill_pages_blt(struct drm_i915_gem_object *obj,
				     struct i915_gem_context *ctx,
				     struct sg_table *pages,
				     unsigned int page_sizes,
				     u64 size,
				     u32 value);

#endif
