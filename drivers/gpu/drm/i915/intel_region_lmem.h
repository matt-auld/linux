/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_REGION_LMEM_H
#define __INTEL_REGION_LMEM_H

unsigned long i915_gem_object_lmem_io_pfn(struct drm_i915_gem_object *obj,
					  unsigned long n);

bool i915_gem_object_is_lmem(struct drm_i915_gem_object *obj);

struct drm_i915_gem_object *
i915_gem_object_create_lmem(struct drm_i915_private *i915,
			    resource_size_t size,
			    unsigned int flags);

#endif /* !__INTEL_REGION_LMEM_H */
