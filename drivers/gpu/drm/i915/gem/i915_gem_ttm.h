/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */
#ifndef _I915_GEM_TTM_H_
#define _I915_GEM_TTM_H_

#include "gem/i915_gem_object_types.h"

static inline struct ttm_buffer_object *
i915_gem_to_ttm(struct drm_i915_gem_object *obj)
{
	return &obj->__do_not_access;
}

static inline struct drm_i915_gem_object *
i915_ttm_to_gem(struct ttm_buffer_object *bo)
{
	return container_of(bo, struct drm_i915_gem_object, __do_not_access);
}

int __i915_gem_ttm_object_init(struct intel_memory_region *mem,
			       struct drm_i915_gem_object *obj,
			       resource_size_t size,
			       unsigned int flags);
#endif
