/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "../i915_selftest.h"

#include "mock_gem_device.h"
#include "mock_context.h"
#include "mock_drm.h"

typedef int (*cpu_check_fn_t)(struct drm_i915_gem_object *obj,
			      u32 __iomem *base, u32 dword, u32 val);

static void close_objects(struct list_head *objects)
{
	struct drm_i915_gem_object *obj, *on;

	list_for_each_entry_safe(obj, on, objects, st_link) {
		if (i915_gem_object_has_pinned_pages(obj))
			i915_gem_object_unpin_pages(obj);
		/* No polluting the memory region between tests */
		__i915_gem_object_put_pages(obj, I915_MM_NORMAL);
		i915_gem_object_put(obj);
		list_del(&obj->st_link);
	}
}

static int igt_mock_fill(void *arg)
{
	struct intel_memory_region *mem = arg;
	resource_size_t total = resource_size(&mem->region);
	resource_size_t page_size;
	resource_size_t rem;
	unsigned long max_pages;
	unsigned long page_num;
	LIST_HEAD(objects);
	int err = 0;

	page_size = mem->mm.min_size;
	max_pages = total / page_size;
	rem = total;

	for_each_prime_number_from(page_num, 1, max_pages) {
		resource_size_t size = page_num * page_size;
		struct drm_i915_gem_object *obj;

		obj = i915_gem_object_create_region(mem, size, 0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			break;
		}

		err = i915_gem_object_pin_pages(obj);
		if (err) {
			i915_gem_object_put(obj);
			break;
		}

		list_add(&obj->st_link, &objects);
		rem -= size;
	}

	if (err == -ENOMEM)
		err = 0;
	if (err == -ENOSPC) {
		if (page_num * page_size <= rem) {
			pr_err("%s failed, space still left in region\n",
				__func__);
			err = -EINVAL;
		} else {
			err = 0;
		}
	}

	close_objects(&objects);

	return err;
}

static void igt_mark_evictable(struct drm_i915_gem_object *obj)
{
	if (i915_gem_object_has_pinned_pages(obj))
		i915_gem_object_unpin_pages(obj);
	obj->mm.madv = I915_MADV_DONTNEED;
	list_move(&obj->region_link, &obj->memory_region->purgeable);
}

static int igt_frag_region(struct intel_memory_region *mem,
			   struct list_head *objects)
{
	struct drm_i915_gem_object *obj;
	unsigned long n_objects;
	resource_size_t target;
	resource_size_t total;
	int err = 0;

	target = mem->mm.min_size;
	total = resource_size(&mem->region);
	n_objects = total / target;

	while (n_objects--) {
		obj = i915_gem_object_create_region(mem,
						    target,
						    0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err_close_objects;
		}

		list_add(&obj->st_link, objects);

		err = i915_gem_object_pin_pages(obj);
		if (err)
			goto err_close_objects;

		/*
		 * Make half of the region evictable, though do so in a
		 * horribly fragmented fashion.
		 */
		if (n_objects % 2)
			igt_mark_evictable(obj);
	}

	return 0;

err_close_objects:
	close_objects(objects);
	return err;
}

static void igt_defrag_region(struct list_head *objects)
{
	struct drm_i915_gem_object *obj;

	list_for_each_entry(obj, objects, st_link) {
		if (obj->mm.madv == I915_MADV_WILLNEED)
			igt_mark_evictable(obj);
	}
}

static int igt_mock_shrink(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	LIST_HEAD(objects);
	resource_size_t target;
	resource_size_t total;
	int err;

	err = igt_frag_region(mem, &objects);
	if (err)
		return err;

	total = resource_size(&mem->region);
	target = mem->mm.min_size;

	while (target <= total / 2) {
		obj = i915_gem_object_create_region(mem, target, 0);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto err_close_objects;
		}

		list_add(&obj->st_link, &objects);

		/* Provoke the shrinker to start violently swinging its axe! */
		err = i915_gem_object_pin_pages(obj);
		if (err) {
			pr_err("failed to shrink for target=%pa", &target);
			goto err_close_objects;
		}

		/* Again, half of the region should remain evictable */
		igt_mark_evictable(obj);

		target <<= 1;
	}

err_close_objects:
	close_objects(&objects);

	if (err == -ENOMEM)
		err = 0;

	return err;
}

