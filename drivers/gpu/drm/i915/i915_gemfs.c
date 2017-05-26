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

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/pagemap.h>

static const struct dentry_operations anon_ops = {
	.d_dname = simple_dname
};

struct vfsmount *i915_gemfs_create(void)
{
	struct file_system_type *type;
	struct vfsmount *gemfs_mnt;

	type = get_fs_type("tmpfs");
	if (!type)
		return ERR_PTR(-EINVAL);

	gemfs_mnt = kern_mount(type);

#if defined(CONFIG_TRANSPARENT_HUGE_PAGECACHE)
	if (!IS_ERR(gemfs_mnt) && has_transparent_hugepage()) {
		struct super_block *sb = gemfs_mnt->mnt_sb;
		char options[] = "huge=within_size";
		int flags = 0;
		int ret;

		/* Idealy we would just pass the mount options when mounting,
		 * but for some reason shmem chooses not to parse the options
		 * for MS_KERNMOUNT, probably because shm_mnt is the only tmpfs
		 * kernel mount other than this, where the mount options aren't
		 * used. To workaround this we do a remount, which is fairly
		 * inexpensive, where we know the options are never igonored.
		 */
		ret = sb->s_op->remount_fs(sb, &flags, options);
		if (ret) {
			kern_unmount(gemfs_mnt);
			return ERR_PTR(ret);
		}
	}
#endif

	return gemfs_mnt;
}

void i915_gemfs_destroy(struct vfsmount *gemfs_mnt)
{
	kern_unmount(gemfs_mnt);
}

struct file *i915_gemfs_file_setup(struct vfsmount *gemfs_mnt,
				   const char *name, size_t size)
{
	struct super_block *sb = gemfs_mnt->mnt_sb;
	struct inode *dir = d_inode(sb->s_root);
	struct inode *inode;
	struct path path;
	struct qstr this;
	struct file *res;
	int ret;

	if (size < 0 || size > MAX_LFS_FILESIZE)
		return ERR_PTR(-EINVAL);

	this.name = name;
	this.len = strlen(name);
	this.hash = 0;

	path.mnt = mntget(gemfs_mnt);
	path.dentry = d_alloc_pseudo(sb, &this);
	if (!path.dentry) {
		res = ERR_PTR(-ENOMEM);
		goto put_path;
	}
	d_set_d_op(path.dentry, &anon_ops);

	ret = dir->i_op->create(dir, path.dentry, S_IFREG | S_IRWXUGO, false);
	if (ret) {
		res = ERR_PTR(ret);
		goto put_path;
	}

	inode = d_inode(path.dentry);
	inode->i_size = size;

	res = alloc_file(&path, FMODE_WRITE | FMODE_READ, inode->i_fop);
	if (IS_ERR(res))
		goto unlink;

	return res;

unlink:
	dir->i_op->unlink(dir, path.dentry);
put_path:
	path_put(&path);
	return res;
}

int i915_gemfs_unlink(struct file *filp)
{
	struct inode *dir = d_inode(filp->f_inode->i_sb->s_root);

	return dir->i_op->unlink(dir, filp->f_path.dentry);
}
