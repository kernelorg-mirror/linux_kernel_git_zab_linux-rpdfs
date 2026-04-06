/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/writeback.h>
#include <linux/iversion.h>
#include <linux/sort.h>

#include "btree.h"
#include "compare.h"
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
static void rpdfs_inode_refresh(struct rpdfs_fs_info *rfi, struct inode *inode,
				struct rpdfs_block_handle *hnd)
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

	ret = rpdfs_block_acquire(rfi, NULL, bnr, &hnd, 0);
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

struct inode_hnd {
	struct inode *inode;
	struct rpdfs_block_handle **hnd;
};

/* NULL inode goes to the end */
static int cmp_inode_ino(const void *a, const void *b)
{
	const struct inode_hnd *ihnd_a = a;
	const struct inode_hnd *ihnd_b = b;

	return rpdfs_compare(ihnd_a->inode ? rpdfs_inode_ino(ihnd_a->inode) : U64_MAX,
			     ihnd_b->inode ? rpdfs_inode_ino(ihnd_b->inode) : U64_MAX);
}

/*
 * Acquire handles on the blocks for the given inodes in the appropriate
 * order.  After acquiring each block handle make sure that the inode
 * (and our private into) are current with the contents of the block.
 * NULL inode arguments and their matching handle are ignored.
 *
 * On success all the non-null handles have been acquired and must be
 * released by the caller.
 */
int rpdfs_inode_acquire_ordered(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *a, struct rpdfs_block_handle **a_hnd,
				struct inode *b, struct rpdfs_block_handle **b_hnd,
				struct inode *c, struct rpdfs_block_handle **c_hnd,
				struct inode *d, struct rpdfs_block_handle **d_hnd, rbaf_t rbaf)
{
	struct inode_hnd sorted[4] = {
		{ a, a_hnd }, { b, b_hnd }, { c, c_hnd }, { d, d_hnd }
	};
	int ret;
	int i;

	sort(sorted, ARRAY_SIZE(sorted), sizeof(sorted[0]), cmp_inode_ino, NULL);

	ret = 0;
	for (i = 0; i < ARRAY_SIZE(sorted); i++) {
		if (!sorted[i].inode)
			break;

		ret = rpdfs_block_acquire(rfi, txn, rpdfs_inode_bnr(sorted[i].inode),
					  sorted[i].hnd, rbaf);
		if (ret < 0) {
			while (i-- > 0)
				rpdfs_block_release(rfi, sorted[i].hnd);
			break;
		}
		rpdfs_inode_refresh(rfi, sorted[i].inode, *sorted[i].hnd);
	}

	return ret;
}

/*
 * Acquire a handle on the block containing the inode.  Once acquired,
 * make sure the inode matches the current contents of the block.
 */
int rpdfs_inode_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *inode, struct rpdfs_block_handle **hnd, rbaf_t rbaf)
{
	int ret;

	ret = rpdfs_block_acquire(rfi, txn, rpdfs_inode_bnr(inode), hnd, rbaf);
	if (ret == 0)
		rpdfs_inode_refresh(rfi, inode, *hnd);

	return ret;
}

/*
 * The caller has modified the inode.  Update the block contents in the
 * handle to match the inode (and our private info).
 *
 * It's often the case that callers must be careful to call when they're
 * returning errors.  We have patterns where modifications can update a
 * structure stored in a block that the path isn't aware of (btree
 * roots) and not be able to dirty the block that contains the root.
 * Callers must assume that the root was modified, even when errors are
 * returned.  If we didn't do this then references and allocated block
 * uses could get out of sync.
 *
 * It's safe to call this with a null/err handle so that it can be
 * unconditionally called in exit paths regardless of whether the inode
 * handle was acquired.
 */
void rpdfs_inode_update(struct rpdfs_fs_info *rfi, struct inode *inode,
			struct rpdfs_block_handle *hnd)
{
	struct rpdfs_inode_info *ri;
	struct rpdfs_inode *rinode;

	if (IS_ERR_OR_NULL(hnd))
		return;

	ri = RPDFS_I(inode);
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
	struct rpdfs_inode_info *ri;
	int ret;

	ret = rpdfs_inode_acquire(rfi, NULL, inode, &hnd, 0);
	if (ret < 0)
		goto out;

	if (request_mask & STATX_BTIME) {
		ri = RPDFS_I(inode);
		stat->result_mask |= STATX_BTIME;
		stat->btime = ns_to_timespec64(le64_to_cpu(ri->crtime_nsec));
	}

	generic_fillattr(idmap, request_mask, inode, stat);
out:
	rpdfs_block_release(rfi, &hnd);

	return ret;
}

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_transaction txn = RPDFS_INIT_TXN;
	struct rpdfs_block_handle *hnd = NULL;
	int ret;

	ret = rpdfs_inode_acquire(rfi, &txn, inode, &hnd, RBAF_WRITE);
	if (ret < 0)
		goto out;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret == 0) {
		setattr_copy(idmap, inode, attr);
		inode_inc_iversion(inode);
		rpdfs_inode_update(rfi, inode, hnd);
	}

out:
	rpdfs_block_release(rfi, &hnd);
	rpdfs_txn_finish(rfi, &txn);

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