static int igt_mock_continuous(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	LIST_HEAD(objects);
	resource_size_t target;
	resource_size_t total;
	int err;

	err = igt_frag_region(mem, &objects);
	if (err)
		return err;

	total = resource_size(&mem->region);
	target = total / 2;

	/*
	 * Sanity check that we can allocate all of the available fragmented
	 * space.
	 */
	obj = i915_gem_object_create_region(mem, target, 0);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate available space\n");
		goto err_close_objects;
	}

	igt_mark_evictable(obj);

	/* Try the smallest possible size -- should succeed */
	obj = i915_gem_object_create_region(mem, mem->mm.min_size,
					    I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate smallest possible size\n");
		goto err_close_objects;
	}

	igt_mark_evictable(obj);

	if (obj->mm.pages->nents != 1) {
		pr_err("[1]object spans multiple sg entries\n");
		err = -EINVAL;
		goto err_close_objects;
	}

	/*
	 * Even though there is enough free space for the allocation, we
	 * shouldn't be able to allocate it, given that it is fragmented, and
	 * non-continuous.
	 */
	obj = i915_gem_object_create_region(mem, target, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (!err) {
		pr_err("expected allocation to fail\n");
		err = -EINVAL;
		goto err_close_objects;
	}

	igt_defrag_region(&objects);

	/* Should now succeed */
	obj = i915_gem_object_create_region(mem, target, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_close_objects;
	}

	list_add(&obj->st_link, &objects);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		pr_err("failed to allocate from defraged area\n");
		goto err_close_objects;
	}

	if (obj->mm.pages->nents != 1) {
		pr_err("object spans multiple sg entries\n");
		err = -EINVAL;
	}

err_close_objects:
	close_objects(&objects);

	return err;
}

static int igt_mock_volatile(void *arg)
{
	struct intel_memory_region *mem = arg;
	struct drm_i915_gem_object *obj;
	int err;

	obj = i915_gem_object_create_region(mem, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto err_put;

	i915_gem_object_unpin_pages(obj);

	err = i915_memory_region_shrink(mem, PAGE_SIZE);
	if (err != -ENOSPC) {
		pr_err("shrink memory region\n");
		goto err_put;
	}

	obj = i915_gem_object_create_region(mem, PAGE_SIZE, I915_BO_ALLOC_VOLATILE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	if (!(obj->flags & I915_BO_ALLOC_VOLATILE)) {
		pr_err("missing flags\n");
		goto err_put;
	}

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto err_put;

	i915_gem_object_unpin_pages(obj);

	err = i915_memory_region_shrink(mem, PAGE_SIZE);
	if (err) {
		pr_err("failed to shrink memory\n");
		goto err_put;
	}

	if (i915_gem_object_has_pages(obj)) {
		pr_err("object pages not discarded\n");
		err = -EINVAL;
	}

err_put:
	i915_gem_object_put(obj);
	return err;
}

static struct i915_vma *
igt_gpu_write_dw(struct i915_vma *vma, u64 offset, u32 val)
{
	struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);
	const int gen = INTEL_GEN(vma->vm->i915);
	unsigned int count = vma->size >> PAGE_SHIFT;
	struct drm_i915_gem_object *obj;
	struct i915_vma *batch;
	unsigned int size;
	u32 *cmd;
	int n;
	int err;

	size = (1 + 4 * count) * sizeof(u32);
	size = round_up(size, PAGE_SIZE);
	obj = i915_gem_object_create_internal(i915, size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	offset += vma->node.start;

	for (n = 0; n < count; n++) {
		if (gen >= 8) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = val;
		} else if (gen >= 4) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4 |
				(gen < 6 ? 1 << 22 : 0);
			*cmd++ = 0;
			*cmd++ = offset;
			*cmd++ = val;
		} else {
			*cmd++ = MI_STORE_DWORD_IMM | 1 << 22;
			*cmd++ = offset;
			*cmd++ = val;
		}

		offset += PAGE_SIZE;
	}

	*cmd = MI_BATCH_BUFFER_END;

	i915_gem_object_unpin_map(obj);

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		goto err;

	batch = i915_vma_instance(obj, vma->vm, NULL);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto err;
	}

	err = i915_vma_pin(batch, 0, 0, PIN_USER);
	if (err)
		goto err;

	return batch;

err:
	i915_gem_object_put(obj);

	return ERR_PTR(err);
}

