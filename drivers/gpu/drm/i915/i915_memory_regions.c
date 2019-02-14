// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_memory_regions.h"

int i915_memory_regions_hw_probe(struct drm_i915_private *i915)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(i915->mm.regions); i++) {
		struct intel_memory_region *mem = ERR_PTR(-ENODEV);
		u32 type;

		if (!HAS_REGION(i915, BIT(i)))
			continue;

		type = MEMORY_TYPE_FROM_REGION(intel_region_map[i]);
		switch (type) {
		case INTEL_MEMORY_SYSTEM:
			mem = i915_gem_shmem_setup(i915);
			break;
		case INTEL_MEMORY_STOLEN:
			mem = i915_gem_stolen_setup(i915);
			break;
		case INTEL_MEMORY_LOCAL:
			mem = intel_setup_fake_lmem(i915);
			break;
		}

		if (IS_ERR(mem)) {
			err = PTR_ERR(mem);
			DRM_ERROR("Failed to setup region(%d) type=%d\n", err, type);
			goto out_cleanup;
		}

		mem->id = intel_region_map[i];
		mem->type = type;
		mem->instance = MEMORY_INSTANCE_FROM_REGION(intel_region_map[i]);

		i915->mm.regions[i] = mem;
	}

	return 0;

out_cleanup:
	i915_memory_regions_driver_release(i915);
	return err;
}

void i915_memory_regions_driver_release(struct drm_i915_private *i915)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(i915->mm.regions); i++) {
		struct intel_memory_region *region =
			fetch_and_zero(&i915->mm.regions[i]);

		if (region)
			intel_memory_region_put(region);
	}
}
