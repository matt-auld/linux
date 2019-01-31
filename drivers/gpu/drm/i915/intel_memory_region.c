/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "intel_memory_region.h"
#include "i915_drv.h"

int i915_memory_region_shrink(struct intel_memory_region *mem,
			      resource_size_t target)
{
	struct drm_i915_gem_object *obj, *on;
	resource_size_t found;
	LIST_HEAD(purgeable);
	int err;

	err = 0;
	found = 0;

	mutex_lock(&mem->obj_lock);

	list_for_each_entry(obj, &mem->purgeable, region_link) {
		if (!i915_gem_object_has_pages(obj))
			continue;

		if (READ_ONCE(obj->pin_global))
			continue;

		if (atomic_read(&obj->mm.pages_pin_count) > obj->bind_count)
			continue;

		list_add(&obj->tmp_link, &purgeable);

		found += obj->base.size;
		if (found >= target)
			goto found;
	}

	err = -ENOSPC;
found:
	list_for_each_entry_safe(obj, on, &purgeable, tmp_link) {
		if (!err) {
			/* XXX: at some point also try to unbind the object */
			__i915_gem_object_put_pages(obj, I915_MM_SHRINKER);

			mutex_lock(&obj->mm.lock);
			if (!i915_gem_object_has_pages(obj))
				obj->mm.madv = __I915_MADV_PURGED;
			mutex_unlock(&obj->mm.lock);
		}

		list_del(&obj->tmp_link);
	}

	mutex_unlock(&mem->obj_lock);

	return err;
}

static void
memory_region_free_pages(struct drm_i915_gem_object *obj,
			 struct sg_table *pages)
{
	struct i915_gem_buddy_block *block, *on;

	lockdep_assert_held(&obj->memory_region->mm_lock);

	list_for_each_entry_safe(block, on, &obj->blocks, link) {
		list_del_init(&block->link);
		i915_gem_buddy_free(&obj->memory_region->mm, block);
	}

	sg_free_table(pages);
	kfree(pages);
}

void
i915_memory_region_put_pages_buddy(struct drm_i915_gem_object *obj,
				   struct sg_table *pages)
{
	mutex_lock(&obj->memory_region->mm_lock);
	memory_region_free_pages(obj, pages);
	mutex_unlock(&obj->memory_region->mm_lock);

	obj->mm.dirty = false;
}

