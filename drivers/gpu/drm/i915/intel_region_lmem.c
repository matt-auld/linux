// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_memory_region.h"
#include "intel_region_lmem.h"

static const struct drm_i915_gem_object_ops region_lmem_obj_ops = {
	.get_pages = i915_memory_region_get_pages_buddy,
	.put_pages = i915_memory_region_put_pages_buddy,
	.release = i915_gem_object_release_memory_region,
};

static struct drm_i915_gem_object *
lmem_create_object(struct intel_memory_region *mem,
		   resource_size_t size,
		   unsigned int flags)
{
	struct drm_i915_private *i915 = mem->i915;
	struct drm_i915_gem_object *obj;
	unsigned int cache_level;

	if (flags & I915_BO_ALLOC_CONTIGUOUS)
		size = roundup_pow_of_two(size);

	if (size > BIT(mem->mm.max_order) * mem->mm.min_size)
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &region_lmem_obj_ops);

	obj->read_domains = I915_GEM_DOMAIN_CPU | I915_GEM_DOMAIN_GTT;

	cache_level = HAS_LLC(i915) ? I915_CACHE_LLC : I915_CACHE_NONE;
	i915_gem_object_set_cache_coherency(obj, cache_level);

	return obj;
}

static void
region_lmem_release(struct intel_memory_region *mem)
{
	io_mapping_fini(&mem->iomap);
	i915_memory_region_release_buddy(mem);
}

static int
region_lmem_init(struct intel_memory_region *mem)
{
	int ret;

	if (!io_mapping_init_wc(&mem->iomap,
				mem->io_start,
				resource_size(&mem->region)))
		return -EIO;

	ret = i915_memory_region_init_buddy(mem);
	if (ret)
		io_mapping_fini(&mem->iomap);

	return ret;
}

static const struct intel_memory_region_ops region_lmem_ops = {
	.init = region_lmem_init,
	.release = region_lmem_release,
	.create_object = lmem_create_object,
};

bool i915_gem_object_is_lmem(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *region = obj->memory_region;

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
