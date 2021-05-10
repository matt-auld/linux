/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/types.h>

#ifndef __I915_TTM_BUDDY_MANAGER_H__
#define __I915_TTM_BUDDY_MANAGER_H__

struct ttm_device;
struct ttm_resource_manager;

int i915_ttm_buddy_man_init(struct ttm_device *bdev,
			    unsigned type, bool use_tt,
			    unsigned long p_size,
			    u64 chunk_size);
int i915_ttm_buddy_man_fini(struct ttm_device *bdev,
			    unsigned type);

struct ttm_resource_manager *
i915_ttm_buddy_man_init_nodev(u64 p_size, u64 chunk_size, bool use_tt);
void i915_ttm_buddy_man_fini_nodev(struct ttm_resource_manager *man);

int i915_ttm_buddy_man_reserve_nodev(struct ttm_resource_manager *man, u64 start, u64 size);
int i915_ttm_buddy_man_reserve(struct ttm_device *bdev, unsigned type,
			       u64 start, u64 size);

#endif