static int igt_gpu_write(struct i915_vma *vma,
			 struct i915_gem_context *ctx,
			 struct intel_engine_cs *engine,
			 u32 dword,
			 u32 value)
{
	struct i915_request *rq;
	struct i915_vma *batch;
	int flags = 0;
	int err;

	GEM_BUG_ON(!intel_engine_can_store_dword(engine));

	err = i915_gem_object_set_to_gtt_domain(vma->obj, true);
	if (err)
		return err;

	rq = i915_request_alloc(engine, ctx);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	batch = igt_gpu_write_dw(vma, dword * sizeof(u32), value);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto err_request;
	}

	err = i915_vma_move_to_active(batch, rq, 0);
	i915_vma_unpin(batch);
	i915_vma_close(batch);
	if (err)
		goto err_request;

	i915_gem_object_set_active_reference(batch->obj);

	err = engine->emit_bb_start(rq,
				    batch->node.start, batch->node.size,
				    flags);
	if (err)
		goto err_request;

	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (err)
		i915_request_skip(rq, err);

err_request:
	i915_request_add(rq);

	return err;
}

static int igt_cpu_check(struct drm_i915_gem_object *obj,
			 u32 __iomem *base,
			 u32 dword, u32 val)
{
	unsigned long n;
	int err;

	err = i915_gem_object_set_to_wc_domain(obj, false);
	if (err)
		return err;

	err = i915_gem_object_pin_pages(obj);
	if (err)
		return err;

	for (n = 0; n < obj->base.size >> PAGE_SHIFT; ++n) {
		u32 __iomem *base;
		u32 read_val;

		base = i915_gem_object_lmem_io_map_page(obj, n);

		read_val = ioread32(base + dword);
		io_mapping_unmap_atomic(base);
		if (read_val != val) {
			pr_err("n=%lu base[%u]=%u, val=%u\n",
			       n, dword, read_val, val);
			err = -EINVAL;
			break;
		}
	}

	i915_gem_object_unpin_pages(obj);
	return err;
}

static int igt_gpu_fill(struct i915_gem_context *ctx,
			struct drm_i915_gem_object *obj,
			cpu_check_fn_t cpu_check,
			u32 __iomem *base)
{
	struct drm_i915_private *i915 = ctx->i915;
	struct i915_address_space *vm = ctx->ppgtt ? &ctx->ppgtt->vm : &i915->ggtt.vm;
	struct i915_vma *vma;
	struct rnd_state prng;
	u32 dword;
	int err;

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err) {
		i915_vma_close(vma);
		return err;
	}

	prandom_seed_state(&prng, i915_selftest.random_seed);
	for (dword = 0; dword < PAGE_SIZE / sizeof(u32); ++dword) {
		u32 val = prandom_u32_state(&prng);
		err = igt_gpu_write(vma, ctx, i915->engine[RCS], dword, val);
		if (err)
			break;

		err = cpu_check(obj, base, dword, val);
		if (err)
			break;
	}

	i915_vma_unpin(vma);
	i915_vma_close(vma);

	return err;
}

