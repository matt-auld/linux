/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_TTM_BUDDY_MANAGER_H__
#define __I915_TTM_BUDDY_MANAGER_H__

#include <drm/ttm/ttm_resource.h>

#include <linux/list.h>
#include <linux/types.h>

struct ttm_device;
struct ttm_resource_manager;

/**
 * struct i915_ttm_buddy_resource
 *
 * @base: struct ttm_resource base class we extend
 * @blocks: the list of struct i915_buddy_block for this resource/allocation
 *
 * Extends the struct ttm_resource to manage an address space allocation with
 * one or more struct i915_buddy_block.
 */
struct i915_ttm_buddy_resource {
	struct ttm_resource base;
	struct list_head blocks;
};

/**
 * to_ttm_range_mgr_node
 *
 * @res: the resource to upcast
 *
 * Upcast the ttm_resource object into a i915_ttm_buddy_resource.
 */
static inline struct i915_ttm_buddy_resource *
to_ttm_buddy_resource(struct ttm_resource *res)
{
	return container_of(res, struct i915_ttm_buddy_resource, base);
}

int i915_ttm_buddy_man_init(struct ttm_device *bdev,
			    unsigned type, bool use_tt,
			    u64 size, u64 chunk_size);
int i915_ttm_buddy_man_fini(struct ttm_device *bdev,
			    unsigned int type);

int i915_ttm_buddy_man_reserve(struct ttm_resource_manager *man,
			       u64 start, u64 size);

#endif
