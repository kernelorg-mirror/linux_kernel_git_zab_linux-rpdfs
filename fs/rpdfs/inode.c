/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/writeback.h>
#include <linux/iversion.h>

#include "btree.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "pr.h"

static struct kmem_cache *rpdfs_inode_cache;

struct inode *rpdfs_alloc_inode(struct super_block *sb)
{
	struct rpdfs_inode_info *ri;

	ri = alloc_inode_sb(sb, rpdfs_inode_cache, GFP_KERNEL);
	if (!ri)
		return NULL;

	rpdfs_prd("inode %p ri %p", &ri->vfs_inode, ri);

	return &ri->vfs_inode;
}

void rpdfs_free_inode(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	rpdfs_prd("inode %p ri %p", inode, ri);

	kmem_cache_free(rpdfs_inode_cache, ri);
}

static void info_ctor(void *obj)
{
	struct rpdfs_inode_info *ri = obj;

	seqlock_init(&ri->refresh_seqlock);
	inode_init_once(&ri->vfs_inode);
}

/*
 * This seems awfully sketchy, but this is what's done on 32bit archs or
 * with 32bit ino mount options.  This is generally for presentation
 * only.
 */
static u32 rpdfs_inode_ino32(u64 ino)
{
	u32 i = (u32)ino ^ (ino >> 32);
	if (i <= RPDFS_ROOT_INO)
		i = RPDFS_ROOT_INO + 1;

	return i;
}

static void copy_rinode_to_vfs_inode(struct inode *inode, struct rpdfs_inode *rinode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	ri->ig = rinode->ig;
	i_size_write(inode, le64_to_cpu(rinode->size));
	set_nlink(inode, le32_to_cpu(rinode->nlink));
	i_uid_write(inode, le32_to_cpu(rinode->uid));
	i_gid_write(inode, le32_to_cpu(rinode->gid));
	inode->i_mode = le32_to_cpu(rinode->mode);

	inode_set_atime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->atime_nsec)));
	inode_set_ctime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->ctime_nsec)));
	inode_set_mtime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->mtime_nsec)));
	ri->crtime_nsec = rinode->crtime_nsec;

	ri->dirents = rinode->dirents;

	ri->xattrs = rinode->xattrs;
	ri->xattr_creates = rinode->xattr_creates;
}

static __le64 cpu_ts64_to_le64_ns(struct timespec64 ts)
{
	return cpu_to_le64(timespec64_to_ns(&ts));
}

static void copy_vfs_inode_to_rinode(struct rpdfs_inode *rinode, struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	rinode->ig = ri->ig;
	rinode->size = cpu_to_le64(i_size_read(inode));
	rinode->nlink = cpu_to_le32(inode->i_nlink);
	rinode->uid = cpu_to_le32(i_uid_read(inode));
	rinode->gid = cpu_to_le32(i_gid_read(inode));
	rinode->mode = cpu_to_le32(inode->i_mode);

	rinode->atime_nsec = cpu_ts64_to_le64_ns(inode_get_atime(inode));
	rinode->ctime_nsec = cpu_ts64_to_le64_ns(inode_get_ctime(inode));
	rinode->mtime_nsec = cpu_ts64_to_le64_ns(inode_get_mtime(inode));
	rinode->crtime_nsec = ri->crtime_nsec;

	rinode->dirents = ri->dirents;

	rinode->xattrs = ri->xattrs;
	rinode->xattr_creates = ri->xattr_creates;
}

/*
 * The caller must have initialized i_mode, i_rdev (via reading or
 * init_inode_owner during create).
 */
void rpdfs_inode_init_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &rpdfs_file_iops;
		inode->i_fop = &rpdfs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &rpdfs_dir_iops;
		inode->i_fop = &rpdfs_dir_fops;
		break;
	case S_IFLNK:
		break;
	default:
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}
}

static u64 ts64_to_ns(struct timespec64 ts)
{
	return timespec64_to_ns(&ts);
}

/*
 * Update the vfs inode if the caller's persistent version of the rinode
 * is more recent.  A lot of tasks can be checking a shared inode to see
 * if it should be updated, so we want the negative test to be cheap.
 */
