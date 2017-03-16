#include "fake_dma_object.h"

static void fake_free_pages(struct drm_i915_gem_object *obj,
			    struct sg_table *pages)
{
	sg_free_table(pages);
	kfree(pages);
}

static struct sg_table *
fake_get_pages(struct drm_i915_gem_object *obj)
{
#define GFP (GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY)
#define PFN_BIAS 0x1000
	struct sg_table *pages;
	struct scatterlist *sg;
	typeof(obj->base.size) rem;

	pages = kmalloc(sizeof(*pages), GFP);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	rem = round_up(obj->base.size, BIT(31)) >> 31;
	if (sg_alloc_table(pages, rem, GFP)) {
		kfree(pages);
		return ERR_PTR(-ENOMEM);
	}

	rem = obj->base.size;
	for (sg = pages->sgl; sg; sg = sg_next(sg)) {
		unsigned long len = min_t(typeof(rem), rem, BIT(31));

		GEM_BUG_ON(!len);
		sg_set_page(sg, pfn_to_page(PFN_BIAS), len, 0);
		sg_dma_address(sg) = page_to_phys(sg_page(sg));
		sg_dma_len(sg) = len;

		rem -= len;
	}
	GEM_BUG_ON(rem);

	obj->mm.madv = I915_MADV_DONTNEED;
	return pages;
#undef PFN_BIAS
#undef GFP
}

static void fake_put_pages(struct drm_i915_gem_object *obj,
			   struct sg_table *pages)
{
	fake_free_pages(obj, pages);
	obj->mm.dirty = false;
	obj->mm.madv = I915_MADV_WILLNEED;
}

static const struct drm_i915_gem_object_ops fake_ops = {
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE,
	.get_pages = fake_get_pages,
	.put_pages = fake_put_pages,
};

struct drm_i915_gem_object *
fake_dma_object(struct drm_i915_private *i915, u64 size, unsigned int page_size)
{
	struct drm_i915_gem_object *obj;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!is_valid_gtt_page_size(page_size));

	size = round_up(size, page_size);

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc(i915);
	if (!obj)
		goto err;

	drm_gem_private_object_init(&i915->drm, &obj->base, size);

	i915_gem_object_init(obj, &fake_ops);

	obj->page_size = page_size;

	GEM_BUG_ON(!IS_ALIGNED(obj->base.size, obj->page_size));

	obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	obj->base.read_domains = I915_GEM_DOMAIN_CPU;
	obj->cache_level = I915_CACHE_NONE;

	/* Preallocate the "backing storage" */
	if (i915_gem_object_pin_pages(obj))
		goto err_obj;

	i915_gem_object_unpin_pages(obj);
	return obj;

err_obj:
	i915_gem_object_put(obj);
err:
	return ERR_PTR(-ENOMEM);
}

