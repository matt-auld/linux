/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_REGION_LMEM_H
#define __INTEL_REGION_LMEM_H


void __iomem *i915_gem_object_lmem_io_map(struct drm_i915_gem_object *obj,
					  unsigned long n, unsigned long size);
void __iomem *i915_gem_object_lmem_io_map_page(struct drm_i915_gem_object *obj,
					       unsigned long n);

resource_size_t i915_gem_object_lmem_io_offset(struct drm_i915_gem_object *obj,
					       unsigned long n);

bool i915_gem_object_is_lmem(struct drm_i915_gem_object *obj);

vm_fault_t i915_gem_fault_lmem(struct vm_fault *vmf);

struct drm_i915_gem_object *
i915_gem_object_create_lmem(struct drm_i915_private *i915,
			    resource_size_t size,
			    unsigned int flags);

struct intel_memory_region *
i915_gem_setup_fake_lmem(struct drm_i915_private *i915);

#endif /* !__INTEL_REGION_LMEM_H */