static void rpdfs_inode_refresh(struct rpdfs_fs_info *rfi, struct rpdfs_block_handle *hnd,
				struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_inode *rinode = hnd->data;
	unsigned seq;
	bool refresh;

	do {
		seq = read_seqbegin(&ri->refresh_seqlock);
		refresh = ri->refresh_wcount < hnd->wcount;
	} while (read_seqretry(&ri->refresh_seqlock, seq));

	if (refresh) {
		write_seqlock(&ri->refresh_seqlock);
		if (ri->refresh_wcount < hnd->wcount) {
			rpdfs_prd("ino %llu rw %llu hw %llu rm %llu vm %llu",
				  rpdfs_inode_ino(inode), ri->refresh_wcount, hnd->wcount,
				  le64_to_cpu(rinode->atime_nsec),
				  ts64_to_ns(inode_get_mtime(inode)));
			copy_rinode_to_vfs_inode(inode, rinode);
			ri->refresh_wcount = hnd->wcount;
		}
		write_sequnlock(&ri->refresh_seqlock);
	}
}

/*
 * This only sets the in-memory inode fields (both vfs inode and our
 * inode_info) based on the persistent inode.  We don't need a
 * multi-block transaction to initially populate the vfs inode.  A
 * coherent read handle on the current inode block is sufficient.
 */
static int rpdfs_read_inode(struct rpdfs_fs_info *rfi, struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	u64 bnr = rpdfs_inode_bnr(inode);
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_inode *rinode;
	int ret;

	ret = rpdfs_block_acquire(rfi, bnr, &hnd, 0);
	if (ret < 0)
		goto out;

	rinode = hnd->data;
	rpdfs_prd("ino %llu rw %llu hw %llu rm %llu vm %llu",
		  rpdfs_inode_ino(inode), ri->refresh_wcount, hnd->wcount,
		  le64_to_cpu(rinode->atime_nsec),
		  ts64_to_ns(inode_get_mtime(inode)));
	copy_rinode_to_vfs_inode(inode, rinode);
	ri->refresh_wcount = hnd->wcount;
	ret = 0;
out:
	rpdfs_block_release(rfi, &hnd);

	return ret;
}

static unsigned long ig_hashval(struct rpdfs_ino_gen *ig)
{
#if BITS_PER_LONG == 64
	return ((unsigned long)hash_64_generic(le64_to_cpu(ig->ino), 32) << 32) |
			       hash_64_generic(le64_to_cpu(ig->gen), 32);
#else
	return hash_64_generic(le64_to_cpu(ig->ino), 32) ^
	       hash_64_generic(le64_to_cpu(ig->gen), 32);
#endif
}

static inline int rpdfs_iget_test(struct inode *inode, void *data)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_ino_gen *ig = data;

	return ri->ig.ino == ig->ino && ri->ig.gen == ig->gen;
}

static int rpdfs_iget_set(struct inode *inode, void *data)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_ino_gen *ig = data;

	ri->ig = *ig;
	if (sizeof(ino_t) == sizeof(u32))
		inode->i_ino = rpdfs_inode_ino32(le64_to_cpu(ig->ino));
	else
		inode->i_ino = le64_to_cpu(ig->ino);

	return 0;
}

/*
 * Instantiate a specific ino/gen in the vfs inode cache.  We insert the
 * allocated I_NEW inode in the cache before we've checked that the
 * inode is live.  Users will wait on I_NEW and will find it unhashed by
 * iget_failed as they wake and won't use it.
 */
struct inode *rpdfs_iget(struct super_block *sb, struct rpdfs_ino_gen *ig)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);
	struct inode *inode;
	int ret;

	inode = iget5_locked(sb, ig_hashval(ig), rpdfs_iget_test, rpdfs_iget_set, ig);
	if (!inode) {
		ret = -ENOMEM;
		goto out;
	}

	if (!(inode->i_state & I_NEW)) {
		ret = 0;
		goto out;
	}

	ret = rpdfs_read_inode(rfi, inode);
	if (ret < 0)
		goto out;

	rpdfs_inode_init_ops(inode);
	unlock_new_inode(inode);
	ret = 0;
out:
	if (ret < 0) {
	       if (!IS_ERR_OR_NULL(inode))
			iget_failed(inode);
	       inode = ERR_PTR(ret);
	}

	return inode;
}

/*
 * Allocate and insert a I_NEW|I_CREATING inode in the vfs at the given
 * ig.  This can block waiting for other aliases of the ig to drain, or
 * can return errors if a duplicate ig was fully inserted (would
 * indicate inconsistency somewhere).
 *
 * The caller is entirely responsible for finishing initialization of
 * the inode and either instantiating it or unhashing and dropping it.
 */
struct inode *rpdfs_new_inode(struct super_block *sb, struct rpdfs_ino_gen *ig)
{
	struct rpdfs_inode_info *ri;
	struct timespec64 ts;
	struct inode *inode;
	int ret;

