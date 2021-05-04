/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

/*
 * This files contains functionality that we might want to move into
 * ttm_bo_util.c if there is a common interest.
 */
#ifndef _I915_GEM_TTM_BO_UTIL_H_
#define _I915_GEM_TTM_BO_UTIL_H_

#include <drm/ttm/ttm_bo_driver.h>
struct dma_buf_map;
struct io_mapping;
struct sg_table;
struct scatterlist;

struct ttm_tt;
struct ttm_kmap_iter;

struct ttm_kmap_iter_ops {
	void (*kmap_local)(struct ttm_kmap_iter *res_kmap,
			   struct dma_buf_map *dmap, pgoff_t i);
};

struct ttm_kmap_iter {
	const struct ttm_kmap_iter_ops *ops;
};

struct ttm_kmap_iter_tt {
	struct ttm_kmap_iter base;
	struct ttm_tt *tt;
};

struct ttm_kmap_iter_iomap {
	struct ttm_kmap_iter base;
	struct io_mapping *iomap;
	struct sg_table *st;
	resource_size_t start;
	struct {
		struct scatterlist *sg;
		pgoff_t i;
		pgoff_t end;
		pgoff_t offs;
	} cache;
};

extern struct ttm_kmap_iter_ops ttm_kmap_iter_tt_ops;
extern struct ttm_kmap_iter_ops ttm_kmap_iter_io_ops;

static inline struct ttm_kmap_iter *
ttm_kmap_iter_iomap_init(struct ttm_kmap_iter_iomap *iter_io,
			 struct io_mapping *iomap,
			 struct sg_table *st,
			 resource_size_t start)
{
	iter_io->base.ops = &ttm_kmap_iter_io_ops;
	iter_io->iomap = iomap;
	iter_io->st = st;
	iter_io->start = start;
	memset(&iter_io->cache, 0, sizeof(iter_io->cache));
	return &iter_io->base;
}

static inline struct ttm_kmap_iter *
ttm_kmap_iter_tt_init(struct ttm_kmap_iter_tt *iter_tt,
		      struct ttm_tt *tt)
{
	iter_tt->base.ops = &ttm_kmap_iter_tt_ops;
	iter_tt->tt = tt;
	return &iter_tt->base;
}

void i915_ttm_move_memcpy(struct ttm_buffer_object *bo,
			  struct ttm_resource *new_mem,
			  struct ttm_kmap_iter *new_iter,
			  struct ttm_kmap_iter *old_iter);
#endif
