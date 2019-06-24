/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include <linux/mman.h>
#include <linux/pfn_t.h>
#include <linux/sizes.h>

#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_gem_gtt.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_lmem.h"
#include "i915_gem_object.h"
#include "i915_trace.h"
#include "i915_vma.h"

static inline bool
__vma_matches(struct vm_area_struct *vma, struct file *filp,
	      unsigned long addr, unsigned long size)
{
	if (vma->vm_file != filp)
		return false;

	return vma->vm_start == addr &&
	       (vma->vm_end - vma->vm_start) == PAGE_ALIGN(size);
}

/**
 * i915_gem_mmap_ioctl - Maps the contents of an object, returning the address
 *			 it is mapped to.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * While the mapping holds a reference on the contents of the object, it doesn't
 * imply a ref on the object itself.
 *
 * IMPORTANT:
 *
 * DRM driver writers who look a this function as an example for how to do GEM
 * mmap support, please don't implement mmap support like here. The modern way
 * to implement DRM mmap support is with an mmap offset ioctl (like
 * i915_gem_mmap_gtt) and then using the mmap syscall on the DRM fd directly.
 * That way debug tooling like valgrind will understand what's going on, hiding
 * the mmap call in a driver private ioctl will break that. The i915 driver only
 * does cpu mmaps this way because we didn't know better.
 */
int
i915_gem_mmap_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_mmap *args = data;
	struct drm_i915_gem_object *obj;
	unsigned long addr;

	if (args->flags & ~(I915_MMAP_WC))
		return -EINVAL;

	if (args->flags & I915_MMAP_WC && !boot_cpu_has(X86_FEATURE_PAT))
		return -ENODEV;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* prime objects have no backing filp to GEM mmap
	 * pages from.
	 */
	if (!obj->base.filp) {
		addr = -ENXIO;
		goto err;
	}

	if (range_overflows(args->offset, args->size, (u64)obj->base.size)) {
		addr = -EINVAL;
		goto err;
	}

	addr = vm_mmap(obj->base.filp, 0, args->size,
		       PROT_READ | PROT_WRITE, MAP_SHARED,
		       args->offset);
	if (IS_ERR_VALUE(addr))
		goto err;

	if (args->flags & I915_MMAP_WC) {
		struct mm_struct *mm = current->mm;
		struct vm_area_struct *vma;

		if (down_write_killable(&mm->mmap_sem)) {
			addr = -EINTR;
			goto err;
		}
		vma = find_vma(mm, addr);
		if (vma && __vma_matches(vma, obj->base.filp, addr, args->size))
			vma->vm_page_prot =
				pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		else
			addr = -ENOMEM;
		up_write(&mm->mmap_sem);
		if (IS_ERR_VALUE(addr))
			goto err;

		/* This may race, but that's ok, it only gets set */
		WRITE_ONCE(obj->frontbuffer_ggtt_origin, ORIGIN_CPU);
	}
	i915_gem_object_put(obj);

	args->addr_ptr = (u64)addr;
	return 0;

err:
	i915_gem_object_put(obj);
	return addr;
}

static unsigned int tile_row_pages(const struct drm_i915_gem_object *obj)
{
	return i915_gem_object_get_tile_row_size(obj) >> PAGE_SHIFT;
}

