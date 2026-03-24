/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xxhash.h>

#include "balloc.h"
#include "btree_txn.h"
#include "compare.h"
#include "dir.h"
#include "format-block.h"
#include "inode.h"

/*
 * A collection of the arguments that describe a directory entry for
 * item callbacks.  It matters mostly for rename which juggles
 * combinations of multiple dirs, names, and inodes.  These are
 * initialized from the caller's input, typically a dentry.  Our dent
 * can be prepared from args for writing into blocks or can be read from
 * blocks and returned.
 */
struct dent_cb_args {
	u64 key;
	const char *name;
	unsigned name_len;
	struct rpdfs_dirent dent;
};

/*
 * The directory entries for . and .. are generated during lookup and
 * readdir and are not "real" directory entries stored as dirents. For
 * readdir to work properly, we need the position of each entry (its
 * hash value) to be stable. We also want to generate . and .. first
 * because it's easier than inserting them somewhere in the middle and
 * because applications like it that way.
 *
 * The solution is to reserve the hash values 0 for . and 1 for .. so
 * that we can return them first in readdir() and the positions returned
 * by readdir are strictly ascending.
 */
static u64 name_hash(const char *name, size_t name_len)
{
	u64 hash;

	if (is_dot_dotdot(name, name_len)) {
		if (name_len == 1)
			return RPDFS_DIRENT_DOT_HASH;
		else
			return RPDFS_DIRENT_DOT_DOT_HASH;
	}

	hash = xxh64(name, name_len, RPDFS_DIRENT_HASH_SEED) & RPDFS_BTREE_ITEM_KEY_MASK;

	if (hash < RPDFS_DIRENT_MIN_HASH)
		hash = RPDFS_DIRENT_MIN_HASH;

	return hash;
}

static void init_dent_cb_args(struct dent_cb_args *da, struct dentry *dentry, struct inode *inode)
{
	BUG_ON(dentry->d_name.len == 0 || dentry->d_name.len > RPDFS_NAME_MAX);

	/* args used by functions */
	da->name = dentry->d_name.name;
	da->name_len = dentry->d_name.len;
	da->key = name_hash(da->name, da->name_len);

	/* initialize dent if used as input, might later clobbered for output */
	da->dent.name_len = da->name_len;
	if (inode)
		da->dent.ig = *rpdfs_inode_ig(inode);
	else
		da->dent.ig = (struct rpdfs_ino_gen) { 0, };
}

static bool bad_dent_item(struct rpdfs_dirent *dent, u16 val_size)
{
	return val_size < RPDFS_DIRENT_SIZEOF ||
	       val_size < offsetof(struct rpdfs_dirent, name[dent->name_len]);
}

static int match_dent_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct dent_cb_args *da = arg;
	struct rpdfs_dirent *dent = val;

	if (bad_dent_item(dent, val_size))
		return -EUCLEAN;

	if (rpdfs_names_match(da->name, da->name_len, dent->name, dent->name_len))
		return 0;

	return -ELOOP;
}

static int copy_dent_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct dent_cb_args *da = arg;
	struct rpdfs_dirent *dent = val;
	int ret;

	ret = match_dent_cb(rfi, key, val, val_size, arg);
	if (ret == 0)
		memcpy(&da->dent, dent, RPDFS_DIRENT_SIZEOF);
	return ret;
}

static int lookup_entry(struct rpdfs_fs_info *rfi, struct inode *dir, struct dent_cb_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_lookup(rfi, &ri->dirents, da->key, copy_dent_cb, da);
}

static int insert_dent_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	int ret = match_dent_cb(rfi, key, val, val_size, arg);
	if (ret == 0)
		ret = -EEXIST;
	return ret;
}

static int insert_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *dir, struct dent_cb_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);
	struct kvec kv[2] = {
		{ .iov_base = &da->dent, .iov_len = RPDFS_DIRENT_SIZEOF },
		{ .iov_base = (void *)da->name, .iov_len = da->name_len },
	};

	return rpdfs_btree_insert(rfi, txn, &ri->dirents, da->key, insert_dent_cb, da,
				  kv, ARRAY_SIZE(kv));
}

static int modify_dent_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct dent_cb_args *da = arg;
	struct rpdfs_dirent *dent = val;
	int ret;

	ret = match_dent_cb(rfi, key, val, val_size, arg);
	if (ret == 0) {
		dent->pers_dtype = da->dent.pers_dtype;
		dent->ig = da->dent.ig;
	}
	return ret;
}

/*
 * Modification is used to update the target inode of an existing dent.
 */
static int modify_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *dir, struct dent_cb_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_modify(rfi, txn, &ri->dirents, da->key, modify_dent_cb, da);
}

