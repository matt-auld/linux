#ifndef __DRM_GEMFS_H
#define __DRM_GEMFS_H

#include <linux/pagemap.h>

struct drm_gem_object;

struct page *drm_gemfs_read_page_gfp(struct drm_gem_object *obj,
				      pgoff_t index, gfp_t gfp);

struct page *drm_gemfs_read_page(struct drm_gem_object *obj, pgoff_t index);

void drm_gemfs_truncate(struct drm_gem_object *obj);

struct file *drm_gemfs_file_setup(const char *name, loff_t size,
				  unsigned long flags);

#endif
