/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "i915_selftest.h"
#include "mock_drm.h"
#include "huge_gem_object.h"

#define DW_PER_PAGE (PAGE_SIZE / sizeof(u32))

static struct i915_vma *
gpu_fill_pages(struct i915_vma *vma, u64 offset, unsigned long count, u32 value)
{
	struct drm_i915_gem_object *obj;
	const int gen = INTEL_GEN(vma->vm->i915);
	unsigned long sz = (4*count + 1)*sizeof(u32);
	u32 *cmd;
	int err;

	obj = i915_gem_object_create_internal(vma->vm->i915,
					      round_up(sz, PAGE_SIZE));
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cmd)) {
		i915_gem_object_put(obj);
		return ERR_CAST(cmd);
	}

	GEM_BUG_ON(offset + (count - 1) * PAGE_SIZE > vma->node.size);
	offset += vma->node.start;

	for (sz = 0; sz < count; sz++) {
		if (gen >= 8) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = value;
		} else if (gen >= 4) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4 |
				(gen < 6 ? 1 << 22 : 0);
			*cmd++ = 0;
			*cmd++ = offset;
			*cmd++ = value;
		} else {
			*cmd++ = MI_STORE_DWORD_IMM | 1 << 22;
			*cmd++ = offset;
			*cmd++ = value;
		}
		offset += PAGE_SIZE;
	}
	*cmd = MI_BATCH_BUFFER_END;
	i915_gem_object_unpin_map(obj);

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err) {
		i915_gem_object_put(obj);
		return ERR_PTR(err);
	}

	vma = i915_vma_instance(obj, vma->vm, NULL);
	if (IS_ERR(vma)) {
		i915_gem_object_put(obj);
		return vma;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err) {
		i915_gem_object_put(obj);
		return ERR_PTR(err);
	}

	return vma;
}

static int gpu_fill(struct drm_i915_gem_object *obj,
		    struct i915_gem_context *ctx,
		    struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_address_space *vm =
		ctx->ppgtt ? &ctx->ppgtt->base : &i915->ggtt.base;
	struct i915_vma *vma;
	struct i915_vma *batch;
	unsigned long n, max;
	int err;

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		return err;

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		return err;

	GEM_BUG_ON(!IS_ALIGNED(obj->base.size >> PAGE_SHIFT, DW_PER_PAGE));
	max = (obj->base.size >> PAGE_SHIFT) / DW_PER_PAGE;
	for (n = 0; n < max; n++) {
		struct drm_i915_gem_request *rq;

		batch = gpu_fill_pages(vma,
				       ((n * DW_PER_PAGE) << PAGE_SHIFT) |
				       (n * sizeof(u32)),
				       DW_PER_PAGE,
				       n);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			goto err_vma;
		}

		rq = i915_gem_request_alloc(engine, ctx);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err_batch;
		}

		i915_switch_context(rq);
		engine->emit_bb_start(rq,
				      batch->node.start, batch->node.size, 0);

		i915_vma_move_to_active(batch, rq, 0);
		i915_gem_object_set_active_reference(batch->obj);
		i915_vma_unpin(batch);
		i915_vma_close(batch);

		i915_vma_move_to_active(vma, rq, 0);

		reservation_object_lock(obj->resv, NULL);
		reservation_object_add_excl_fence(obj->resv, &rq->fence);
		reservation_object_unlock(obj->resv);

		__i915_add_request(rq, true);
	}
	i915_vma_unpin(vma);

	return 0;

err_batch:
	i915_vma_unpin(batch);
err_vma:
	i915_vma_unpin(vma);
	return err;
}

static int cpu_fill(struct drm_i915_gem_object *obj, u32 value)
{
	const bool has_llc = HAS_LLC(to_i915(obj->base.dev));
	unsigned int n, m, need_flush;
	int err;

	err = i915_gem_obj_prepare_shmem_write(obj, &need_flush);
	if (err)
		return err;

	for (n = 0; n < DW_PER_PAGE; n++) {
		u32 *map;

		map = kmap_atomic(i915_gem_object_get_page(obj, n));
		for (m = 0; m < DW_PER_PAGE; m++)
			map[m] = value;
		if (!has_llc)
			drm_clflush_virt_range(map, PAGE_SIZE);
		kunmap_atomic(map);
	}

	i915_gem_obj_finish_shmem_access(obj);
	obj->base.read_domains = I915_GEM_DOMAIN_GTT | I915_GEM_DOMAIN_CPU;
	obj->base.write_domain = 0;
	return 0;
}

