#include <drm/drm_vma_manager.h>
#include <drm/drm_gem.h>
#include <drm/drm_gemfs.h>
#include <linux/shmem_fs.h>

struct page *
drm_gemfs_read_page_gfp(struct drm_gem_object *obj,
			pgoff_t index, gfp_t gfp)
{
	struct address_space *mapping = obj->filp->f_mapping;

	return shmem_read_mapping_page_gfp(mapping, index, gfp);
}
EXPORT_SYMBOL(drm_gemfs_read_page_gfp);

struct page *
drm_gemfs_read_page(struct drm_gem_object *obj, pgoff_t index)
{
	struct address_space *mapping = obj->filp->f_mapping;

	return drm_gemfs_read_page_gfp(obj, index, mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(drm_gemfs_read_page);

void drm_gemfs_truncate(struct drm_gem_object *obj)
{
	struct inode *inode = file_inode(obj->filp);

	shmem_truncate_range(inode, 0, (loff_t)-1);
}
EXPORT_SYMBOL(drm_gemfs_truncate);

struct file *drm_gemfs_file_setup(const char *name, loff_t size,
				  unsigned long flags)
{
	return shmem_file_setup(name, size, flags);
}
EXPORT_SYMBOL(drm_gemfs_file_setup);
