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

static int init_fake_lmem_bar(struct intel_memory_region *mem)
{
	struct drm_i915_private *i915 = mem->i915;
	struct i915_ggtt *ggtt = &i915->ggtt;
	unsigned long n;
	int ret;

	mem->fake_mappable.start = 0;
	mem->fake_mappable.size = resource_size(&mem->region);
	mem->fake_mappable.color = I915_COLOR_UNEVICTABLE;

	ret = drm_mm_reserve_node(&ggtt->vm.mm, &mem->fake_mappable);
	if (ret)
		return ret;

	/* 1:1 map the mappable aperture to our reserved region */
	for (n = 0; n < mem->fake_mappable.size >> PAGE_SHIFT; ++n) {
		ggtt->vm.insert_page(&ggtt->vm,
				     mem->region.start + (n << PAGE_SHIFT),
				     n << PAGE_SHIFT, I915_CACHE_NONE, 0);
	}

	return 0;
}

static void release_fake_lmem_bar(struct intel_memory_region *mem)
{
	if (drm_mm_node_allocated(&mem->fake_mappable))
		drm_mm_remove_node(&mem->fake_mappable);
}

static void
region_lmem_release(struct intel_memory_region *mem)
{
	release_fake_lmem_bar(mem);
	io_mapping_fini(&mem->iomap);
	intel_memory_region_release_buddy(mem);
}

static int
region_lmem_init(struct intel_memory_region *mem)
{
	int ret;

	if (intel_graphics_fake_lmem_res.start) {
		ret = init_fake_lmem_bar(mem);
		GEM_BUG_ON(ret);
	}

	if (!io_mapping_init_wc(&mem->iomap,
				mem->io_start,
				resource_size(&mem->region)))
		return -EIO;

	ret = intel_memory_region_init_buddy(mem);
	if (ret)
		io_mapping_fini(&mem->iomap);

	return ret;
}

const struct intel_memory_region_ops intel_region_lmem_ops = {
	.init = region_lmem_init,
	.release = region_lmem_release,
	.create_object = lmem_create_object,
};

struct intel_memory_region *
intel_setup_fake_lmem(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = i915->drm.pdev;
	struct intel_memory_region *mem;
	resource_size_t mappable_end;
	resource_size_t io_start;
	resource_size_t start;

	GEM_BUG_ON(HAS_MAPPABLE_APERTURE(i915));
	GEM_BUG_ON(!intel_graphics_fake_lmem_res.start);

	/* Your mappable aperture belongs to me now! */
	mappable_end = pci_resource_len(pdev, 2);
	io_start = pci_resource_start(pdev, 2),
	start = intel_graphics_fake_lmem_res.start;

	mem = intel_memory_region_create(i915,
					 start,
					 mappable_end,
					 I915_GTT_PAGE_SIZE_4K,
					 io_start,
					 &intel_region_lmem_ops);
	if (!IS_ERR(mem)) {
		DRM_INFO("Intel graphics fake LMEM: %pR\n", &mem->region);
		DRM_INFO("Intel graphics fake LMEM IO start: %llx\n",
			 (u64)mem->io_start);
	}

	return mem;
}