/**
 * i915_gem_mmap_gtt_version - report the current feature set for GTT mmaps
 *
 * A history of the GTT mmap interface:
 *
 * 0 - Everything had to fit into the GTT. Both parties of a memcpy had to
 *     aligned and suitable for fencing, and still fit into the available
 *     mappable space left by the pinned display objects. A classic problem
 *     we called the page-fault-of-doom where we would ping-pong between
 *     two objects that could not fit inside the GTT and so the memcpy
 *     would page one object in at the expense of the other between every
 *     single byte.
 *
 * 1 - Objects can be any size, and have any compatible fencing (X Y, or none
 *     as set via i915_gem_set_tiling() [DRM_I915_GEM_SET_TILING]). If the
 *     object is too large for the available space (or simply too large
 *     for the mappable aperture!), a view is created instead and faulted
 *     into userspace. (This view is aligned and sized appropriately for
 *     fenced access.)
 *
 * 2 - Recognise WC as a separate cache domain so that we can flush the
 *     delayed writes via GTT before performing direct access via WC.
 *
 * 3 - Remove implicit set-domain(GTT) and synchronisation on initial
 *     pagefault; swapin remains transparent.
 *
 * Restrictions:
 *
 *  * snoopable objects cannot be accessed via the GTT. It can cause machine
 *    hangs on some architectures, corruption on others. An attempt to service
 *    a GTT page fault from a snoopable object will generate a SIGBUS.
 *
 *  * the object must be able to fit into RAM (physical memory, though no
 *    limited to the mappable aperture).
 *
 *
 * Caveats:
 *
 *  * a new GTT page fault will synchronize rendering from the GPU and flush
 *    all data to system memory. Subsequent access will not be synchronized.
 *
 *  * all mappings are revoked on runtime device suspend.
 *
 *  * there are only 8, 16 or 32 fence registers to share between all users
 *    (older machines require fence register for display and blitter access
 *    as well). Contention of the fence registers will cause the previous users
 *    to be unmapped and any new access will generate new page faults.
 *
 *  * running out of memory while servicing a fault may generate a SIGBUS,
 *    rather than the expected SIGSEGV.
 */
int i915_gem_mmap_gtt_version(void)
{
	return 3;
}

static inline struct i915_ggtt_view
compute_partial_view(const struct drm_i915_gem_object *obj,
		     pgoff_t page_offset,
		     unsigned int chunk)
{
	struct i915_ggtt_view view;

	if (i915_gem_object_is_tiled(obj))
		chunk = roundup(chunk, tile_row_pages(obj));

	view.type = I915_GGTT_VIEW_PARTIAL;
	view.partial.offset = rounddown(page_offset, chunk);
	view.partial.size =
		min_t(unsigned int, chunk,
		      (obj->base.size >> PAGE_SHIFT) - view.partial.offset);

	/* If the partial covers the entire object, just create a normal VMA. */
	if (chunk >= obj->base.size >> PAGE_SHIFT)
		view.type = I915_GGTT_VIEW_NORMAL;

	return view;
}

/**
 * i915_gem_fault - fault a page into the GTT
 * @vmf: fault info
 *
 * The fault handler is set up by drm_gem_mmap() when a object is GTT mapped
 * from userspace.  The fault handler takes care of binding the object to
 * the GTT (if needed), allocating and programming a fence register (again,
 * only if needed based on whether the old reg is still valid or the object
 * is tiled) and inserting a new PTE into the faulting process.
 *
 * Note that the faulting process may involve evicting existing objects
 * from the GTT and/or fence registers to make room.  So performance may
 * suffer if the GTT working set is large or there are few fence registers
 * left.
 *
 * The current feature set supported by i915_gem_fault() and thus GTT mmaps
 * is exposed via I915_PARAM_MMAP_GTT_VERSION (see i915_gem_mmap_gtt_version).
 */
