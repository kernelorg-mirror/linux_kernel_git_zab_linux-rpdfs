/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xxhash.h>
#include "compare.h"
#include "dir.h"
#include "ehtable.h"
#include "format-block.h"
#include "inode.h"
#include "rlock.h"

/*
 * A collection of the arguments that describe a directory entry for the
 * ehtable calls.  It matters mostly for rename which juggles
 * combinations of multiple dirs, names, and inodes.  These are
 * initialized from the caller's input, typically a dentry.  Our dent
 * can be prepared from args for writing into blocks or can be read from
 * blocks and returned.
 */
struct dent_args {
	u32 hash;
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
static u32 name_hash(const char *name, size_t name_len)
{
	u32 hash;

	if (is_dot_dotdot(name, name_len)) {
		if (name_len == 1)
			return RPDFS_DIRENT_DOT_HASH;
		else
			return RPDFS_DIRENT_DOT_DOT_HASH;
	}

	hash = xxh32(name, name_len, RPDFS_DIRENT_HASH_SEED);

	if (hash < RPDFS_DIRENT_MIN_HASH)
		hash = RPDFS_DIRENT_MIN_HASH;

	return hash;
}

static void set_dent_args_inode(struct dent_args *da, struct inode *inode)
{
	da->dent.ino = *rpdfs_inode_ino(inode);
}

static void init_dent_args(struct dent_args *da, struct dentry *dentry, struct inode *inode)
{
	BUG_ON(dentry->d_name.len == 0 || dentry->d_name.len > RPDFS_NAME_MAX);

	/* args used by functions */
	da->name = dentry->d_name.name;
	da->name_len = dentry->d_name.len;
	da->hash = name_hash(da->name, da->name_len);

	memset(&da->dent, 0, sizeof(da->dent));
	if (inode)
		set_dent_args_inode(da, inode);
}

static int lookup_entry(struct inode *dir, struct dent_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);
	struct rpdfs_ehtable_item_args iargs = {
		.key = da->name,
		.key_size = da->name_len,
		.hash = da->hash,
		.val = &da->dent,
		.val_size = sizeof(da->dent),
	};
	int ret;

	ret = rpdfs_ehtable_lookup(dir, &ri->dirent_eht, RPDFS_BLOCK_KEY_TYPE_DIRENT, &iargs);
	if (ret >= 0) {
		if (iargs.val_size != sizeof(da->dent))
			ret = -EUCLEAN;
		else
			ret = 0;
	}
	return ret;
}

static int insert_entry(struct inode *dir, struct dent_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);
	struct rpdfs_ehtable_item_args iargs = {
		.key = da->name,
		.key_size = da->name_len,
		.hash = da->hash,
		.val = &da->dent,
		.val_size = sizeof(da->dent),
	};

	return rpdfs_ehtable_insert(dir, &ri->dirent_eht, RPDFS_BLOCK_KEY_TYPE_DIRENT, &iargs);
}

static int delete_entry(struct inode *dir, struct dent_args *da)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);
	struct rpdfs_ehtable_item_args iargs = {
		.key = da->name,
		.key_size = da->name_len,
		.hash = da->hash,
	};

	return rpdfs_ehtable_delete(dir, &ri->dirent_eht, RPDFS_BLOCK_KEY_TYPE_DIRENT, &iargs);
}

/*
 * Between d_time and d_fsdata we have 64bits of counter storage on
 * 32bit.  This is called with the rlock held so the counter won't
 * change.
 */
static void set_invalidate_counter(struct inode *dir, struct dentry *dentry)
{
	u64 ctr = rpdfs_inode_invalidate_counter(dir);

	spin_lock(&dentry->d_lock);
	raw_write_seqcount_begin(&dentry->d_seq);

	if (sizeof(dentry->d_time) < sizeof(ctr)) {
		dentry->d_time = (u32)ctr;
		dentry->d_fsdata = (void *)(uintptr_t)(ctr >> 32);
	} else  {
		dentry->d_time = ctr;
	}

	raw_write_seqcount_end(&dentry->d_seq);
	spin_unlock(&dentry->d_lock);
}