static int delete_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *dir, struct dent_cb_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_delete(rfi, txn, &ri->dirents, da->key, match_dent_cb, da);
}

static struct dentry *rpdfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct rpdfs_block_handle *hnd = NULL;
	struct dent_cb_args da;
	struct inode *inode;
	int ret;

	if (dentry->d_name.len > RPDFS_NAME_MAX) {
		inode = ERR_PTR(-ENAMETOOLONG);
		goto out;
	}

	init_dent_cb_args(&da, dentry, NULL);

	ret = rpdfs_inode_acquire(rfi, dir, &hnd, 0);
	if (ret < 0)
		goto out;

	ret = lookup_entry(rfi, dir, &da);
	rpdfs_block_release(rfi, &hnd);
	if (ret < 0 && ret != -ENOENT) {
		inode = ERR_PTR(ret);
		goto out;
	}

	if (ret == -ENOENT)
		inode = NULL;
	else
		inode = rpdfs_iget(sb, &da.dent.ig);

out:
	/* d_splice_alias passes through ERR_PTR inodes */
	return d_splice_alias(inode, dentry);
}

static void init_dir_size(struct inode *inode)
{
	i_size_write(inode, RPDFS_EMPTY_DIR_LEN);
}

static int is_dir_empty(struct inode *inode)
{
	return (i_size_read(inode) == RPDFS_EMPTY_DIR_LEN);
}

/*
 * Helper function for readability.  The caller checked size before
 * getting here consistency.
 */
static void update_dir_size(struct inode *inode, s32 len)
{
	i_size_write(inode, i_size_read(inode) + len);
}

/*
 * Allocate a new inode, insert it into the vfs inode cache, and update
 * its dirty block with the current inode.
 *
 * Is it a bit too cheeky that we're using iget_failed() to remove
 * inodes on errors?  It does all we want (unhash, mark bad, unlock_new,
 * iput) but we're not in iget.
 */
static struct inode *create_new_inode(struct mnt_idmap *idmap, struct inode *dir,
				      struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct rpdfs_transaction txn = RPDFS_INIT_TXN;
	struct rpdfs_block_handle *inode_hnd = NULL;
	struct rpdfs_block_handle *dir_hnd = NULL;
	struct inode *inode = NULL;
	struct rpdfs_ino_gen ig;
	struct dent_cb_args da;
	int ret;

	ret = rpdfs_inode_acquire(rfi, dir, &dir_hnd, RBAF_WRITE) ?:
	      rpdfs_txn_acquire_alloc(rfi, &txn, &inode_hnd);
	if (ret < 0)
		goto out;

	ig.ino = cpu_to_le64(inode_hnd->bnr);
	ig.gen = cpu_to_le64(1);
	inode = rpdfs_new_inode(sb, &ig);
	if (IS_ERR(inode)) {
		/* -EBUSY from the ino being hashed probably should be retried, not returned */
		ret = PTR_ERR(inode);
		goto out;
	}

	if (dentry) {
		init_dent_cb_args(&da, dentry, inode);

		ret = insert_entry(rfi, &txn, dir, &da);
		if (ret < 0)
			goto out;

		update_dir_size(dir, da.dent.name_len + 1);
	}

	/* update vfs inodes */
	inode_init_owner(idmap, inode, dir, mode);
	if (S_ISDIR(mode)) {
		set_nlink(inode, 2);
		init_dir_size(inode);
	} else {
		set_nlink(inode, 1);
	}

	rpdfs_inode_init_ops(inode);

	if (S_ISDIR(mode))
		inc_nlink(dir);

	rpdfs_inode_update_dirty(rfi, &txn, inode, inode_hnd);
	ret = 0;
out:
	rpdfs_inode_update_dirty(rfi, &txn, dir, dir_hnd);

	if (ret < 0) {
		if (!IS_ERR_OR_NULL(inode))
			iget_failed(inode);
		inode = ERR_PTR(ret);
		if (inode_hnd)
			rpdfs_balloc_free_meta(rfi, &txn, inode_hnd->bnr);
	}

	rpdfs_block_release(rfi, &dir_hnd);
	rpdfs_block_release(rfi, &inode_hnd);
	rpdfs_txn_finish(rfi, &txn);

	return inode;
}

static int create_and_instantiate_new(struct mnt_idmap *idmap, struct inode *dir,
				      struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = create_new_inode(idmap, dir, dentry, mode);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
	} else {
		d_instantiate_new(dentry, inode);
		ret = 0;
	}

	return ret;
}

static int rpdfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
			umode_t mode, bool excl)
{
	return create_and_instantiate_new(idmap, dir, dentry, S_IFREG | mode);
}

static struct dentry *rpdfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	return ERR_PTR(create_and_instantiate_new(idmap, dir, dentry, S_IFDIR | mode));
}