vm_fault_t i915_gem_fault(struct vm_fault *vmf)
{
#define MIN_CHUNK_PAGES (SZ_1M >> PAGE_SHIFT)
	struct vm_area_struct *area = vmf->vma;
	struct i915_mmap_offset *priv = area->vm_private_data;
	struct drm_i915_gem_object *obj = priv->obj;
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *i915 = to_i915(dev);
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	struct i915_ggtt *ggtt = &i915->ggtt;
	bool write = area->vm_flags & VM_WRITE;
	intel_wakeref_t wakeref;
	struct i915_vma *vma;
	pgoff_t page_offset;
	int srcu;
	int ret;

	/* Sanity check that we allow writing into this object */
	if (i915_gem_object_is_readonly(obj) && write)
		return VM_FAULT_SIGBUS;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = (vmf->address - area->vm_start) >> PAGE_SHIFT;

	trace_i915_gem_object_fault(obj, page_offset, true, write);

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		goto err;

	wakeref = intel_runtime_pm_get(rpm);

	srcu = intel_gt_reset_trylock(ggtt->vm.gt);
	if (srcu < 0) {
		ret = srcu;
		goto err_rpm;
	}

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		goto err_reset;

	/* Access to snoopable pages through the GTT is incoherent. */
	if (obj->cache_level != I915_CACHE_NONE && !HAS_LLC(i915)) {
		ret = -EFAULT;
		goto err_unlock;
	}

	/* Now pin it into the GTT as needed */
	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0,
				       PIN_MAPPABLE |
				       PIN_NONBLOCK |
				       PIN_NONFAULT);
	if (IS_ERR(vma)) {
		/* Use a partial view if it is bigger than available space */
		struct i915_ggtt_view view =
			compute_partial_view(obj, page_offset, MIN_CHUNK_PAGES);
		unsigned int flags;

		flags = PIN_MAPPABLE;
		if (view.type == I915_GGTT_VIEW_NORMAL)
			flags |= PIN_NONBLOCK; /* avoid warnings for pinned */

		/*
		 * Userspace is now writing through an untracked VMA, abandon
		 * all hope that the hardware is able to track future writes.
		 */
		obj->frontbuffer_ggtt_origin = ORIGIN_CPU;

		vma = i915_gem_object_ggtt_pin(obj, &view, 0, 0, flags);
		if (IS_ERR(vma) && !view.type) {
			flags = PIN_MAPPABLE;
			view.type = I915_GGTT_VIEW_PARTIAL;
			vma = i915_gem_object_ggtt_pin(obj, &view, 0, 0, flags);
		}
	}
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_unlock;
	}

	ret = i915_vma_pin_fence(vma);
	if (ret)
		goto err_unpin;

	/* Finally, remap it using the new GTT offset */
	ret = remap_io_mapping(area,
			       area->vm_start + (vma->ggtt_view.partial.offset << PAGE_SHIFT),
			       (ggtt->gmadr.start + vma->node.start) >> PAGE_SHIFT,
			       min_t(u64, vma->size, area->vm_end - area->vm_start),
			       &ggtt->iomap);
	if (ret)
		goto err_fence;

	/* Mark as being mmapped into userspace for later revocation */
	assert_rpm_wakelock_held(rpm);
	if (!i915_vma_set_userfault(vma) && !obj->userfault_count++)
		list_add(&obj->userfault_link, &i915->ggtt.userfault_list);
	if (CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND)
		intel_wakeref_auto(&i915->ggtt.userfault_wakeref,
				   msecs_to_jiffies_timeout(CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND));
	GEM_BUG_ON(!obj->userfault_count);

	i915_vma_set_ggtt_write(vma);

err_fence:
	i915_vma_unpin_fence(vma);
err_unpin:
	__i915_vma_unpin(vma);
err_unlock:
	mutex_unlock(&dev->struct_mutex);
err_reset:
	intel_gt_reset_unlock(ggtt->vm.gt, srcu);
err_rpm:
	intel_runtime_pm_put(rpm, wakeref);
	i915_gem_object_unpin_pages(obj);
