// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */
#include "i915_gem_client_blt.h"

#include "i915_gem_object_blt.h"
#include "intel_drv.h"

static struct drm_i915_gem_object *
create_sleeve(struct drm_i915_private *i915,
	      struct sg_table *pages,
	      unsigned int page_sizes,
	      u64 size)
{
	struct drm_i915_gem_object *sleeve;

	/* XXX: sketchy af */
	sleeve = i915_gem_object_create_internal(i915, size);
	if (IS_ERR(sleeve))
		return sleeve;

	mutex_lock(&sleeve->mm.lock);

	atomic_inc(&sleeve->mm.pages_pin_count);
	__i915_gem_object_set_pages(sleeve, pages, page_sizes);

	mutex_unlock(&sleeve->mm.lock);

	return sleeve;
}

static void destroy_sleeve(struct drm_i915_gem_object *sleeve)
{
	mutex_lock(&sleeve->mm.lock);

	__i915_gem_object_unset_pages(sleeve);
	atomic_dec(&sleeve->mm.pages_pin_count);

	mutex_unlock(&sleeve->mm.lock);

	i915_gem_object_put(sleeve);
}

struct clear_pages_work {
	struct dma_fence dma;
	struct i915_sw_fence wait;
	struct work_struct work;
	struct drm_i915_gem_object *sleeve;
	struct drm_i915_gem_object *obj;
	struct i915_gem_context *ctx;
	u32 value;
};

static const char *clear_pages_work_driver_name(struct dma_fence *fence)
{
	return DRIVER_NAME;
}

static const char *clear_pages_work_timeline_name(struct dma_fence *fence)
{
	return "clear";
}

static void clear_pages_work_release(struct dma_fence *fence)
{
	struct clear_pages_work *w = container_of(fence, typeof(*w), dma);

	i915_sw_fence_fini(&w->wait);

	BUILD_BUG_ON(offsetof(typeof(*w), dma));
	dma_fence_free(&w->dma);
}

static const struct dma_fence_ops clear_pages_work_ops = {
	.get_driver_name = clear_pages_work_driver_name,
	.get_timeline_name = clear_pages_work_timeline_name,
	.release = clear_pages_work_release,
};

/* XXX: needs to be taken out and shot */
static void i915_clear_pages_worker(struct work_struct *work)
{
	struct clear_pages_work *w = container_of(work, typeof(*w), work);
	struct drm_i915_private *i915 = w->ctx->i915;
	intel_wakeref_t wakeref;
	int err;

	mutex_lock(&i915->drm.struct_mutex);

	wakeref = intel_runtime_pm_get(i915);
	err = i915_gem_object_fill_blt(w->ctx, w->sleeve, w->value);
	intel_runtime_pm_put(i915, wakeref);

	if (unlikely(err))
		dma_fence_set_error(&w->dma, err);

	err = i915_gem_object_unbind(w->sleeve);
	if (unlikely(err))
		dma_fence_set_error(&w->dma, err);

	mutex_unlock(&i915->drm.struct_mutex);

	i915_gem_object_put(w->obj);
	i915_gem_context_put(w->ctx);

	destroy_sleeve(w->sleeve);

	dma_fence_signal(&w->dma);
	dma_fence_put(&w->dma);
}

static int __i915_sw_fence_call
clear_pages_work_notify(struct i915_sw_fence *fence,
			enum i915_sw_fence_notify state)
{
	struct clear_pages_work *w = container_of(fence, typeof(*w), wait);

	switch (state) {
	case FENCE_COMPLETE:
		schedule_work(&w->work);
		break;

	case FENCE_FREE:
		dma_fence_put(&w->dma);
		break;
	}

	return NOTIFY_DONE;
}

static DEFINE_SPINLOCK(fence_lock);

int i915_gem_schedule_fill_pages_blt(struct drm_i915_gem_object *obj,
				     struct i915_gem_context *ctx,
				     struct sg_table *pages,
				     unsigned int page_sizes,
				     u64 size,
				     u32 value)
{
	struct drm_i915_gem_object *sleeve;
	struct clear_pages_work *work;

	sleeve = create_sleeve(ctx->i915, pages, page_sizes, size);
	if (IS_ERR(sleeve))
		return PTR_ERR(sleeve);

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (work == NULL) {
		destroy_sleeve(sleeve);
		return -ENOMEM;
	}

	work->value = value;
	work->sleeve = sleeve;
	work->obj = i915_gem_object_get(obj);
	work->ctx = i915_gem_context_get(ctx);

	INIT_WORK(&work->work, i915_clear_pages_worker);

	dma_fence_init(&work->dma,
		       &clear_pages_work_ops,
		       &fence_lock,
		       to_i915(obj->base.dev)->mm.unordered_timeline,
		       0);
	i915_sw_fence_init(&work->wait, clear_pages_work_notify);

	dma_fence_get(&work->dma);

	i915_sw_fence_await_reservation(&work->wait,
					obj->resv, NULL,
					true, I915_FENCE_TIMEOUT,
					I915_FENCE_GFP);

	i915_gem_object_lock(obj);
	reservation_object_add_excl_fence(obj->resv, &work->dma);
	i915_gem_object_unlock(obj);

	i915_sw_fence_commit(&work->wait);

	return 0;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_client_blt.c"
#endif