static int igt_lmem_create(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	int err = 0;

	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto out_put;

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_smem_create_migrate(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	int err = 0;

	/* Switch object backing-store on create */
	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	err = i915_gem_object_migrate(ctx, obj, INTEL_MEMORY_SMEM);
	if (err)
		goto out_put;

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto out_put;

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_lmem_create_migrate(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	int err = 0;

	/* Switch object backing-store on create */
	obj = i915_gem_object_create(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	err = i915_gem_object_migrate(ctx, obj, INTEL_MEMORY_LMEM);
	if (err)
		goto out_put;

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto out_put;

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}
static int igt_lmem_write_gpu(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	int err = 0;

	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto out_put;

	err = igt_gpu_fill(ctx, obj, igt_cpu_check, NULL);
	if (err) {
		pr_err("igt_gpu_fill failed(%d)\n", err);
		goto out_unpin;
	}

out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_lmem_cpu_check(struct drm_i915_gem_object *obj,
			      u32 __iomem *base, u32 dword, u32 val)
{
	u32 read_val;
	int err;

	err = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_LOCKED,
				   MAX_SCHEDULE_TIMEOUT);
	if (err)
		return err;

	err = i915_gem_object_pin_pages(obj);
	if (err)
		return err;

	read_val = ioread32(base + dword);
	if (read_val != val) {
		pr_err("base[%u]=0x%x, val=0x%x\n",
		       dword, read_val, val);
		return -EINVAL;
	}

	i915_gem_object_unpin_pages(obj);
	return 0;
}

static int igt_lmem_write_cpu(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	struct rnd_state prng;
	u32 __iomem *vaddr;
	u32 dword;
	int ret = 0;

	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		goto out_put;

	vaddr = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(vaddr)) {
		pr_err("Failed to iomap lmembar; err=%d\n", (int)PTR_ERR(vaddr));
		ret = PTR_ERR(vaddr);
		goto out_unpin;
	}

	/* gpu write/cpu read */
	ret = igt_gpu_fill(ctx, obj, igt_lmem_cpu_check, vaddr);
	if (ret) {
		pr_err("igt_gpu_fill failed(%d)\n", ret);
		goto out_unpin;
	}

	/* cpu write/cpu read */
	prandom_seed_state(&prng, i915_selftest.random_seed);
	for (dword = 0; dword < PAGE_SIZE / sizeof(u32); ++dword) {
		u32 read_val;
		u32 val = prandom_u32_state(&prng);

		iowrite32(val, vaddr + dword);
		wmb();

		read_val = ioread32(vaddr + dword);
		if (read_val != val) {
			pr_err("base[%u]=%u, val=%u\n", dword, read_val, val);
			ret = -EINVAL;
			break;
		}
	}

	i915_gem_object_unpin_map(obj);

out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);
	return ret;
}

static int igt_lmem_pages_migrate(void *arg)
{
	struct i915_gem_context *ctx = arg;
	struct drm_i915_private *i915 = ctx->i915;
	struct drm_i915_gem_object *obj;
	int err;
	int i;

	/* From LMEM to shmem and back again */

	obj = i915_gem_object_create_lmem(i915, SZ_2M, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_clear_blt(ctx, obj);
	if (err)
		goto out_put;

	for (i = 1; i <= 4; ++i) {
		err = i915_gem_object_prepare_move(obj);
		if (err)
			goto out_put;

		if (i915_gem_object_is_lmem(obj)) {
			err = i915_gem_object_migrate(ctx, obj, INTEL_MEMORY_SMEM);
			if (err)
				goto out_put;

			if (i915_gem_object_is_lmem(obj)) {
				pr_err("object still backed by lmem\n");
				err = -EINVAL;
			}

			if (!list_empty(&obj->blocks)) {
				pr_err("object leaking memory region\n");
				err = -EINVAL;
			}

			if (!i915_gem_object_has_struct_page(obj)) {
				pr_err("object not backed by struct page\n");
				err = -EINVAL;
			}

		} else {
			err = i915_gem_object_migrate(ctx, obj, INTEL_MEMORY_LMEM);
			if (err)
				goto out_put;

			if (i915_gem_object_has_struct_page(obj)) {
				pr_err("object still backed by struct page\n");
				err = -EINVAL;
			}

			if (!i915_gem_object_is_lmem(obj)) {
				pr_err("object not backed by lmem\n");
				err = -EINVAL;
			}
		}

		if (err)
			break;

		err = i915_gem_object_fill_blt(ctx, obj, 0xdeadbeaf);
		if (err)
			break;
	}

out_put:
	i915_gem_object_put(obj);

	return err;
}

int intel_memory_region_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_mock_fill),
		SUBTEST(igt_mock_shrink),
		SUBTEST(igt_mock_continuous),
		SUBTEST(igt_mock_volatile),
	};
	struct intel_memory_region *mem;
	struct drm_i915_private *i915;
	int err;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	mem = mock_region_create(i915, 0, SZ_2G,
				 I915_GTT_PAGE_SIZE_4K, 0);
	if (IS_ERR(mem)) {
		pr_err("failed to create memory region\n");
		err = PTR_ERR(mem);
		goto out_unref;
	}

	mutex_lock(&i915->drm.struct_mutex);
	err = i915_subtests(tests, mem);
	mutex_unlock(&i915->drm.struct_mutex);

	i915_gem_drain_freed_objects(i915);
	intel_memory_region_destroy(mem);

out_unref:
	drm_dev_put(&i915->drm);

	return err;
}

int intel_memory_region_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_create),
		SUBTEST(igt_smem_create_migrate),
		SUBTEST(igt_lmem_create_migrate),
		SUBTEST(igt_lmem_write_gpu),
		SUBTEST(igt_lmem_write_cpu),
		SUBTEST(igt_lmem_pages_migrate),
	};
	struct i915_gem_context *ctx;
	struct drm_file *file;
	intel_wakeref_t wakeref;
	int err;

	if (!HAS_LMEM(i915)) {
		pr_info("device lacks LMEM support, skipping\n");
		return 0;
	}

	if (i915_terminally_wedged(&i915->gpu_error))
		return 0;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	mutex_lock(&i915->drm.struct_mutex);
	wakeref = intel_runtime_pm_get(i915);

	ctx = live_context(i915, file);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto out_unlock;
	}

	err = i915_subtests(tests, ctx);

out_unlock:
	intel_runtime_pm_put(i915, wakeref);
	mutex_unlock(&i915->drm.struct_mutex);

	mock_file_free(i915, file);

	return err;
}