/*
 * It's reasonably important to have read-only tests in d_revalidate as
 * tasks do full path resolution with shared prefixes.
 */
static bool stale_invalidate_counter(struct inode *dir, struct dentry *dentry)
{
	u64 ctr = rpdfs_inode_invalidate_counter(dir);
	unsigned seq;
	bool stale;

	do {
		seq = read_seqcount_begin(&dentry->d_seq);

		if (sizeof(dentry->d_time) < sizeof(ctr)) {
			u64 d_ctr = ((u64)(uintptr_t)dentry->d_fsdata << 32) | dentry->d_time;
			stale = d_ctr != ctr;
		} else  {
			stale = dentry->d_time != ctr;
		}

	} while (read_seqcount_retry(&dentry->d_seq, seq));

	return stale;
}

static int rpdfs_d_revalidate(struct inode *dir, const struct qstr *name, struct dentry *dentry,
			      unsigned int flags)
{
	return !stale_invalidate_counter(dir, dentry);
}

/*
 * The dentry args that vfs callers check can go stale before our
 * methods are called and we're able to protect them across the network
 * with rlocks.  This is effectively a mini d_revalidate->lookup.  We
 * check for the presence of the dirent and synthesize errors that the
 * caller would have seen.
 */
static int validate_dentry(struct inode *dir, struct dentry *dentry, struct dent_args *da)
{
	const bool positive = dentry->d_inode != NULL;
	int ret;

	/* expected fast path */
	if (!stale_invalidate_counter(dir, dentry)) {
		ret = 0;
		goto out;
	}

	ret = lookup_entry(dir, da);
	if (ret < 0 && ret != -ENOENT)
		goto out;

	/* caller expected negative but there was a dirent */
	if (!positive && ret == 0) {
		ret = -EEXIST;
		goto out;
	}

	/* caller expected positive but there was no dirent */
	if (positive && ret == -ENOENT) {
		ret = -ENOENT;
	}

	/* name linked to different inode than caller's */
	if (rpdfs_inode_cmp_inos(&da->dent.ino, rpdfs_inode_ino(dentry->d_inode)) != 0) {
		ret = -ESTALE;
		goto out;
	}

	/* dirent ino matches dentry ino */
	set_invalidate_counter(dir, dentry);
	ret = 0;
out:
	return ret;
}

static struct dentry *rpdfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct super_block *sb = dir->i_sb;
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	struct dent_args da;
	struct inode *inode;
	int ret;

	if (dentry->d_name.len > RPDFS_NAME_MAX) {
		inode = ERR_PTR(-ENAMETOOLONG);
		goto out;
	}

	init_dent_args(&da, dentry, NULL);

	ret = rpdfs_inode_rlock_refresh(dir, RPDFS_RLOCK_MODE_SH_RD, &hold);
	if (ret < 0) {
		inode = ERR_PTR(ret);
		goto out;
	}

	set_invalidate_counter(dir, dentry);

	ret = lookup_entry(dir, &da);
	if (ret < 0 && ret != -ENOENT) {
		inode = ERR_PTR(ret);
		goto out;
	}

	if (ret == -ENOENT)
		inode = NULL;
	else
		inode = rpdfs_iget(sb, &da.dent.ino);
out:
	rpdfs_rlock_unlock(rfi, &hold);

	/* d_splice_alias passes through ERR_PTR inodes */
	return d_splice_alias(inode, dentry);
}

static bool is_dir_empty(struct inode *inode)
{
	return (i_size_read(inode) == RPDFS_EMPTY_DIR_LEN);
}

/*
 * A directory's i_size is the sum of the size of all the null
 * terminated entry names, including "." and ".." when empty.
 */
