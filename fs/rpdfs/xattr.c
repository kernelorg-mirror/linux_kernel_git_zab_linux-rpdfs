/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include <linux/xxhash.h>

#include "btree_txn.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "pr.h"
#include "xattr.h"

struct key_xattr {
	struct rpdfs_btree_key key;
	const void *value;
	struct rpdfs_xattr xattr;
};

static unsigned xattr_size(struct key_xattr *kx)
{
	int name_len = kx->xattr.name_len;

	return offsetof(struct rpdfs_xattr, name[name_len]) +
		__le16_to_cpu(kx->xattr.val_len);
}

static u64 xattr_hash(const char *name, const size_t name_len)
{
	return xxh64(name, name_len, RPDFS_XATTR_HASH_SEED);
}

static void init_xattr_key(struct rpdfs_btree_key *key, u64 hash)
{
	key->msq = cpu_to_le64(hash);
	key->lsq = 0;
}

static void xattr_key_set_uniq(struct key_xattr *kx, __le64 creates)
{
	kx->key.lsq = creates;
}

static u64 xattr_key_hash(struct rpdfs_btree_key *key)
{
	return le64_to_cpu(key->msq);
}

static bool xattr_name_matches(struct rpdfs_btree_item_args *bti,
			       const char *name, unsigned name_len)
{
	struct rpdfs_xattr *xattr = bti->val;

	return xattr->name_len == name_len &&
		memcmp(xattr->name, name, name_len) == 0;
}

static bool xattr_hash_and_name_matches(struct rpdfs_btree_item_args *bti,
					struct key_xattr *kx)
{
	return xattr_key_hash(&bti->key) == xattr_key_hash(&kx->key) &&
	       xattr_name_matches(bti, kx->xattr.name, kx->xattr.name_len);
}

static void init_key_xattr(struct key_xattr *kx, const char *name,
			   const size_t name_len, const void *value,
			   size_t size, bool copy_value)
{
	BUG_ON(name_len == 0);
	BUG_ON(name_len > RPDFS_XATTR_MAX_NAME_LEN);
	BUG_ON(name_len + size > RPDFS_XATTR_MAX_SIZE);

	init_xattr_key(&kx->key, xattr_hash(name, name_len));

	memcpy(&kx->xattr.name[0], name, name_len);
	kx->xattr.name_len = name_len;
	kx->xattr.val_len = cpu_to_le16(size);

	if (copy_value) {
		memcpy(&kx->xattr.name[name_len], value, size);
		kx->value = &kx->xattr.name[name_len];
	} else {
		kx->value = value;
	}
}

static struct key_xattr *alloc_key_xattr(const char *name, const void *value,
					 size_t size, bool copy_value)
{
	const size_t name_len = strlen(name);
	size_t alloc_size;
	struct key_xattr *kx;

	alloc_size = offsetof(struct key_xattr, xattr.name[name_len]);
	if (copy_value)
		alloc_size += size;

	kx = kmalloc(alloc_size, GFP_NOFS);
	if (kx)
		init_key_xattr(kx, name, name_len, value, size, copy_value);

	return kx;
}

static int add_xattr_item_cb(struct rpdfs_fs_info *rfi,
			     struct rpdfs_btree_item_args *a,
			     struct rpdfs_btree_item_args *b,
			     struct rpdfs_btree_item_args *ins,
			     void *arg)
{
	struct key_xattr *kx = arg;

	if (a && xattr_key_hash(&kx->key) == xattr_key_hash(&a->key))
		return -EEXIST;

	ins->key = kx->key;
	ins->val = &kx->xattr;
	ins->val_size = xattr_size(kx);

	return 0;
}

static int delete_xattr_item_cb(struct rpdfs_fs_info *rfi,
				struct rpdfs_btree_item_args *a,
				struct rpdfs_btree_item_args *b,
				struct rpdfs_btree_item_args *ins,
				void *arg)
{
	struct key_xattr *kx = arg;

	if (a && xattr_hash_and_name_matches(a, kx))
		return 0;
	else if (b && xattr_hash_and_name_matches(b, kx))
		return 1;

	return -ENODATA;
}

static int prepare_add_xattr(struct rpdfs_fs_info *rfi,
			     struct rpdfs_transaction *txn,
			     struct inode *inode,
			     struct key_xattr *kx)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_txn_prepare_insert(rfi, txn, &ri->xattrs, &kx->key,
					      xattr_size(kx), add_xattr_item_cb,
					      kx);
}

static int prepare_delete_xattr(struct rpdfs_fs_info *rfi,
				struct rpdfs_transaction *txn,
				struct inode *inode,
				struct key_xattr *kx)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_txn_prepare_delete(rfi, txn, &ri->xattrs, &kx->key,
					      xattr_size(kx),
					      delete_xattr_item_cb, kx);
}

static int apply_add_xattr(struct rpdfs_fs_info *rfi,
			   struct rpdfs_transaction *txn,
			   struct inode *inode,
			   struct key_xattr *kx)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_txn_apply_insert(rfi, txn, &ri->xattrs, &kx->key,
					    xattr_size(kx),
					    add_xattr_item_cb, kx);
}

static int apply_delete_xattr(struct rpdfs_fs_info *rfi,
			      struct rpdfs_transaction *txn,
			      struct inode *inode,
			      struct key_xattr *kx)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_txn_apply_delete(rfi, txn, &ri->xattrs, &kx->key,
					    xattr_size(kx),
					    delete_xattr_item_cb, kx);
}

