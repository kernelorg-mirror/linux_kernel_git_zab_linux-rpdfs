/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include <linux/xxhash.h>

#include "btree_txn.h"
#include "compare.h"
#include "inode.h"
#include "pr.h"
#include "xattr.h"

struct xattr_cb_args {
	u64 key;
	const char *name;
	unsigned name_len;
	void *value;
	size_t size;
	bool del_key;

	struct rpdfs_xattr xattr;
};

static size_t xattr_item_size(u16 name_len, size_t size)
{
	return offsetof(struct rpdfs_xattr, name[name_len + size]);
}

static void init_xattr_cb_args(struct xattr_cb_args *xa, const char *name, unsigned name_len,
			       const void *value, size_t size)
{
	BUG_ON(name_len == 0);
	BUG_ON(name_len > RPDFS_XATTR_MAX_NAME_LEN);
	BUG_ON(xattr_item_size(name_len, size) > RPDFS_BTREE_MAX_VAL_SIZE);

	xa->key = xxh64(name, name_len, RPDFS_XATTR_HASH_SEED) & RPDFS_BTREE_ITEM_KEY_MASK;
	xa->name = name;
	xa->name_len = name_len;
	xa->value = (void *)value; /* _get copied into value, _set const doesn't :/ */
	xa->size = size;
	xa->del_key = false;

	xa->xattr.val_len = cpu_to_le16(size);
	xa->xattr.name_len = name_len;
}

static bool bad_xattr_item(struct rpdfs_xattr *xattr, u16 val_size)
{
	return val_size < RPDFS_XATTR_SIZEOF ||
	       xattr->name_len > RPDFS_XATTR_MAX_NAME_LEN ||
	       val_size < xattr_item_size(xattr->name_len, le16_to_cpu(xattr->val_len));
}

static u64 xattr_place_hi(struct inode *inode)
{
	return rpdfs_place_hi(RPDFS_PLACE_XATTR_BTREE, rpdfs_inode_ino(inode), 0);
}

static int match_xattr_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct xattr_cb_args *xa = arg;
	struct rpdfs_xattr *xattr = val;

	if (bad_xattr_item(xattr, val_size))
		return -EUCLEAN;

	if (rpdfs_names_match(xa->name, xa->name_len, xattr->name, xattr->name_len))
		return 0;

	return -ELOOP;
}

/*
 * If we find the item then the return value matches getxattr semantics.
 * If we return >= 0 then we also update the caller's args with the
 * specific key that matched the name.
 */
static int lookup_xattr_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct xattr_cb_args *xa = arg;
	struct rpdfs_xattr *xattr = val;
	unsigned val_len;
	int ret;

	ret = match_xattr_cb(rfi, key, val, val_size, arg);
	if (ret == 0) {
		val_len = le16_to_cpu(xattr->val_len);
		if (xa->size > 0 && xa->size < val_len) {
			ret = -ERANGE;
		} else {
			xa->key = key;
			if (val_len < xa->size)
				memcpy(xa->value, &xattr->name[xattr->name_len], val_len);
			ret = val_len;
		}
	}

	return ret;
}

static int lookup_xattr(struct rpdfs_fs_info *rfi, struct inode *inode, struct xattr_cb_args *xa)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_lookup(rfi, &ri->xattrs, xa->key, lookup_xattr_cb, xa);
}

/* we modify xattrs by temporarily inserting at the same name, no need to check eexist */
static int nil_xattr_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	return -ELOOP;
}

static int insert_xattr(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *inode, struct xattr_cb_args *xa)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct kvec kv[3] = {
		{ .iov_base = &xa->xattr, .iov_len = RPDFS_XATTR_SIZEOF },
		{ .iov_base = (void *)xa->name, .iov_len = xa->name_len },
	};
	unsigned long nr_segs = 2;

	/* pretty sure everything would handle a 0 len vec, but why make them */
	if (xa->size > 0) {
		kv[2] = (struct kvec) { .iov_base = xa->value, .iov_len = xa->size };
		nr_segs++;
	}

	return rpdfs_btree_insert(rfi, txn, xattr_place_hi(inode), &ri->xattrs, xa->key,
				  nil_xattr_cb, xa, kv, nr_segs);
}

