// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_memory_region.h"
#include "intel_region_lmem.h"

static int lmem_pread(struct drm_i915_gem_object *obj,
		      const struct drm_i915_gem_pread *arg)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	intel_wakeref_t wakeref;
	struct dma_fence *fence;
	char __user *user_data;
	unsigned int offset;
	unsigned long idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		return ret;

	i915_gem_object_lock(obj);
	ret = i915_gem_object_set_to_wc_domain(obj, false);
	if (ret) {
		i915_gem_object_unlock(obj);
		goto out_unpin;
	}

	fence = i915_gem_object_lock_fence(obj);
	i915_gem_object_unlock(obj);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unpin;
	}

	wakeref = intel_runtime_pm_get(rpm);

	remain = arg->size;
	user_data = u64_to_user_ptr(arg->data_ptr);
	offset = offset_in_page(arg->offset);
	for (idx = arg->offset >> PAGE_SHIFT; remain; idx++) {
		unsigned long unwritten;
		void __iomem *vaddr;
		int length;

		length = remain;
		if (offset + length > PAGE_SIZE)
			length = PAGE_SIZE - offset;

		vaddr = i915_gem_object_lmem_io_map_page(obj, idx);
		if (!vaddr) {
			ret = -ENOMEM;
			goto out_put;
		}

		unwritten = copy_to_user(user_data,
					 (void __force *)vaddr + offset,
					 length);
		io_mapping_unmap_atomic(vaddr);
		if (unwritten) {
			ret = -EFAULT;
			goto out_put;
		}

		remain -= length;
		user_data += length;
		offset = 0;
	}

out_put:
	intel_runtime_pm_put(rpm, wakeref);
	i915_gem_object_unlock_fence(obj, fence);
out_unpin:
	i915_gem_object_unpin_pages(obj);

	return ret;
}

static const struct drm_i915_gem_object_ops region_lmem_obj_ops = {
	.get_pages = i915_memory_region_get_pages_buddy,
	.put_pages = i915_memory_region_put_pages_buddy,
	.release = i915_gem_object_release_memory_region,

	.pread = lmem_pread,
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

/* XXX: Time to vfunc your life up? */
void __iomem *i915_gem_object_lmem_io_map_page(struct drm_i915_gem_object *obj,
					       unsigned long n)
{
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);

	return io_mapping_map_atomic_wc(&obj->memory_region->iomap, offset);
}

void __iomem *i915_gem_object_lmem_io_map(struct drm_i915_gem_object *obj,
					  unsigned long n,
					  unsigned long size)
{
	resource_size_t offset;

	GEM_BUG_ON(!(obj->flags & I915_BO_ALLOC_CONTIGUOUS));

	offset = i915_gem_object_get_dma_address(obj, n);

	return io_mapping_map_wc(&obj->memory_region->iomap, offset, size);
}

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