int
i915_memory_region_get_pages_buddy(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mem = obj->memory_region;
	resource_size_t size = obj->base.size;
	unsigned int flags = obj->flags;
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned int sg_page_sizes;
	unsigned long n_pages;

	GEM_BUG_ON(!IS_ALIGNED(size, mem->mm.min_size));
	GEM_BUG_ON(!list_empty(&obj->blocks));

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	n_pages = size >> ilog2(mem->mm.min_size);

	if (sg_alloc_table(st, n_pages, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	sg = st->sgl;
	st->nents = 0;
	sg_page_sizes = 0;

	mutex_lock(&mem->mm_lock);

	do {
		struct i915_gem_buddy_block *block;
		unsigned int order;
		u64 block_size;
		u64 offset;
		bool retry = true;
retry:
		order = fls(n_pages) - 1;
		GEM_BUG_ON(order > mem->mm.max_order);

		do {
			block = i915_gem_buddy_alloc(&mem->mm, order);
			if (!IS_ERR(block))
				break;

			if (flags & I915_BO_ALLOC_CONTIGUOUS || !order--) {
				resource_size_t target;
				int err;

				if (!retry)
					goto err_free_blocks;

				target = n_pages * mem->mm.min_size;

				mutex_unlock(&mem->mm_lock);
				err = i915_memory_region_shrink(mem, target);
				mutex_lock(&mem->mm_lock);
				if (err)
					goto err_free_blocks;

				retry = false;
				goto retry;
			}
		} while (1);

		n_pages -= BIT(order);

		INIT_LIST_HEAD(&block->link);
		list_add(&block->link, &obj->blocks);

		block_size = i915_gem_buddy_block_size(&mem->mm, block);
		offset = i915_gem_buddy_block_offset(block);

		sg_dma_address(sg) = mem->region.start + offset;
		sg_dma_len(sg) = block_size;

		sg->length = block_size;
		sg_page_sizes |= block_size;
		st->nents++;

		if (!n_pages) {
			sg_mark_end(sg);
			break;
		}

		sg = __sg_next(sg);
	} while (1);

	mutex_unlock(&mem->mm_lock);

	i915_sg_trim(st);

	if (flags & I915_BO_ALLOC_VOLATILE)
		obj->mm.madv = I915_MADV_DONTNEED;

	__i915_gem_object_set_pages(obj, st, sg_page_sizes);

	return 0;

err_free_blocks:
	memory_region_free_pages(obj, st);
	mutex_unlock(&mem->mm_lock);
	return -ENOSPC;
}

int i915_memory_region_init_buddy(struct intel_memory_region *mem)
{
	return i915_gem_buddy_init(&mem->mm, resource_size(&mem->region),
				   mem->min_page_size);
}

void i915_memory_region_release_buddy(struct intel_memory_region *mem)
{
	i915_gem_buddy_fini(&mem->mm);
}

void i915_gem_object_release_memory_region(struct drm_i915_gem_object *obj)
{
	mutex_lock(&obj->memory_region->obj_lock);
	list_del(&obj->region_link);
	mutex_unlock(&obj->memory_region->obj_lock);
}

struct drm_i915_gem_object *
i915_gem_object_create_region(struct intel_memory_region *mem,
			      resource_size_t size,
			      unsigned int flags)
{
	struct drm_i915_gem_object *obj;

	if (!mem)
		return ERR_PTR(-ENODEV);

	if (flags & ~I915_BO_ALLOC_FLAGS)
		return ERR_PTR(-EINVAL);

	size = round_up(size, mem->min_page_size);

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, I915_GTT_MIN_ALIGNMENT));

	/*
	 * There is a prevalence of the assumption that we fit the object's
	 * page count inside a 32bit _signed_ variable. Let's document this and
	 * catch if we ever need to fix it. In the meantime, if you do spot
	 * such a local variable, please consider fixing!
	 */

	if (size >> PAGE_SHIFT > INT_MAX)
		return ERR_PTR(-E2BIG);

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = mem->ops->create_object(mem, size, flags);
	if (IS_ERR(obj))
		return obj;

	INIT_LIST_HEAD(&obj->blocks);
	obj->memory_region = mem;
	obj->flags = flags;

	mutex_lock(&mem->obj_lock);

	if (flags & I915_BO_ALLOC_VOLATILE)
		list_add(&obj->region_link, &mem->purgeable);
	else
		list_add(&obj->region_link, &mem->objects);

	mutex_unlock(&mem->obj_lock);

	i915_gem_object_set_cache_coherency(obj, obj->cache_level);

	trace_i915_gem_object_create(obj);

	return obj;
}

struct intel_memory_region *
intel_memory_region_create(struct drm_i915_private *i915,
			   resource_size_t start,
			   resource_size_t size,
			   resource_size_t min_page_size,
			   resource_size_t io_start,
			   const struct intel_memory_region_ops *ops)
{
	struct intel_memory_region *mem;
	int err;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	mem->i915 = i915;
	mem->region = (struct resource)DEFINE_RES_MEM(start, size);
	mem->io_start = io_start;
	mem->min_page_size = min_page_size;
	mem->ops = ops;

	mutex_init(&mem->obj_lock);
	INIT_LIST_HEAD(&mem->objects);
	INIT_LIST_HEAD(&mem->purgeable);

	mutex_init(&mem->mm_lock);

	if (ops->init) {
		err = ops->init(mem);
		if (err) {
			kfree(mem);
			mem = ERR_PTR(err);
		}
	}

	return mem;
}

void
intel_memory_region_destroy(struct intel_memory_region *mem)
{
	if (mem->ops->release)
		mem->ops->release(mem);

	kfree(mem);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/mock_region.c"
#endif