err:
	switch (ret) {
	case -EIO:
		/*
		 * We eat errors when the gpu is terminally wedged to avoid
		 * userspace unduly crashing (gl has no provisions for mmaps to
		 * fail). But any other -EIO isn't ours (e.g. swap in failure)
		 * and so needs to be reported.
		 */
		if (!intel_gt_is_wedged(ggtt->vm.gt))
			return VM_FAULT_SIGBUS;
		/* else, fall through */
	case -EAGAIN:
		/*
		 * EAGAIN means the gpu is hung and we'll wait for the error
		 * handler to reset everything when re-faulting in
		 * i915_mutex_lock_interruptible.
		 */
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		/*
		 * EBUSY is ok: this just means that another thread
		 * already did the job.
		 */
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

static vm_fault_t i915_gem_fault_cpu(struct vm_fault *vmf)
{
	struct vm_area_struct *area = vmf->vma;
	struct i915_mmap_offset *priv = area->vm_private_data;
	struct drm_i915_gem_object *obj = priv->obj;
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	vm_fault_t vmf_ret;
	unsigned long size = area->vm_end - area->vm_start;
	bool write = area->vm_flags & VM_WRITE;
	int i, ret;

	/* Sanity check that we allow writing into this object */
	if (i915_gem_object_is_readonly(obj) && write)
		return VM_FAULT_SIGBUS;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		goto err;

	for (i = 0; i < size >> PAGE_SHIFT; i++) {
		struct page *page = i915_gem_object_get_page(obj, i);
		vmf_ret = vmf_insert_pfn(area,
					 (unsigned long)area->vm_start + i * PAGE_SIZE,
					 page_to_pfn(page));
		if (vmf_ret & VM_FAULT_ERROR) {
			ret = vm_fault_to_errno(vmf_ret, 0);
			break;
		}
	}

	i915_gem_object_unpin_pages(obj);
err:
	switch (ret) {
	case -EIO:
		if (!intel_gt_is_wedged(&dev_priv->gt))
			return VM_FAULT_SIGBUS;
		/* fallthrough */
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

void __i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma;
	struct i915_mmap_offset *mmo;

	GEM_BUG_ON(!obj->userfault_count);

	obj->userfault_count = 0;
	list_del(&obj->userfault_link);

	mutex_lock(&obj->mmo_lock);
	list_for_each_entry(mmo, &obj->mmap_offsets, offset) {
		if (mmo->mmap_type == I915_MMAP_TYPE_GTT)
			drm_vma_node_unmap(&mmo->vma_node,
					   obj->base.dev->anon_inode->i_mapping);
	}
	mutex_unlock(&obj->mmo_lock);

	for_each_ggtt_vma(vma, obj)
		i915_vma_unset_userfault(vma);
}

/**
 * It is vital that we remove the page mapping if we have mapped a tiled
 * object through the GTT and then lose the fence register due to
 * resource pressure. Similarly if the object has been moved out of the
 * aperture, than pages mapped into userspace must be revoked. Removing the
 * mapping will then trigger a page fault on the next user access, allowing
 * fixup by i915_gem_fault().
 */
static void i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	intel_wakeref_t wakeref;

	/* Serialisation between user GTT access and our code depends upon
	 * revoking the CPU's PTE whilst the mutex is held. The next user
	 * pagefault then has to wait until we release the mutex.
	 *
	 * Note that RPM complicates somewhat by adding an additional
	 * requirement that operations to the GGTT be made holding the RPM
	 * wakeref.
	 */
	lockdep_assert_held(&i915->drm.struct_mutex);
	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	if (!obj->userfault_count)
		goto out;

	__i915_gem_object_release_mmap_gtt(obj);

	/* Ensure that the CPU's PTE are revoked and there are not outstanding
	 * memory transactions from userspace before we return. The TLB
	 * flushing implied above by changing the PTE above *should* be
	 * sufficient, an extra barrier here just provides us with a bit
	 * of paranoid documentation about our requirement to serialise
	 * memory writes before touching registers / GSM.
	 */
	wmb();

out:
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
}

static void i915_gem_object_release_mmap_offset(struct drm_i915_gem_object *obj)
{
	struct i915_mmap_offset *mmo;

	mutex_lock(&obj->mmo_lock);
	list_for_each_entry(mmo, &obj->mmap_offsets, offset) {
		if (mmo->mmap_type == I915_MMAP_TYPE_OFFSET_WC ||
		    mmo->mmap_type == I915_MMAP_TYPE_OFFSET_WB ||
		    mmo->mmap_type == I915_MMAP_TYPE_OFFSET_UC)
			drm_vma_node_unmap(&mmo->vma_node,
					   obj->base.dev->anon_inode->i_mapping);
	}
	mutex_unlock(&obj->mmo_lock);
}

/**
 * i915_gem_object_release_mmap - remove physical page mappings
 * @obj: obj in question
 *
 * Preserve the reservation of the mmapping with the DRM core code, but
 * relinquish ownership of the pages back to the system.
 */
void i915_gem_object_release_mmap(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_mmap_gtt(obj);
	i915_gem_object_release_mmap_offset(obj);
}

static void init_mmap_offset(struct drm_i915_gem_object *obj,
			     struct i915_mmap_offset *mmo)
{
	mutex_lock(&obj->mmo_lock);
	kref_init(&mmo->ref);
	list_add(&mmo->offset, &obj->mmap_offsets);
	mutex_unlock(&obj->mmo_lock);
}

static int create_mmap_offset(struct drm_i915_gem_object *obj,
			      struct i915_mmap_offset *mmo)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct drm_device *dev = obj->base.dev;
	int err;

	drm_vma_node_reset(&mmo->vma_node);
	if (mmo->file)
		drm_vma_node_allow(&mmo->vma_node, mmo->file);
	err = drm_vma_offset_add(dev->vma_offset_manager, &mmo->vma_node,
				 obj->base.size / PAGE_SIZE);
	if (likely(!err)) {
		init_mmap_offset(obj, mmo);
		return 0;
	}

	/* Attempt to reap some mmap space from dead objects */
	do {
		err = i915_gem_wait_for_idle(i915,
					     I915_WAIT_INTERRUPTIBLE,
					     MAX_SCHEDULE_TIMEOUT);
		if (err)
			break;

		i915_gem_drain_freed_objects(i915);
		err = drm_vma_offset_add(dev->vma_offset_manager, &mmo->vma_node,
					 obj->base.size / PAGE_SIZE);
		if (!err) {
			init_mmap_offset(obj, mmo);
			break;
		}

	} while (flush_delayed_work(&i915->gem.retire_work));

	return err;
}