/*
 * The VFS did a bunch of checks before calling our unlink
 * implementation, but some other node could have made changes between
 * then and now.
 */
static int check_unlink(struct inode *dir, struct inode *inode, struct dentry *dentry)
{
	int ret;

	/* normal failures due to other nodes making changes */
	if (S_ISDIR(inode->i_mode)) {
		if (!is_dir_empty(inode)) {
			ret = -ENOTEMPTY;
			goto out;
		}
	}

	/* consistency/corruption checks */
	if (S_ISDIR(inode->i_mode)) {
		if (inode->i_nlink != 2) {
			pr_warn("empty dir ino %lu has bad n_link %d",
				inode->i_ino, inode->i_nlink);
			ret = -EUCLEAN;
			goto out;
		}
	} else {
		if (inode->i_nlink < 1) {
			pr_warn("attempting to unlink ino %lu but n_link %d is already < 1",
				inode->i_ino, inode->i_nlink);
			ret = -EUCLEAN;
			goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

static int rpdfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct inode *inode = d_inode(dentry);
	struct rpdfs_block_handle *inode_hnd = NULL;
	struct rpdfs_block_handle *dir_hnd = NULL;
	struct rpdfs_transaction txn = RPDFS_INIT_TXN;
	struct dent_cb_args da;
	int ret;

	ret = rpdfs_inode_acquire_ordered(rfi, dir, &dir_hnd, inode, &inode_hnd,
					  NULL, NULL, NULL, NULL, RBAF_WRITE);
	if (ret < 0)
		goto out;

	init_dent_cb_args(&da, dentry, inode);

	ret = check_unlink(dir, inode, dentry) ?:
	      delete_entry(rfi, &txn, dir, &da);
	if (ret < 0)
		goto out;

	/* update link count and metadata change time */
	drop_nlink(inode);
	if (S_ISDIR(inode->i_mode)) {
		drop_nlink(dir);
		drop_nlink(inode);
	}
	inode_set_ctime_current(inode);

	/* update parent dir size and data/metadata times */
	update_dir_size(dir, -(dentry->d_name.len + 1));
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	rpdfs_inode_update_dirty(rfi, &txn, inode, inode_hnd);
	ret = 0;
out:
	rpdfs_inode_update_dirty(rfi, &txn, dir, dir_hnd);

	rpdfs_block_release(rfi, &dir_hnd);
	rpdfs_block_release(rfi, &inode_hnd);
	rpdfs_txn_finish(rfi, &txn);

	return ret;
}

static int rpdfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	return rpdfs_unlink(dir, dentry);
}

static int rpdfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(old_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct rpdfs_transaction txn = RPDFS_INIT_TXN;
	struct rpdfs_block_handle *old_dir_hnd = NULL;
	struct rpdfs_block_handle *new_dir_hnd = NULL;
	struct rpdfs_block_handle *old_inode_hnd = NULL;
	struct rpdfs_block_handle *new_inode_hnd = NULL;
	struct dent_cb_args old_da;
	struct dent_cb_args new_da;
	struct timespec64 now;
	int ret;
	int err;

	if (flags & ~RENAME_NOREPLACE) {
		ret = -EINVAL;
		goto out;
	}

	if (old_dentry->d_name.len > RPDFS_NAME_MAX ||
	    new_dentry->d_name.len > RPDFS_NAME_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	ret = rpdfs_inode_acquire_ordered(rfi, old_dir, &old_dir_hnd,
					  (new_dir != old_dir) ? new_dir : NULL, &new_dir_hnd,
					  old_inode, &old_inode_hnd,
					  new_inode, &new_inode_hnd, RBAF_WRITE);
	if (ret < 0)
		goto out;

	init_dent_cb_args(&old_da, old_dentry, old_inode);
	init_dent_cb_args(&new_da, new_dentry, new_inode);

	if (new_inode)
		ret = modify_entry(rfi, &txn, new_dir, &new_da);
	else
		ret = insert_entry(rfi, &txn, new_dir, &new_da);
	if (ret < 0)
		goto out;

	ret = delete_entry(rfi, &txn, old_dir, &old_da);
	if (ret < 0) {
		if (new_inode) {
			/* clobber new_da to modify new name to point to old inode */
			init_dent_cb_args(&new_da, new_dentry, old_inode);
			err = modify_entry(rfi, &txn, new_dir, &new_da);
		} else {
			err = delete_entry(rfi, &txn, new_dir, &new_da);
		}
		BUG_ON(err); /* unwind can't get errors */
		goto out;
	}

	/* update dir sizes */
	update_dir_size(old_dir, -(old_dentry->d_name.len + 1));
	if (!new_inode)
		update_dir_size(new_dir, new_dentry->d_name.len + 1);

	/* and link counts */
	if (new_inode) {
		drop_nlink(new_inode);
		if (S_ISDIR(new_inode->i_mode)) {
			drop_nlink(new_dir);
			drop_nlink(new_inode);
		}
	}

	if (S_ISDIR(old_inode->i_mode) && (old_dir != new_dir)) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	/* .. and finally times */
	now = inode_set_ctime_current(old_dir);
	inode_set_mtime_to_ts(old_dir, now);
	if (new_dir != old_dir) {
		inode_set_ctime_to_ts(new_dir, now);
		inode_set_mtime_to_ts(new_dir, now);
	}
	inode_set_ctime_to_ts(old_inode, now);
	if (new_inode)
		inode_set_ctime_to_ts(new_inode, now);

	rpdfs_inode_update_dirty(rfi, &txn, old_inode, old_inode_hnd);
	if (new_inode)
		rpdfs_inode_update_dirty(rfi, &txn, new_inode, new_inode_hnd);
	ret = 0;
out:
	rpdfs_inode_update_dirty(rfi, &txn, old_dir, old_dir_hnd);
	if (new_dir != old_dir)
		rpdfs_inode_update_dirty(rfi, &txn, new_dir, new_dir_hnd);

	rpdfs_block_release(rfi, &old_dir_hnd);
	rpdfs_block_release(rfi, &new_dir_hnd);
	rpdfs_block_release(rfi, &old_inode_hnd);
	rpdfs_block_release(rfi, &old_inode_hnd);

	rpdfs_txn_finish(rfi, &txn);

	return ret;
}

static size_t aligned_dent_size(size_t name_len)
{
	return ALIGN(offsetof(struct rpdfs_dirent, name[name_len]),
		     __alignof__(struct rpdfs_dirent));
}

/*
 * We're using the copied dent's gen to store the key so that we don't
 * have to create some wrapper type to store the dent and key.
 */
static int copy_item_cb(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_size, void *arg)
{
	struct kvec *kv = arg;
	struct rpdfs_dirent *dent = val;
	struct rpdfs_dirent *copied;
	size_t size;

	if (bad_dent_item(dent, val_size))
		return -EUCLEAN;

	size = aligned_dent_size(dent->name_len);
	if (size > kv->iov_len)
		return 0;

	copied = kv->iov_base;
	memcpy(copied, dent, offsetof(struct rpdfs_dirent, name[dent->name_len]));
	copied->ig.gen = cpu_to_le64(key);

	kv->iov_base += size;
	kv->iov_len -= size;
	return -ELOOP;
}

/*
 * We don't want to fault emitting an entry while holding a block ref so
 * we bounce the entries we read through a buffer from which we then
 * emit.
 */
static int rpdfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	const size_t buf_size = RPDFS_BLOCK_SIZE;
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_dirent *dent;
	void *buf = NULL;
	struct kvec kv;
	int ret;

	if (!dir_emit_dots(file, ctx)) {
		ret = 0;
		goto out;
	}

	buf = kvmalloc(buf_size, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	ret = rpdfs_inode_acquire(rfi, inode, &hnd, 0);
	if (ret < 0)
		goto out;

	for (;;) {
		/* done if input or iteration passes valid keys */
		if (ctx->pos > RPDFS_BTREE_ITEM_KEY_MASK) {
			ret = 0;
			goto out;
		}

		kv.iov_base = buf;
		kv.iov_len = buf_size;
		ret = rpdfs_btree_read_items(rfi, &ri->dirents, ctx->pos, copy_item_cb, &kv);
		if (ret < 0)
			goto out;

		/* done when there's no more entries */
		if (kv.iov_base == buf)
			break;

		dent = buf;
		while ((void *)dent < kv.iov_base) {
			ctx->pos = le64_to_cpu(dent->ig.gen);
			if (!dir_emit(ctx, dent->name, dent->name_len, le64_to_cpu(dent->ig.ino),
				      DT_UNKNOWN) || ctx->pos == S64_MAX) {
				ret = 0;
				goto out;
			}

			ctx->pos++;
			dent = (void *)dent + aligned_dent_size(dent->name_len);
		}
	}

	ret = 0;
out:
	rpdfs_block_release(rfi, &hnd);
	kfree(buf);
	return ret;
}

const struct inode_operations rpdfs_dir_iops = {
	.create		= rpdfs_create,
	.getattr	= rpdfs_getattr,
	.lookup		= rpdfs_lookup,
	.mkdir		= rpdfs_mkdir,
	.rename		= rpdfs_rename,
	.setattr	= rpdfs_setattr,
	.unlink		= rpdfs_unlink,
	.rmdir		= rpdfs_rmdir,
};

const struct file_operations rpdfs_dir_fops = {
	.read		= generic_read_dir,
	.iterate_shared	= rpdfs_readdir,
};
