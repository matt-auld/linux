// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "intel_memory_region.h"
#include "gem/i915_gem_region.h"
#include "gem/i915_gem_lmem.h"
#include "i915_drv.h"

const struct drm_i915_gem_object_ops i915_gem_lmem_obj_ops = {
	.flags = I915_GEM_OBJECT_IS_MAPPABLE,

	.get_pages = i915_gem_object_get_pages_buddy,
	.put_pages = i915_gem_object_put_pages_buddy,
	.release = i915_gem_object_release_memory_region,
};

/* XXX: Time to vfunc your life up? */
void __iomem *i915_gem_object_lmem_io_map_page(struct drm_i915_gem_object *obj,
					       unsigned long n)
{
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);

	return io_mapping_map_wc(&obj->mm.region->iomap, offset, PAGE_SIZE);
}

void __iomem *i915_gem_object_lmem_io_map_page_atomic(struct drm_i915_gem_object *obj,
						      unsigned long n)
{
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);

	return io_mapping_map_atomic_wc(&obj->mm.region->iomap, offset);
}

void __iomem *i915_gem_object_lmem_io_map(struct drm_i915_gem_object *obj,
					  unsigned long n,
					  unsigned long size)
{
	resource_size_t offset;

	GEM_BUG_ON(!(obj->flags & I915_BO_ALLOC_CONTIGUOUS));

	offset = i915_gem_object_get_dma_address(obj, n);

	return io_mapping_map_wc(&obj->mm.region->iomap, offset, size);
}

bool i915_gem_object_is_lmem(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *region = obj->mm.region;

	return region && region->type == INTEL_LMEM;
}

struct drm_i915_gem_object *
i915_gem_object_create_lmem(struct drm_i915_private *i915,
			    resource_size_t size,
			    unsigned int flags)
{
	return i915_gem_object_create_region(i915->regions[INTEL_MEMORY_LMEM],
					     size, flags);
}