static int
__assign_gem_object_mmap_data(struct drm_file *file,
			      u32 handle,
			      enum i915_mmap_type mmap_type,
			      u64 *offset)
{
	struct drm_i915_gem_object *obj;
	struct i915_mmap_offset *mmo;
	int ret;

	obj = i915_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;

	mmo = kzalloc(sizeof(*mmo), GFP_KERNEL);
	if (!mmo) {
		ret = -ENOMEM;
		goto err;
	}

	mmo->file = file;
	ret = create_mmap_offset(obj, mmo);
	if (ret) {
		kfree(mmo);
		goto err;
	}

	mmo->mmap_type = mmap_type;
	mmo->obj = obj;
	*offset = drm_vma_node_offset_addr(&mmo->vma_node);
err:
	i915_gem_object_put(obj);
	return ret;
}

/**
 * i915_gem_mmap_gtt_ioctl - prepare an object for GTT mmap'ing
 * @dev: DRM device
 * @data: GTT mapping ioctl data
 * @file: GEM object info
 *
 * Simply returns the fake offset to userspace so it can mmap it.
 * The mmap call will end up in drm_gem_mmap(), which will set things
 * up so we can get faults in the handler above.
 *
 * The fault handler will take care of binding the object into the GTT
 * (since it may have been evicted to make room for something), allocating
 * a fence register, and mapping the appropriate aperture address into
 * userspace.
 */
int
i915_gem_mmap_gtt_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct drm_i915_gem_mmap_offset *args = data;
	struct drm_i915_private *i915 = to_i915(dev);

	if (args->flags & I915_MMAP_OFFSET_FLAGS)
		return i915_gem_mmap_offset_ioctl(dev, data, file);

	if (!HAS_MAPPABLE_APERTURE(i915)) {
		DRM_ERROR("No aperture, cannot mmap via legacy GTT\n");
		return -ENODEV;
	}

	return __assign_gem_object_mmap_data(file, args->handle,
					     I915_MMAP_TYPE_GTT,
					     &args->offset);
}

int i915_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_i915_gem_mmap_offset *args = data;
	enum i915_mmap_type type;

	if ((args->flags & (I915_MMAP_OFFSET_WC | I915_MMAP_OFFSET_WB)) &&
	    !boot_cpu_has(X86_FEATURE_PAT))
		return -ENODEV;

	if (args->flags & I915_MMAP_OFFSET_WC)
		type = I915_MMAP_TYPE_OFFSET_WC;
	else if (args->flags & I915_MMAP_OFFSET_WB)
		type = I915_MMAP_TYPE_OFFSET_WB;
	else if (args->flags & I915_MMAP_OFFSET_UC)
		type = I915_MMAP_TYPE_OFFSET_UC;

	return __assign_gem_object_mmap_data(file, args->handle, type,
					     &args->offset);
}

void i915_mmap_offset_object_release(struct kref *ref)
{
	struct i915_mmap_offset *mmo = container_of(ref,
						    struct i915_mmap_offset,
						    ref);
	struct drm_i915_gem_object *obj = mmo->obj;
	struct drm_device *dev = obj->base.dev;

	if (mmo->file)
		drm_vma_node_revoke(&mmo->vma_node, mmo->file);
	drm_vma_offset_remove(dev->vma_offset_manager, &mmo->vma_node);
	list_del(&mmo->offset);

	kfree(mmo);
}