static int cpu_check(struct drm_i915_gem_object *obj,
		     unsigned long num)
{
	const unsigned int max = (obj->base.size >> PAGE_SHIFT) / DW_PER_PAGE;
	unsigned int n, m, needs_flush;
	int err;

	err = i915_gem_obj_prepare_shmem_read(obj, &needs_flush);
	if (err)
		return err;

	for (n = 0; !err && n < DW_PER_PAGE; n++) {
		u32 *map;

		map = kmap_atomic(i915_gem_object_get_page(obj, n));
		if (needs_flush & CLFLUSH_BEFORE)
			drm_clflush_virt_range(map, sizeof(u32)*max);
		for (m = 0; !err && m < max; m++) {
			if (map[m] != m) {
				pr_err("Invalid value in object %lu at page %d, offset %d: found %x expected %x\n",
				       num, n, m, map[m], m);
				err = -EINVAL;
			}
		}
		kunmap_atomic(map);
	}

	i915_gem_obj_finish_shmem_access(obj);
	return err;
}

static int igt_ctx_exec(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_file *file = mock_file(i915);
	struct drm_i915_gem_object *obj;
	IGT_TIMEOUT(end_time);
	LIST_HEAD(objects);
	unsigned int count;
	int err = 0;

	/* Create a few different contexts (with different mm) and write
	 * through each ctx/mm using the GPU making sure those writes end
	 * up in the expected pages of our obj.
	 */

	mutex_lock(&i915->drm.struct_mutex);

	count = 0;
	while (!time_after(jiffies, end_time)) {
		struct intel_engine_cs *engine;
		struct i915_gem_context *ctx;
		struct i915_address_space *vm;
		unsigned int id;

		ctx = i915_gem_create_context(i915, file->driver_priv);
		if (IS_ERR(ctx)) {
			err = PTR_ERR(ctx);
			goto err;
		}

		vm = ctx->ppgtt ? &ctx->ppgtt->base : &i915->ggtt.base;

		for_each_engine(engine, i915, id) {
			u64 npages;
			u32 handle;

			npages = min(vm->total / 2,
				     1024ull * DW_PER_PAGE * PAGE_SIZE);
			npages = round_down(npages, DW_PER_PAGE * PAGE_SIZE);
			obj = huge_gem_object(i915,
					      DW_PER_PAGE * PAGE_SIZE,
					      npages);
			if (IS_ERR(obj)) {
				err = PTR_ERR(obj);
				goto err;
			}

			/* tie the handle to the drm_file for easy reaping */
			err = drm_gem_handle_create(file, &obj->base, &handle);
			if (err) {
				i915_gem_object_put(obj);
				goto err;
			}

			err = cpu_fill(obj, 0xdeadbeef);
			if (err) {
				pr_err("Failed to fill object with cpu, err=%d\n",
				       err);
				goto err;
			}

			err = gpu_fill(obj, ctx, engine);
			if (err) {
				pr_err("Failed to fill object with gpu (%s), err=%d\n",
				       engine->name, err);
				goto err;
			}

			list_add_tail(&obj->batch_pool_link, &objects);
		}
		count++;
	}
	pr_info("Submitted %d contexts (across %u engines)\n",
		count, INTEL_INFO(i915)->num_rings);

	count = 0;
	list_for_each_entry(obj, &objects, batch_pool_link) {
		err = cpu_check(obj, count++);
		if (err)
			break;
	}

err:
	mutex_unlock(&i915->drm.struct_mutex);

	mock_file_free(i915, file);
	return err;
}

int i915_gem_context_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ctx_exec),
	};

	return i915_subtests(tests, i915);
}
