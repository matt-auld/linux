/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_MEMORY_REGION_H__
#define __INTEL_MEMORY_REGION_H__

#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/io-mapping.h>

#include "i915_gem_buddy.h"

struct drm_i915_private;
struct drm_i915_gem_object;
struct intel_memory_region;
struct sg_table;

/**
 *  Base memory type
 */
enum intel_memory_type {
	INTEL_SMEM = 0,
	INTEL_LMEM,
	INTEL_STOLEN,
};

enum intel_region_id {
	INTEL_MEMORY_SMEM = 0,
	INTEL_MEMORY_LMEM,
	INTEL_MEMORY_STOLEN,
	INTEL_MEMORY_UKNOWN, /* Should be last */
};

#define REGION_SMEM     BIT(INTEL_MEMORY_SMEM)
#define REGION_LMEM     BIT(INTEL_MEMORY_LMEM)
#define REGION_STOLEN   BIT(INTEL_MEMORY_STOLEN)

#define INTEL_MEMORY_TYPE_SHIFT 16

#define MEMORY_TYPE_FROM_REGION(r) (ilog2((r) >> INTEL_MEMORY_TYPE_SHIFT))
#define MEMORY_INSTANCE_FROM_REGION(r) (ilog2((r) & 0xffff))

/**
 * Memory regions encoded as type | instance
 */
static const u32 intel_region_map[] = {
	[INTEL_MEMORY_SMEM] = BIT(INTEL_SMEM + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
	[INTEL_MEMORY_LMEM] = BIT(INTEL_LMEM + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
	[INTEL_MEMORY_STOLEN] = BIT(INTEL_STOLEN + INTEL_MEMORY_TYPE_SHIFT) | BIT(0),
};

struct intel_memory_region_ops {
	unsigned int flags;

	int (*init)(struct intel_memory_region *);
	void (*release)(struct intel_memory_region *);

	struct drm_i915_gem_object *
	(*create_object)(struct intel_memory_region *,
			 resource_size_t,
			 unsigned int);
};

struct intel_memory_region {
	struct drm_i915_private *i915;

	const struct intel_memory_region_ops *ops;

	struct io_mapping iomap;
	struct resource region;

	struct i915_gem_buddy_mm mm;
	struct mutex mm_lock;

	resource_size_t io_start;
	resource_size_t min_page_size;

	unsigned int type;
	unsigned int instance;
	unsigned int id;

	/* Protects access to objects and purgeable */
	struct mutex obj_lock;
	struct list_head objects;
	struct list_head purgeable;
};

int i915_memory_region_init_buddy(struct intel_memory_region *mem);
void i915_memory_region_release_buddy(struct intel_memory_region *mem);

int i915_memory_region_get_pages_buddy(struct drm_i915_gem_object *obj);
void i915_memory_region_put_pages_buddy(struct drm_i915_gem_object *obj,
					struct sg_table *pages);

void i915_gem_object_release_memory_region(struct drm_i915_gem_object *obj);

int i915_memory_region_shrink(struct intel_memory_region *mem,
			      resource_size_t target);

struct intel_memory_region *
intel_memory_region_create(struct drm_i915_private *i915,
			   resource_size_t start,
			   resource_size_t size,
			   resource_size_t min_page_size,
			   resource_size_t io_start,
			   const struct intel_memory_region_ops *ops);
void
intel_memory_region_destroy(struct intel_memory_region *mem);

struct drm_i915_gem_object *
i915_gem_object_create_region(struct intel_memory_region *mem,
			      resource_size_t size,
			      unsigned int flags);

#endif