static void set_dir_i_size(struct inode *dir)
{
	struct rpdfs_ehtable_desc *desc = &RPDFS_I(dir)->dirent_eht;

	i_size_write(dir, RPDFS_EMPTY_DIR_LEN + le32_to_cpu(desc->total_key_size) +
		     le32_to_cpu(desc->nr_keys));
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
	DECLARE_RPDFS_RLOCK_HOLD(dir_hold);
	DECLARE_RPDFS_RLOCK_HOLD(inode_hold);
	struct inode *inode = NULL;
	struct rpdfs_iget_data igd;
	struct dent_args da;
	int ret;

	rpdfs_alloc_inode_nr(rfi, &igd.ino);
	igd.is_shadow = false;

	/* a little awkward because we don't have the allocated inode yet */
	if (rpdfs_inode_cmp_inos(&igd.ino, rpdfs_inode_ino(dir)) < 0)
		ret = rpdfs_inode_rlock_ino(rfi, &igd.ino, RPDFS_RLOCK_MODE_EX_WR, &inode_hold) ?:
		      rpdfs_inode_rlock_refresh(dir, RPDFS_RLOCK_MODE_EX_WR, &dir_hold);
	else
		ret = rpdfs_inode_rlock_refresh(dir, RPDFS_RLOCK_MODE_EX_WR, &dir_hold) ?:
		      rpdfs_inode_rlock_ino(rfi, &igd.ino, RPDFS_RLOCK_MODE_EX_WR, &inode_hold);
	if (ret < 0)
		goto out;

	inode = rpdfs_new_inode(sb, &igd);
	if (IS_ERR(inode)) {
		/* -EBUSY from the ino being hashed probably should be retried, not returned */
		ret = PTR_ERR(inode);
		goto out;
	}

	if (dentry) {
		set_invalidate_counter(dir, dentry);
		init_dent_args(&da, dentry, inode);

		ret = insert_entry(dir, &da);
		if (ret < 0)
			goto out;

		set_dir_i_size(dir);
	}

	/* update vfs inodes */
	inode_init_owner(idmap, inode, dir, mode);
	if (S_ISDIR(mode))
		set_nlink(inode, 2);
	else
		set_nlink(inode, 1);

	rpdfs_inode_init_ops(inode);

	if (S_ISDIR(mode))
		inc_nlink(dir);

	rpdfs_inode_update(rfi, inode);
	ret = 0;
out:
	/* ehtable can change while returning error */
	if (!IS_ERR_OR_NULL(inode))
		rpdfs_inode_update(rfi, dir);

	rpdfs_rlock_unlock(rfi, &dir_hold);
	rpdfs_rlock_unlock(rfi, &inode_hold);

	if (ret < 0) {
		if (!IS_ERR_OR_NULL(inode))
			iget_failed(inode);
		inode = ERR_PTR(ret);
	}

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
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	struct dent_args da;
	int ret;

	init_dent_args(&da, dentry, inode);

	ret = rpdfs_inode_rlock_refresh(dir, RPDFS_RLOCK_MODE_EX_WR, &hold);
	if (ret < 0) {
		inode = ERR_PTR(ret);
		goto out;
	}

	ret = check_unlink(dir, inode, dentry) ?:
	      delete_entry(dir, &da);
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
	set_dir_i_size(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	rpdfs_inode_update(rfi, inode);
	ret = 0;
out:
	rpdfs_inode_update(rfi, dir);
	rpdfs_rlock_unlock(rfi, &hold);

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
	DECLARE_RPDFS_RLOCK_HOLD(old_dir_hold);
	DECLARE_RPDFS_RLOCK_HOLD(new_dir_hold);
	DECLARE_RPDFS_RLOCK_HOLD(old_inode_hold);
	DECLARE_RPDFS_RLOCK_HOLD(new_inode_hold);
	bool cleanup_existing = false;
	bool cleanup_new = false;
	struct dent_args old_da;
	struct dent_args new_da;
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

	/* hash names once, each insertion fills in dent inode fields */
	init_dent_args(&old_da, old_dentry, NULL);
	init_dent_args(&new_da, new_dentry, NULL);

	ret = rpdfs_inode_rlock_refresh_many(old_dir, &old_dir_hold,
					     new_dir, &new_dir_hold,
					     old_inode, &old_inode_hold,
					     new_inode, &new_inode_hold, RPDFS_RLOCK_MODE_EX_WR) ?:
	      validate_dentry(old_dir, old_dentry, &old_da) ?:
	      validate_dentry(new_dir, new_dentry, &new_da);
	if (ret < 0)
		goto out;

	if (new_inode) {
		ret = delete_entry(new_dir, &new_da);
		if (ret < 0)
			goto update_dirs;
		cleanup_existing = true;
	}

	set_dent_args_inode(&new_da, old_inode);
	ret = insert_entry(new_dir, &new_da);
	if (ret < 0)
		goto cleanup;
	cleanup_new = true;

	ret = delete_entry(old_dir, &old_da);
	if (ret < 0)
		goto cleanup;

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

	rpdfs_inode_update(rfi, old_inode);
	if (new_inode)
		rpdfs_inode_update(rfi, new_inode);

	ret = 0;

cleanup:
	if (ret < 0) {
		if (cleanup_existing) {
			set_dent_args_inode(&new_da, new_inode);
			err = insert_entry(new_dir, &new_da);
		}
		if (cleanup_new)
			err |= delete_entry(new_dir, &new_da);
		BUG_ON(err); /* XXX need to ensure successful cleanup */
	}

update_dirs:
	rpdfs_inode_update(rfi, old_dir);
	if (new_dir != old_dir)
		rpdfs_inode_update(rfi, new_dir);

out:
	rpdfs_rlock_unlock(rfi, &old_dir_hold);
	rpdfs_rlock_unlock(rfi, &new_dir_hold);
	rpdfs_rlock_unlock(rfi, &old_inode_hold);
	rpdfs_rlock_unlock(rfi, &new_inode_hold);

	return ret;
}

/*
 * We don't want to fault emitting an entry while holding a block ref so
 * we bounce the entries we read through a buffer from which we then
 * emit.
 */
static int rpdfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	const size_t buf_size = RPDFS_BLOCK_SIZE;
	struct rpdfs_ehtable_item_args *iargs;
	const struct rpdfs_dirent *dent;
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	void *buf = NULL;
	int ret;
	int nr;

	if (!dir_emit_dots(file, ctx)) {
		ret = 0;
		goto out;
	}

	buf = kvmalloc(buf_size, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	for (;;) {
		/* done if input or iteration passes valid item pos */
		if (ctx->pos > S32_MAX) {
			ret = 0;
			goto out;
		}

		ret = rpdfs_inode_rlock_refresh(inode, RPDFS_RLOCK_MODE_SH_RD, &hold);
		if (ret < 0)
			goto out;

		ret = rpdfs_ehtable_read_items(inode, &ri->dirent_eht, RPDFS_BLOCK_KEY_TYPE_DIRENT,
					       ctx->pos, buf, buf_size);
		if (ret <= 0)
			goto out;
		nr = ret;

		rpdfs_rlock_unlock(rfi, &hold);

		rpdfs_ehtable_for_each_buf_iargs(buf, nr, iargs) {
			if (iargs->val_size != sizeof(struct rpdfs_dirent)) {
				ret = -EUCLEAN;
				goto out;
			}
			ctx->pos = iargs->pos;
			dent = iargs->val;
			if (!dir_emit(ctx, iargs->key, iargs->key_size,
				      rpdfs_inode_presentation(&dent->ino), DT_UNKNOWN) ||
			    ctx->pos == S32_MAX) {
				ret = 0;
				goto out;
			}
			ctx->pos++;
		}
	}

	ret = 0;
out:
	rpdfs_rlock_unlock(rfi, &hold);
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

const struct dentry_operations rpdfs_dentry_ops = {
	.d_revalidate   = rpdfs_d_revalidate,
};