static int lookup_xattr_cb(struct rpdfs_fs_info *rfi,
			   struct rpdfs_btree_item_args *a,
			   struct rpdfs_btree_item_args *b,
			   struct rpdfs_btree_item_args *c,
			   void *arg)
{
	struct key_xattr *kx = arg;
	struct rpdfs_btree_item_args *match = NULL;
	struct rpdfs_xattr *xattr;
	int name_len;
	int in_val_len;
	int buf_len;
	int ret = 0;

	if (a && xattr_hash_and_name_matches(a, kx))
		match = a;
	else if (b && xattr_hash_and_name_matches(b, kx))
		match = b;

	if (match) {
		xattr = match->val;
		name_len = xattr->name_len;
		in_val_len = __le16_to_cpu(xattr->val_len);
		buf_len = __le16_to_cpu(kx->xattr.val_len);

		if (in_val_len <= buf_len) {
			memcpy((void *)kx->value, &xattr->name[name_len],
			       in_val_len);
			ret = in_val_len;
		} else {
			/* the caller just wants the size */
			if (buf_len == 0)
				ret = in_val_len;
			else
				ret = -ERANGE;
		}
	} else {
		ret = -ENODATA;
	}

	return ret;
}

static int lookup_xattr(struct rpdfs_fs_info *rfi, struct inode *inode,
			const char *name, void *value, size_t size)
{
	DECLARE_RPDFS_TXN(txn);
	struct key_xattr *kx;
	int ret;

	kx = alloc_key_xattr(name, value, size, false);

	do {
		ret = rpdfs_inode_txn_prepare(rfi, &txn, inode, 0) ?:
		      rpdfs_btree_txn_prepare_lookup(rfi, &txn,
						     &RPDFS_I(inode)->xattrs,
						     &kx->key, lookup_xattr_cb,
						     kx);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));

	rpdfs_txn_reset(rfi, &txn);
	kfree(kx);

	return ret;
}

static int rpdfs_xattr_get(struct inode *inode, const char *name,
			   void *value, size_t size)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);

	if (strlen(name) > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	return lookup_xattr(rfi, inode, name, value, size);
}

static int rpdfs_validate_xattr_set(int flags, bool found)
{
	int ret = 0;

	/*
	 * It's an error to specify XATTR_REPLACE if the name doesn't
	 * already exist.
	 */
	if (!found) {
		if (flags & XATTR_REPLACE)
			ret = -ENODATA;
	}

	/*
	 * It's an error to specify XATTR_CREATE if the name
	 * already exists.
	 */
	if (ret == 0 && found && (flags & XATTR_CREATE))
		ret = -EEXIST;

	return ret;
}

static int rpdfs_xattr_set(struct inode *inode, const char *name,
			   const void *value, size_t size, int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct key_xattr *old_kx = NULL;
	struct key_xattr *new_kx = NULL;
	DECLARE_RPDFS_TXN(txn);
	bool found;
	int ret;

	if (name == NULL)
		return -EINVAL;

	if (strlen(name) > RPDFS_XATTR_MAX_NAME_LEN)
		return -ERANGE;

	if (value && size > RPDFS_XATTR_MAX_SIZE)
		return -ERANGE;

	if (((flags & XATTR_CREATE) && (flags & XATTR_REPLACE)) ||
	    (flags & ~(XATTR_CREATE | XATTR_REPLACE)))
		return -EINVAL;

	old_kx = alloc_key_xattr(name, value, 0, false);
	if (!old_kx)
		return -ENOMEM;

	new_kx = alloc_key_xattr(name, value, size, true);
	if (!new_kx) {
		ret = -ENOMEM;
		goto out;
	}

	do {
		found = false;

		ret = rpdfs_inode_txn_prepare(rfi, &txn, inode, RBAF_WRITE);
		if (ret == 0) {
			ret = rpdfs_btree_txn_prepare_lookup(rfi, &txn,
							     &RPDFS_I(inode)->xattrs,
							     &old_kx->key,
							     lookup_xattr_cb,
							     old_kx);

			/* lookup returns the xattr size if it finds the name */
			if (ret >= 0) {
				found = true;
				ret = 0;
			} else if (ret == -ENODATA || ret == -ENOENT) {
				/* An empty btree gives us -ENOENT */
				ret = 0;
			}

			if (ret == 0) {
				ret = rpdfs_validate_xattr_set(flags, found);
			}

			if (ret == 0 && found) {
				ret = prepare_delete_xattr(rfi, &txn, inode,
							   old_kx);
			}
		}

		if (ret == 0 && value != NULL) {
			xattr_key_set_uniq(new_kx, ri->xattr_creates);

			ret = prepare_add_xattr(rfi, &txn, inode, new_kx);
		}
	} while (rpdfs_txn_retry(rfi, &txn, &ret));

	if (ret < 0)
		goto out;

	if (found)
		apply_delete_xattr(rfi, &txn, inode, old_kx);

	if (value) {
		apply_add_xattr(rfi, &txn, inode, new_kx);
		le64_add_cpu(&ri->xattr_creates, 1);
	}

	rpdfs_inode_txn_update(rfi, &txn, inode);

out:
	rpdfs_txn_reset(rfi, &txn);
	kfree(old_kx);
	kfree(new_kx);

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

