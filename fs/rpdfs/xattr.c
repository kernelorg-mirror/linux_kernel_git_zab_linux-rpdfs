/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include <linux/xxhash.h>

#include "compare.h"
#include "ehtable.h"
#include "inode.h"
#include "pr.h"
#include "xattr.h"

static u32 name_hash(const char *name, unsigned name_len)
{
	return xxh32(name, name_len, RPDFS_XATTR_HASH_SEED);
}

/*
 * We see the stored value size in the iargs so this is where we
 * implement the getxattr return that depends on the value buffer size.
 */
static int lookup_xattr(struct inode *inode, const char *name, unsigned name_len,
			const void *value, size_t size)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_ehtable_item_args iargs = {
		.key = name,
		.key_size = name_len,
		.hash = name_hash(name, name_len),
		.val = (void *)value,
		.val_size = size,
	};
	int ret;

	ret = rpdfs_ehtable_lookup(inode, &ri->xattr_eht, RPDFS_BLOCK_KEY_TYPE_XATTR, &iargs);
	if (ret >= 0 && iargs.val_size > size) {
		if (size == 0)
			ret = iargs.val_size;
		else
			ret = -ERANGE;
	}

	return ret;
}

static int set_xattr(struct inode *inode, const char *name, unsigned name_len,
		     const void *value, size_t size, int flags)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_ehtable_item_args iargs = {
		.key = name,
		.key_size = name_len,
		.hash = name_hash(name, name_len),
		.val = (void *)value,
		.val_size = size,
	};
	int ehtfl;
	int ret;

	if (flags & XATTR_CREATE)
		ehtfl = RPDFS_EHT_EEXIST;
	else if (flags & XATTR_REPLACE)
		ehtfl = RPDFS_EHT_ENOENT;
	else
		ehtfl = 0;

	ret = rpdfs_ehtable_set(inode, &ri->xattr_eht, RPDFS_BLOCK_KEY_TYPE_XATTR, &iargs, ehtfl);
	if (ret == -ENOENT)
		ret = -ENODATA;

	return ret;
}

static int delete_xattr(struct inode *inode, const char *name, unsigned name_len)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_ehtable_item_args iargs = {
		.key = name,
		.key_size = name_len,
		.hash = name_hash(name, name_len),
	};

	return rpdfs_ehtable_delete(inode, &ri->xattr_eht, RPDFS_BLOCK_KEY_TYPE_XATTR, &iargs);
}

static int rpdfs_xattr_get(struct inode *inode, const char *name, void *value, size_t size)
{
	unsigned name_len = strlen(name);
	int ret;

	if (name_len > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	/* XXX rlock */

	ret = lookup_xattr(inode, name, name_len, value, size);
	if (ret == -ENOENT)
		ret = -ENODATA;

	return ret;
}

static int rpdfs_xattr_set(struct inode *inode, const char *name, const void *value, size_t size,
			   int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	unsigned name_len = strlen(name);
	int ret;

	if (name == NULL)
		return -EINVAL;

	if (name_len > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	if (value && (size > RPDFS_EHTABLE_MAX_VAL_SIZE))
		return -E2BIG;

	if (((flags & XATTR_CREATE) && (flags & XATTR_REPLACE)) ||
	    (flags & ~(XATTR_CREATE | XATTR_REPLACE)))
		return -EINVAL;

	/* XXX rlock */

	if (value)
		ret = set_xattr(inode, name, name_len, value, size, flags);
	else
		ret = delete_xattr(inode, name, name_len);
	if (ret == 0)
		rpdfs_inode_update(rfi, inode);

	return ret;
}

/*
 * buf can be null if they're probing the size of the names and want to
 * get -ERANGE if the size is insufficient.  Size can be 0 if they just
 * want the size.
 */
ssize_t rpdfs_listxattr(struct dentry *dentry, char *buf, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	const size_t part_size = RPDFS_BLOCK_SIZE;
	struct rpdfs_ehtable_item_args *iargs;
	void *part = NULL;
	int copied = 0;
	int with_null;
	u64 pos;
	int ret;
	int nr;

	/* XXX rlock */

	part = kvmalloc(part_size, GFP_KERNEL);
	if (!part) {
		ret = -ENOMEM;
		goto out;
	}

	for (pos = 0; pos <= S32_MAX; ) {
		ret = rpdfs_ehtable_read_items(inode, &ri->xattr_eht, RPDFS_BLOCK_KEY_TYPE_XATTR,
					       pos, part, part_size);
		if (ret <= 0) {
			if (ret == -ENOENT)
				ret = 0;
			goto out;
		}
		nr = ret;

		rpdfs_ehtable_for_each_buf_iargs(part, nr, iargs) {
			with_null = iargs->key_size + 1;

			if (size > 0 && copied + with_null > size) {
				ret = -ERANGE;
				goto out;
			}

			if (buf) {
				memcpy(buf + copied, iargs->key, iargs->key_size);
				buf[copied + iargs->key_size] = '\0';
			}

			copied += with_null;
			pos = iargs->pos + 1;
		}
	}

	ret = 0;
out:
	kfree(part);
	return ret ?: copied;
}

static int rpdfs_xattr_get_handler(const struct xattr_handler *handler,
				   struct dentry *unused, struct inode *inode,
				   const char *name, void *value, size_t size)
{
	name = xattr_full_name(handler, name);

	return rpdfs_xattr_get(inode, name, value, size);
}

static int rpdfs_xattr_set_handler(const struct xattr_handler *handler,
				   struct mnt_idmap *idmap,
				   struct dentry *unused, struct inode *inode,
				   const char *name, const void *value,
				   size_t size, int flags)
{
	name = xattr_full_name(handler, name);

	return rpdfs_xattr_set(inode, name, value, size, flags);
}

static const struct xattr_handler rpdfs_xattr_user_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get = rpdfs_xattr_get_handler,
	.set = rpdfs_xattr_set_handler,
};

const struct xattr_handler * const rpdfs_xattr_handlers[] = {
	&rpdfs_xattr_user_handler,
	NULL
};
