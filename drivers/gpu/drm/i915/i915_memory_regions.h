/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_MEMORY_REGIONS_H__
#define __I915_MEMORY_REGIONS_H__

struct drm_i915_private;

int i915_memory_regions_hw_probe(struct drm_i915_private *i915);
void i915_memory_regions_driver_release(struct drm_i915_private *i915);

#endif /* __I915_MEMORY_REGIONS_H__ */
