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

static int lmem_pwrite(struct drm_i915_gem_object *obj,
		       const struct drm_i915_gem_pwrite *arg)
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
	ret = i915_gem_object_set_to_wc_domain(obj, true);
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

		unwritten = copy_from_user((void __force*)vaddr + offset,
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
	intel_runtime_pm_put(rpm, wakeref);
	i915_gem_object_unlock_fence(obj, fence);
out_unpin:
	i915_gem_object_unpin_pages(obj);

	return ret;
}

vm_fault_t i915_gem_fault_lmem(struct vm_fault *vmf)
{
	struct vm_area_struct *area = vmf->vma;
	struct i915_mmap_offset *priv = area->vm_private_data;
	struct drm_i915_gem_object *obj = priv->obj;
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *i915 = to_i915(dev);
	unsigned long size = area->vm_end - area->vm_start;
	bool write = area->vm_flags & VM_WRITE;
	vm_fault_t vmf_ret;
	int i, ret;

	/* Sanity check that we allow writing into this object */
	if (i915_gem_object_is_readonly(obj) && write)
		return VM_FAULT_SIGBUS;

	for (i = 0; i < size >> PAGE_SHIFT; i++) {
		vmf_ret = vmf_insert_pfn(area,
					 (unsigned long)area->vm_start + i * PAGE_SIZE,
					 i915_gem_object_lmem_io_offset(obj, i) >> PAGE_SHIFT);
		if (vmf_ret & VM_FAULT_ERROR) {
			ret = vm_fault_to_errno(vmf_ret, 0);
			goto err;
		}
	}
err:
	switch (ret) {
	case -EIO:
		if (!i915_terminally_wedged(i915))
			return VM_FAULT_SIGBUS;
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	case -ENOSPC:
	case -EFAULT:
		return VM_FAULT_SIGBUS;
	default:
		WARN_ONCE(ret, "unhandled error in %s: %i\n", __func__, ret);
		return VM_FAULT_SIGBUS;
	}
}

static const struct drm_i915_gem_object_ops region_lmem_obj_ops = {
	.get_pages = i915_memory_region_get_pages_buddy,
	.put_pages = i915_memory_region_put_pages_buddy,
	.release = i915_gem_object_release_memory_region,

	.pread = lmem_pread,
	.pwrite = lmem_pwrite,
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
		if (ret) {
			GEM_BUG_ON(1);
			return ret;
		}
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
	.create_object = lmem_create_object,
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

resource_size_t i915_gem_object_lmem_io_offset(struct drm_i915_gem_object *obj,
					       unsigned long n)
{
	struct intel_memory_region *mem = obj->memory_region;
	dma_addr_t daddr;

	/*
	 * XXX: It's not a dma address, more a device address or physical
	 * offset, so we are clearly abusing the semantics of the sg_table
	 * here, and elsewhere like in the gtt paths.
	 */
	daddr = i915_gem_object_get_dma_address(obj, n);
	daddr -= intel_graphics_fake_lmem_res.start;

	return mem->io_start + daddr;
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