static int delete_xattr_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct xattr_cb_args *xa = arg;

	if ((xa->del_key && key == xa->key) ||
	    (!xa->del_key && key != xa->key && match_xattr_cb(rfi, key, val, val_size, arg)))
		return 0;

	return -ELOOP;
}

/*
 * Delete a btree item that either matches the key or matches the name
 * and not the key.
 */
static int delete_xattr(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *inode, struct xattr_cb_args *xa, bool del_key)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	xa->del_key = del_key;
	return rpdfs_btree_delete(rfi, txn, xattr_place_hi(inode), &ri->xattrs, xa->key,
				  delete_xattr_cb, xa);
}

static int rpdfs_xattr_get(struct inode *inode, const char *name, void *value, size_t size)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_block_handle *hnd = NULL;
	unsigned name_len = strlen(name);
	struct xattr_cb_args xa;
	int ret;

	if (name_len > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	ret = rpdfs_inode_acquire(rfi, NULL, inode, &hnd, 0);
	if (ret < 0)
		goto out;

	init_xattr_cb_args(&xa, name, name_len, value, size);

	ret = lookup_xattr(rfi, inode, &xa);
	if (ret == -ENOENT)
		ret = -ENODATA;

	rpdfs_block_release(rfi, &hnd);
out:
	return ret;
}

/*
 * This takes the simpler approach of changing an existing xattr's value
 * by inserting a new xattr item with the new value and deleting the
 * existing item with the old value.   It means that overwriting an
 * existing value can fail with enospc or too many item key collisions.
 */
static int rpdfs_xattr_set(struct inode *inode, const char *name, const void *value, size_t size,
			   int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_transaction txn = RPDFS_INIT_TXN;
	struct rpdfs_block_handle *hnd = NULL;
	unsigned name_len = strlen(name);
	struct xattr_cb_args old_xa;
	struct xattr_cb_args new_xa;
	bool found;
	int ret;
	int err;

	if (name == NULL)
		return -EINVAL;

	if (name_len > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	if (value && xattr_item_size(name_len, size) > RPDFS_BTREE_MAX_VAL_SIZE)
		return -ERANGE;

	if (((flags & XATTR_CREATE) && (flags & XATTR_REPLACE)) ||
	    (flags & ~(XATTR_CREATE | XATTR_REPLACE)))
		return -EINVAL;

	init_xattr_cb_args(&new_xa, name, name_len, value, size);
	old_xa = new_xa;
	/* don't copy value when seeing if old exists */
	old_xa.value = NULL;
	old_xa.size = 0;

	ret = rpdfs_inode_acquire(rfi, &txn, inode, &hnd, RBAF_WRITE);
	if (ret < 0)
		goto out;

	ret = lookup_xattr(rfi, inode, &old_xa);
	if (ret < 0 && ret != -ENOENT)
		goto out;
	found = ret >= 0;

	/* enforce existence flags */
	ret = 0;
	if (found && (flags & XATTR_CREATE))
		ret = -EEXIST;
	else if (!found && (flags & XATTR_REPLACE))
		ret = -ENODATA;
	if (ret < 0)
		goto out;

	if (value) {
		ret = insert_xattr(rfi, &txn, inode, &new_xa);
		if (ret < 0)
			goto out;
	}

	if (found) {
		ret = delete_xattr(rfi, &txn, inode, &old_xa, true);
		if (ret < 0) {
			if (value) {
				err = delete_xattr(rfi, &txn, inode, &new_xa, false);
				BUG_ON(err);
			}
			goto out;
		}
	}


	ret = 0;
out:
	rpdfs_inode_update(rfi, inode, hnd);
	rpdfs_block_release(rfi, &hnd);
	rpdfs_txn_finish(rfi, &txn);

	return ret;
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