	inode = new_inode(sb);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}

	ri = RPDFS_I(inode);
	ri->refresh_wcount = 0;
	ri->xattr_creates = 0;
	ri->ig = *ig;
	inode->i_ino = le64_to_cpu(ri->ig.ino);

	rpdfs_btree_root_init(&ri->dirents);
	rpdfs_btree_root_init(&ri->xattrs);

	ts = inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, ts);
	inode_set_atime_to_ts(inode, ts);
	ri->crtime_nsec = cpu_ts64_to_le64_ns(ts);

	ret = insert_inode_locked4(inode, ig_hashval(ig), rpdfs_iget_test, ig);
	if (ret < 0) {
		discard_new_inode(inode);
		inode = ERR_PTR(ret);
	}
out:
	return inode;
}

/*
 * Prepare an inode for use in a transaction by adding its block to the
 * transaction.  As we hold the read reference on the block we can
 * update the vfs inode.  This is typically done at the start of
 * transactions so the rest of the transaction can work with the most
 * recent vfs inode fields.
 */
int rpdfs_inode_txn_prepare(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			    struct inode *inode, rbaf_t rbaf)
{
	struct rpdfs_block_handle *hnd = NULL;
	int ret;

	ret = rpdfs_txn_prepare_acquire(rfi, txn, rpdfs_inode_bnr(inode), &hnd);
	if (ret == 0) {
		rpdfs_inode_refresh(rfi, hnd, inode);
		rpdfs_txn_prepare_release(rfi, txn, &hnd, rbaf);
	}

	return ret;
}

/*
 * Update a block that was prepared for writing in the transaction with
 * the current contents of the vfs inode.
 */
void rpdfs_inode_txn_update(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			    struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_inode *rinode;
	int ret;

	ret = rpdfs_txn_use_prepared(rfi, txn, rpdfs_inode_bnr(inode), &hnd, RBAF_WRITE);
	BUG_ON(ret < 0); /* caller must have prepared */

	rinode = hnd->data;
	rpdfs_prd("ino %llu rw %llu hw %llu rm %llu vm %llu",
		  rpdfs_inode_ino(inode), ri->refresh_wcount, hnd->wcount,
		  le64_to_cpu(rinode->atime_nsec),
		  ts64_to_ns(inode_get_mtime(inode)));
	copy_vfs_inode_to_rinode(rinode, inode);
	ri->refresh_wcount = hnd->wcount;
}

/*
 * Start writing a previously dirtied vfs inode.
 *
 * XXX we could have more metadata linking the inode instance and our
 * dirty blocks.
 */
int rpdfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	u64 bnr = rpdfs_inode_bnr(inode);
	int ret;

	/* XXX no idea! */
	if (WARN_ON_ONCE(current->flags & PF_MEMALLOC))
                return 0;

	ret = rpdfs_block_flush(rfi, bnr, wbc->sync_mode == WB_SYNC_ALL);
	if (ret < 0)
		mark_inode_dirty_sync(inode);
	return ret;
}

int rpdfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_block_handle *hnd = NULL;
	DECLARE_RPDFS_TXN(txn);
	int ret;

	do {
		ret = rpdfs_inode_txn_prepare(rfi, &txn, inode, 0);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));

	if (ret < 0)
		goto out;

	if (request_mask & STATX_BTIME) {
		struct rpdfs_inode *rinode;

		ret = rpdfs_txn_use_prepared(rfi, &txn, rpdfs_inode_bnr(inode),
					     &hnd, 0);
		if (ret < 0)
			goto out;

		rinode = hnd->data;

		stat->result_mask |= STATX_BTIME;
		stat->btime = ns_to_timespec64(le64_to_cpu(rinode->crtime_nsec));
	}

	generic_fillattr(idmap, request_mask, inode, stat);

out:
	rpdfs_txn_reset(rfi, &txn);

	return ret;
}

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	DECLARE_RPDFS_TXN(txn);
	int ret;

	do {
		ret = rpdfs_inode_txn_prepare(rfi, &txn, inode, RBAF_WRITE);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));

	if (ret < 0)
		goto out;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		goto out;

	setattr_copy(idmap, inode, attr);
	inode_inc_iversion(inode);

	rpdfs_inode_txn_update(rfi, &txn, inode);

out:
	rpdfs_txn_reset(rfi, &txn);

	return ret;
}

int rpdfs_inode_init(void)
{
	rpdfs_inode_cache = kmem_cache_create("rpdfs_inode_cache", sizeof(struct rpdfs_inode_info),
					      0, SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, info_ctor);
	if (rpdfs_inode_cache == NULL)
		return -ENOMEM;

	return 0;
}

void rpdfs_inode_exit(void)
{
	if (rpdfs_inode_cache) {
		rcu_barrier();
		kmem_cache_destroy(rpdfs_inode_cache);
	}
}
