// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "intel_memory_region.h"
#include "i915_drv.h"

const u32 intel_region_map[] = {
	[INTEL_MEMORY_SMEM] = BIT(INTEL_SMEM + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
	[INTEL_MEMORY_LMEM] = BIT(INTEL_LMEM + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
	[INTEL_MEMORY_STOLEN] = BIT(INTEL_STOLEN + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
};

static int
intel_memory_region_evict(struct intel_memory_region *mem,
			  resource_size_t target,
			  unsigned int flags)
{
	struct drm_i915_gem_object *obj;
	resource_size_t found;
	int err;

	err = 0;
	found = 0;

	mutex_lock(&mem->obj_lock);
	list_for_each_entry(obj, &mem->purgeable, mm.region_link) {
		if (!i915_gem_object_has_pages(obj))
			continue;

		if (READ_ONCE(obj->pin_global))
			continue;

		if (atomic_read(&obj->bind_count))
			continue;

		mutex_unlock(&mem->obj_lock);

		__i915_gem_object_put_pages(obj, I915_MM_SHRINKER);

		mutex_lock_nested(&obj->mm.lock, I915_MM_SHRINKER);
		if (!i915_gem_object_has_pages(obj)) {
			obj->mm.madv = __I915_MADV_PURGED;
			found += obj->base.size;
		}
		mutex_unlock(&obj->mm.lock);

		if (found >= target)
			return 0;

		mutex_lock(&mem->obj_lock);
	}

	err = -ENOSPC;
	mutex_unlock(&mem->obj_lock);
	return err;
}

static u64
intel_memory_region_free_pages(struct intel_memory_region *mem,
			       struct list_head *blocks)
{
	struct i915_buddy_block *block, *on;
	u64 size = 0;

	list_for_each_entry_safe(block, on, blocks, link) {
		size += i915_buddy_block_size(&mem->mm, block);
		i915_buddy_free(&mem->mm, block);
	}
	INIT_LIST_HEAD(blocks);

	return size;
}

void
__intel_memory_region_put_pages_buddy(struct intel_memory_region *mem,
				      struct list_head *blocks)
{
	mutex_lock(&mem->mm_lock);
	intel_memory_region_free_pages(mem, blocks);
	mutex_unlock(&mem->mm_lock);
}

void
__intel_memory_region_put_block_buddy(struct i915_buddy_block *block)
{
	struct list_head blocks;

	INIT_LIST_HEAD(&blocks);
	list_add(&block->link, &blocks);
	__intel_memory_region_put_pages_buddy(block->private, &blocks);
}

int
__intel_memory_region_get_pages_buddy(struct intel_memory_region *mem,
				      resource_size_t size,
				      unsigned int flags,
				      struct list_head *blocks)
{
	unsigned long n_pages = size >> ilog2(mem->mm.chunk_size);

	GEM_BUG_ON(!IS_ALIGNED(size, mem->mm.chunk_size));
	GEM_BUG_ON(!list_empty(blocks));

	mutex_lock(&mem->mm_lock);

	do {
		struct i915_buddy_block *block;
		unsigned int order;
		bool retry = true;
retry:
		order = fls(n_pages) - 1;
		GEM_BUG_ON(order > mem->mm.max_order);

		do {
			block = i915_buddy_alloc(&mem->mm, order);
			if (!IS_ERR(block))
				break;

			if (flags & I915_ALLOC_CONTIGUOUS || !order--) {
				resource_size_t target;
				int err;

				if (!retry)
					goto err_free_blocks;

				target = n_pages * mem->mm.chunk_size;

				mutex_unlock(&mem->mm_lock);
				err = intel_memory_region_evict(mem, target, 0);
				mutex_lock(&mem->mm_lock);
				if (err)
					goto err_free_blocks;

				retry = false;
				goto retry;
			}
		} while (1);

		n_pages -= BIT(order);

		block->private = mem;
		list_add(&block->link, blocks);

		if (!n_pages)
			break;
	} while (1);

	mutex_unlock(&mem->mm_lock);
	return 0;

err_free_blocks:
	intel_memory_region_free_pages(mem, blocks);
	mutex_unlock(&mem->mm_lock);
	return -ENXIO;
}

struct i915_buddy_block *
__intel_memory_region_get_block_buddy(struct intel_memory_region *mem,
				      resource_size_t size)
{
	struct i915_buddy_block *block;
	struct list_head blocks;
	int ret;

	INIT_LIST_HEAD(&blocks);
	ret = __intel_memory_region_get_pages_buddy(mem, size,
						    I915_ALLOC_CONTIGUOUS,
						    &blocks);
	if (ret)
		return ERR_PTR(ret);

	block = list_first_entry(&blocks, typeof(*block), link);
	list_del_init(&block->link);
	return block;
}

int intel_memory_region_init_buddy(struct intel_memory_region *mem)
{
	return i915_buddy_init(&mem->mm, resource_size(&mem->region),
			       mem->min_page_size);
}

void intel_memory_region_release_buddy(struct intel_memory_region *mem)
{
	i915_buddy_fini(&mem->mm);
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
#include "selftests/intel_memory_region.c"
#include "selftests/mock_region.c"
#endif
