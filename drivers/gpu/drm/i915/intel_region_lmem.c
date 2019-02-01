/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_memory_region.h"
#include "intel_region_lmem.h"

static int region_lmem_pread(struct drm_i915_gem_object *obj,
			     const struct drm_i915_gem_pread *args)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	intel_wakeref_t wakeref;
	char __user *user_data;
	unsigned int offset;
	unsigned long idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&i915->drm.struct_mutex);
	if (ret)
		goto out_unpin;

	wakeref = intel_runtime_pm_get(i915);

	ret = i915_gem_object_set_to_wc_domain(obj, false);
	mutex_unlock(&i915->drm.struct_mutex);
	if (ret)
		goto out_put;

	remain = args->size;
	user_data = u64_to_user_ptr(args->data_ptr);
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
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
	intel_runtime_pm_put(i915, wakeref);
out_unpin:
	i915_gem_object_unpin_pages(obj);

	return ret;
}

static int region_lmem_pwrite(struct drm_i915_gem_object *obj,
			      const struct drm_i915_gem_pwrite *args)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	intel_wakeref_t wakeref;
	char __user *user_data;
	unsigned int offset;
	unsigned long idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&i915->drm.struct_mutex);
	if (ret)
		goto out_unpin;

	wakeref = intel_runtime_pm_get(i915);

	ret = i915_gem_object_set_to_wc_domain(obj, true);
	mutex_unlock(&i915->drm.struct_mutex);
	if (ret)
		goto out_put;

	remain = args->size;
	user_data = u64_to_user_ptr(args->data_ptr);
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
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

		unwritten = copy_from_user((void __force *)vaddr + offset,
					   user_data,
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
	intel_runtime_pm_put(i915, wakeref);
out_unpin:
	i915_gem_object_unpin_pages(obj);

	return ret;
}

static int region_lmem_vmf_fill_pages(struct drm_i915_gem_object *obj,
				      struct vm_fault *vmf,
				      pgoff_t page_offset)
{
	struct vm_area_struct *area = vmf->vma;
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *i915 = to_i915(dev);
	unsigned long size = area->vm_end - area->vm_start;
	int i;
	vm_fault_t vmf_ret;

	for (i = 0; i < size >> PAGE_SHIFT; i++) {
		vmf_ret = vmf_insert_pfn(area,
					 (unsigned long)area->vm_start + i * PAGE_SIZE,
					 i915_gem_object_lmem_io_pfn(obj, i));
		if (vmf_ret & VM_FAULT_ERROR)
			return vm_fault_to_errno(vmf_ret, 0);
	}

	if (!obj->userfault_count++)
		list_add(&obj->userfault_link, &i915->mm.userfault_list);

	GEM_BUG_ON(!obj->userfault_count);

	return 0;
}

static const struct drm_i915_gem_object_ops region_lmem_obj_ops = {
	.get_pages = i915_memory_region_get_pages_buddy,
	.put_pages = i915_memory_region_put_pages_buddy,
	.release = i915_gem_object_release_memory_region,
	.pread = region_lmem_pread,
	.pwrite = region_lmem_pwrite,
	.vmf_fill_pages = region_lmem_vmf_fill_pages,
};

static struct drm_i915_gem_object *
region_lmem_object_create(struct intel_memory_region *mem,
			  resource_size_t size,
			  unsigned int flags)
{
	struct drm_i915_private *i915 = mem->i915;
	struct drm_i915_gem_object *obj;

	if (flags & I915_BO_ALLOC_CONTIGUOUS)
		size = roundup_pow_of_two(size);

	if (size > BIT(mem->mm.max_order) * mem->mm.min_size)
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc(i915);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &region_lmem_obj_ops);

	obj->read_domains = I915_GEM_DOMAIN_CPU | I915_GEM_DOMAIN_GTT;
	obj->cache_level = HAS_LLC(i915) ? I915_CACHE_LLC : I915_CACHE_NONE;

	return obj;
}

static int i915_gem_init_fake_lmem_bar(struct intel_memory_region *mem)
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

static void i915_gem_relase_fake_lmem_bar(struct intel_memory_region *mem)
{
	if (drm_mm_node_allocated(&mem->fake_mappable))
		drm_mm_remove_node(&mem->fake_mappable);
}

static void
region_lmem_release(struct intel_memory_region *mem)
{
	i915_gem_relase_fake_lmem_bar(mem);
	io_mapping_fini(&mem->iomap);
	i915_memory_region_release_buddy(mem);
}

static int
region_lmem_init(struct intel_memory_region *mem)
{
	int ret;

	if (intel_graphics_fake_lmem_res.start) {
		ret = i915_gem_init_fake_lmem_bar(mem);
		GEM_BUG_ON(ret);
	}

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
	.create_object = region_lmem_object_create,
};

/* XXX: Time to vfunc your life up? */
void __iomem *i915_gem_object_lmem_io_map_page(struct drm_i915_gem_object *obj,
					       unsigned long n)
{
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= intel_graphics_fake_lmem_res.start;

	return io_mapping_map_atomic_wc(&obj->memory_region->iomap, offset);
}

void __iomem *i915_gem_object_lmem_io_map(struct drm_i915_gem_object *obj,
					  unsigned long n,
					  unsigned long size)
{
	resource_size_t offset;

	GEM_BUG_ON(!(obj->flags & I915_BO_ALLOC_CONTIGUOUS));

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= intel_graphics_fake_lmem_res.start;

	return io_mapping_map_wc(&obj->memory_region->iomap, offset, size);
}

unsigned long i915_gem_object_lmem_io_pfn(struct drm_i915_gem_object *obj,
					  unsigned long n)
{
	struct intel_memory_region *mem = obj->memory_region;
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= intel_graphics_fake_lmem_res.start;

	return (mem->io_start + offset) >> PAGE_SHIFT;
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

struct intel_memory_region *
i915_gem_setup_fake_lmem(struct drm_i915_private *i915)
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
					 &region_lmem_ops);
	if (!IS_ERR(mem)) {
		DRM_INFO("Intel graphics fake LMEM: %pR\n", &mem->region);
		DRM_INFO("Intel graphics fake LMEM IO start: %llx\n",
			 (u64)mem->io_start);
	}

	return mem;
}

