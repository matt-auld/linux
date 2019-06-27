// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_memory_region.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"
#include "intel_region_lmem.h"

static struct drm_i915_gem_object *
lmem_create_object(struct intel_memory_region *mem,
		   resource_size_t size,
		   unsigned int flags)
{
	struct drm_i915_private *i915 = mem->i915;
	struct drm_i915_gem_object *obj;

	if (flags & I915_BO_ALLOC_CONTIGUOUS)
		size = roundup_pow_of_two(size);

	if (size > BIT(mem->mm.max_order) * mem->mm.chunk_size)
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &i915_gem_lmem_obj_ops);

	obj->read_domains = I915_GEM_DOMAIN_WC | I915_GEM_DOMAIN_GTT;

	i915_gem_object_set_cache_coherency(obj, I915_CACHE_NONE);

	i915_gem_object_init_memory_region(obj, mem, flags);

	return obj;
}

const struct intel_memory_region_ops intel_region_lmem_ops = {
	.init = intel_memory_region_init_buddy,
	.release = intel_memory_region_release_buddy,
	.create_object = lmem_create_object,
};