static void i915_gem_vm_open(struct vm_area_struct *vma)
{
	struct i915_mmap_offset *priv = vma->vm_private_data;
	struct drm_i915_gem_object *obj = priv->obj;

	i915_gem_object_get(obj);
	kref_get(&priv->ref);
}

static void i915_gem_vm_close(struct vm_area_struct *vma)
{
	struct i915_mmap_offset *priv = vma->vm_private_data;
	struct drm_i915_gem_object *obj = priv->obj;

	i915_gem_object_put(obj);
	kref_put(&priv->ref, i915_mmap_offset_object_release);
}

static const struct vm_operations_struct i915_gem_gtt_vm_ops = {
	.fault = i915_gem_fault,
	.open = i915_gem_vm_open,
	.close = i915_gem_vm_close,
};

static const struct vm_operations_struct i915_gem_cpu_vm_ops = {
	.fault = i915_gem_fault_cpu,
	.open = i915_gem_vm_open,
	.close = i915_gem_vm_close,
};

static const struct vm_operations_struct i915_gem_lmem_vm_ops = {
	.fault = i915_gem_fault_lmem,
	.open = i915_gem_vm_open,
	.close = i915_gem_vm_close,
};

static void set_vmdata_mmap_offset(struct i915_mmap_offset *mmo, struct vm_area_struct *vma)
{
	switch (mmo->mmap_type) {
	case I915_MMAP_TYPE_OFFSET_WC:
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		break;
	case I915_MMAP_TYPE_OFFSET_WB:
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		break;
	case I915_MMAP_TYPE_OFFSET_UC:
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));
		break;
	default:
		break;
	}

	if (i915_gem_object_is_lmem(mmo->obj))
		vma->vm_ops = &i915_gem_lmem_vm_ops;
	else
		vma->vm_ops = &i915_gem_cpu_vm_ops;
}

/* This overcomes the limitation in drm_gem_mmap's assignment of a
 * drm_gem_object as the vma->vm_private_data. Since we need to
 * be able to resolve multiple mmap offsets which could be tied
 * to a single gem object.
 */
int i915_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_vma_offset_node *node;
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct i915_mmap_offset *mmo = NULL;
	struct drm_gem_object *obj = NULL;

	if (drm_dev_is_unplugged(dev))
		return -ENODEV;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  vma->vm_pgoff,
						  vma_pages(vma));
	if (likely(node)) {
	        mmo = container_of(node, struct i915_mmap_offset,
				   vma_node);
		/*
		 * Take a ref for our mmap_offset and gem objects. The reference is cleaned
		 * up when the vma is closed.
		 *
		 * Skip 0-refcnted objects as it is in the process of being destroyed
		 * and will be invalid when the vma manager lock is released.
		 */
		if (kref_get_unless_zero(&mmo->ref)) {
			obj = &mmo->obj->base;
			if (!kref_get_unless_zero(&obj->refcount))
				obj = NULL;
		}
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj) {
		if (mmo)
			kref_put(&mmo->ref, i915_mmap_offset_object_release);
		return -EINVAL;
	}

	if (!drm_vma_node_is_allowed(node, priv)) {
		drm_gem_object_put_unlocked(obj);
		return -EACCES;
	}

	if (to_intel_bo(obj)->readonly) {
		if (vma->vm_flags & VM_WRITE) {
			drm_gem_object_put_unlocked(obj);
			return -EINVAL;
		}

		vma->vm_flags &= ~VM_MAYWRITE;
	}

	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);
	vma->vm_private_data = mmo;

	switch (mmo->mmap_type) {
	case I915_MMAP_TYPE_OFFSET_WC:
	case I915_MMAP_TYPE_OFFSET_WB:
	case I915_MMAP_TYPE_OFFSET_UC:
		set_vmdata_mmap_offset(mmo, vma);
		break;
	case I915_MMAP_TYPE_GTT:
		vma->vm_ops = &i915_gem_gtt_vm_ops;
		break;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_mman.c"
#endif
