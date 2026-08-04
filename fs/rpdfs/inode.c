/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/writeback.h>
#include <linux/iversion.h>
#include <linux/xxhash.h>

#include "aops.h"
#include "compare.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "keys.h"
#include "meta.h"
#include "mkfs.h"
#include "pr.h"
#include "rlock.h"
#include "seqlock.h"

struct rpdfs_inode_sb_info {
	struct workqueue_struct *workq;
};

static struct rpdfs_inode_sb_info *RPDFS_IINF(struct rpdfs_fs_info *rfi)
{
	return rfi->inode_sb_info;
}

static void SET_RPDFS_IINF(struct rpdfs_fs_info *rfi, struct rpdfs_inode_sb_info *iinf)
{
	rfi->inode_sb_info = iinf;
}

/*
 * We're using the cheesy local id for now.  This would use the client
 * id assigned by quorumd.  And we might want it for block keys in
 * general, not just inode numbers.
 */
void rpdfs_alloc_inode_nr(struct rpdfs_fs_info *rfi, struct rpdfs_inode_nr *ino)
{
	memcpy(&ino->i[0], rfi->client_uuid, sizeof(ino->i[0]));
	ino->i[1] = cpu_to_le64(atomic64_inc_return(&rfi->next_free_inode_nr));
}

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

/*
 * Our large 128bit inode numbers don't fit in 32/64 i_ino.  We use a
 * strong hash to try and provide a consistent and reasonably unique
 * presentation of the full inode number in i_ino.  In a given file
 * population we'll probably avoid collisions.
 *
 * This is only for presentation -- stat() output, traces, etc.  The
 * system internally always uses the large inode numbers for references.
 */
ino_t rpdfs_inode_presentation(const struct rpdfs_inode_nr *ino)
{
	return xxh64(ino, sizeof(struct rpdfs_inode_nr), 0);
}

void rpdfs_free_inode(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	rpdfs_prd("inode %p ri %p", inode, ri);

	kmem_cache_free(rpdfs_inode_cache, ri);
}

void rpdfs_evict_inode(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	rpdfs_prd("inode %p ri %p", inode, ri);

	rpdfs_meta_evict_shadow(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void invalidate_inode_worker(struct work_struct *work);
static void info_ctor(void *obj)
{
	struct rpdfs_inode_info *ri = obj;

	seqlock_init(&ri->seqlock);
	INIT_WORK(&ri->invalidate_work, invalidate_inode_worker);
	inode_init_once(&ri->vfs_inode);
}

static void copy_rinode_to_vfs_inode(struct inode *inode, struct rpdfs_inode *rinode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	ri->ino = rinode->ino;
	i_size_write(inode, le64_to_cpu(rinode->size));
	set_nlink(inode, le32_to_cpu(rinode->nlink));
	i_uid_write(inode, le32_to_cpu(rinode->uid));
	i_gid_write(inode, le32_to_cpu(rinode->gid));
	inode->i_mode = le32_to_cpu(rinode->mode);

	inode_set_atime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->atime_nsec)));
	inode_set_ctime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->ctime_nsec)));
	inode_set_mtime_to_ts(inode, ns_to_timespec64(le64_to_cpu(rinode->mtime_nsec)));
	ri->crtime_nsec = rinode->crtime_nsec;

	ri->dirent_eht = rinode->dirent_eht;
	ri->xattr_eht = rinode->xattr_eht;

	ri->refreshed = true;
}

static __le64 cpu_ts64_to_le64_ns(struct timespec64 ts)
{
	return cpu_to_le64(timespec64_to_ns(&ts));
}

static void copy_vfs_inode_to_rinode(struct rpdfs_inode *rinode, struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	rinode->ino = ri->ino;
	rinode->size = cpu_to_le64(i_size_read(inode));
	rinode->nlink = cpu_to_le32(inode->i_nlink);
	rinode->uid = cpu_to_le32(i_uid_read(inode));
	rinode->gid = cpu_to_le32(i_gid_read(inode));
	rinode->mode = cpu_to_le32(inode->i_mode);

	rinode->atime_nsec = cpu_ts64_to_le64_ns(inode_get_atime(inode));
	rinode->ctime_nsec = cpu_ts64_to_le64_ns(inode_get_ctime(inode));
	rinode->mtime_nsec = cpu_ts64_to_le64_ns(inode_get_mtime(inode));
	rinode->crtime_nsec = ri->crtime_nsec;

	rinode->dirent_eht = ri->dirent_eht;
	rinode->xattr_eht = ri->xattr_eht;

	ri->refreshed = true;
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
		inode->i_mapping->a_ops = &rpdfs_aops;
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
 * Make sure that the inode is in sync with the most recent persistent
 * version in its inode block.  Typically this is a cheap test of the
 * inode info.  But we might have to read the block and make sure that
 * we serialize with others doing the same.
 */
static int check_refresh_inode(struct rpdfs_fs_info *rfi, struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_inode *rinode;
	struct folio *folio;
	bool refreshed;
	int ret;

	while_read_seqretry(&ri->seqlock)
		refreshed = ri->refreshed;
	if (refreshed) {
		ret = 0;
		goto out;
	}

	folio = rpdfs_meta_get_folio(inode, MGF_READ, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}
	rinode = folio_address(folio);

	write_seqlock(&ri->seqlock);
	if (!ri->refreshed)
		copy_rinode_to_vfs_inode(inode, rinode);
	write_sequnlock(&ri->seqlock);

	folio_put(folio);
	ret = 0;
out:
	return ret;
}

/*
 * We invalidate all cached blocks before releasing the rlock because
 * users don't try and revalidate on use.  There's a good chance we
 * could leave caches behind and trigger revalidation.  We'd still have
 * network round-trips but the responses could be a lot smaller if the
 * revalidating cached version was still current.
 */
static void invalidate_inode_worker(struct work_struct *work)
{
	struct rpdfs_inode_info *ri = container_of(work, struct rpdfs_inode_info, invalidate_work);
	struct inode *inode = &ri->vfs_inode;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_rlock_key key;

	if (ri->invalidate_only_flush) {
		filemap_fdatawrite(inode->i_mapping);
		filemap_fdatawrite(ri->shadow_inode->i_mapping);
		/* XXX io errors */
		filemap_fdatawait(ri->shadow_inode->i_mapping);
		filemap_fdatawait(inode->i_mapping);
	} else {
		filemap_invalidate_inode(inode, true, 0, LLONG_MAX);
		filemap_invalidate_inode(ri->shadow_inode, true, 0, LLONG_MAX);
	}

#if 0 /* XXX NYI */
	/* can't touch during unmount, dcache destroys w/o locks */
	d_prune_aliases(inode);
#endif

	rpdfs_rlock_key_from_inode_nr(&key, &ri->ino);
	rpdfs_rlock_invalidate_finished(rfi, &key);

	iput(inode);
}

/*
 * The rlock caller is revoking a mode that protected caches in the
 * inode.  We always have to flush and might need to also drop the cache
 * if we're losing read mode coverage.  The flush/invalidation itself
 * can be very heavy so we hand it off to async work.
 *
 * We need to be careful with the inode lookup variant here.  We have to
 * wait for writeback during evict so we have to wait for inodes to
 * clear I_WILL_FREE.
 *
 * Returns true if the inode was invalidated during the call.  Returns
 * false if the invalidation is now pending and
 * rpdfs_rlock_invalidate_finished() will be called later.
 */
bool rpdfs_inode_invalidate(struct super_block *sb, struct rpdfs_inode_nr *ino, bool only_flush)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);
	struct rpdfs_inode_sb_info *iinf = RPDFS_IINF(rfi);
	struct rpdfs_iget_data igd = { *ino, false };
	struct rpdfs_inode_info *ri;
	struct inode *inode;

	inode = rpdfs_ilookup(sb, &igd);
	if (!inode)
		return true;

	ri = RPDFS_I(inode);

	write_seqlock(&ri->seqlock);
	ri->refreshed = false;
	ri->invalidate_counter++;
	ri->invalidate_only_flush = only_flush;
	write_sequnlock(&ri->seqlock);

	/* the worker calls the iput for our ilookup */
	queue_work(iinf->workq, &ri->invalidate_work);
	return false;
}

u64 rpdfs_inode_invalidate_counter(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	u64 ctr;

	while_read_seqretry(&ri->seqlock)
		ctr = ri->invalidate_counter;

	return ctr;
}

int rpdfs_inode_cmp_inos(struct rpdfs_inode_nr *a, struct rpdfs_inode_nr *b)
{
	return rpdfs_compare(le64_to_cpu(a->i[0]), le64_to_cpu(b->i[0])) ?:
	       rpdfs_compare(le64_to_cpu(a->i[1]), le64_to_cpu(b->i[1]));
}

int rpdfs_inode_rlock_ino(struct rpdfs_fs_info *rfi, struct rpdfs_inode_nr *ino, u8 mode,
			  struct rpdfs_rlock_hold *hold)
{
	struct rpdfs_rlock_key key;

	rpdfs_rlock_key_from_inode_nr(&key, ino);
	return rpdfs_rlock_lock(rfi, &key, mode, hold);
}

int rpdfs_inode_rlock_refresh(struct inode *inode, u8 mode, struct rpdfs_rlock_hold *hold)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	int ret;

	ret = rpdfs_inode_rlock_ino(rfi, &ri->ino,  mode, hold);
	if (ret == 0) {
		ret = check_refresh_inode(rfi, inode);
		if (ret < 0)
			rpdfs_rlock_unlock(rfi, hold);
	}

	return ret;
}

/*
 * Returns true if a is greater than b.
 */
static bool ino_greater(struct rpdfs_inode_nr *a, struct rpdfs_inode_nr *b)
{
	if (le64_to_cpu(a->i[0]) > le64_to_cpu(b->i[0]))
		return true;
	if (le64_to_cpu(a->i[0]) < le64_to_cpu(b->i[0]))
		return false;

	return le64_to_cpu(a->i[1]) > le64_to_cpu(b->i[1]);
}

/*
 * Returns true if a's inode_nr is greater than b's, allowing for either
 * or both to be null.  Nulls are considered as pointers to a maxmimum
 * value inode so that sorting with this puts nulls at the end.
 */
static bool inode_greater_nulls(struct inode *a, struct inode *b)
{
	if (a && b)
		return ino_greater(rpdfs_inode_ino(a), rpdfs_inode_ino(b));

	return a > b;
}

/*
 * Acquire the rlock for all the inodes in inode/rlock_key order and
 * make sure they're refreshed.  Any of the inode pointers can be null.
 * We use a quick sort network to sort the pointers by their referenced
 * number, moving nulls to the end.  I hope we all enjoy conditional
 * branches.
 *
 * If this returns an error then non of the rlocks will be held.
 */
#define CMP_SWAP(INA, HOA, INB, HOB) \
do { \
	if (inode_greater_nulls(INA, INB)) { \
		swap(INA, INB); \
		swap(HOA, HOB); \
	} \
} while (0)
int rpdfs_inode_rlock_refresh_many(struct inode *in_a, struct rpdfs_rlock_hold *ho_a,
				   struct inode *in_b, struct rpdfs_rlock_hold *ho_b,
				   struct inode *in_c, struct rpdfs_rlock_hold *ho_c,
				   struct inode *in_d, struct rpdfs_rlock_hold *ho_d, u8 mode)
{
	struct rpdfs_fs_info *rfi;
	int ret;

	/* 01, 23, 02, 13, 12 */
	CMP_SWAP(in_a, ho_a, in_b, ho_b);
	CMP_SWAP(in_c, ho_c, in_d, ho_d);
	CMP_SWAP(in_a, ho_a, in_c, ho_c);
	CMP_SWAP(in_b, ho_b, in_d, ho_d);
	CMP_SWAP(in_b, ho_b, in_c, ho_c);
#undef CMP_SWAP

	if (!in_a) {
		ret = 0;
		goto out;
	}
	rfi = RPDFS_INODE_FS(in_a);

	ret = (in_a ? rpdfs_inode_rlock_refresh(in_a, mode, ho_a) : 0) ?:
	      (in_b ? rpdfs_inode_rlock_refresh(in_b, mode, ho_b) : 0) ?:
	      (in_c ? rpdfs_inode_rlock_refresh(in_c, mode, ho_c) : 0) ?:
	      (in_d ? rpdfs_inode_rlock_refresh(in_d, mode, ho_d) : 0);
	if (ret < 0) {
		rpdfs_rlock_unlock(rfi, ho_a);
		rpdfs_rlock_unlock(rfi, ho_b);
		rpdfs_rlock_unlock(rfi, ho_c);
		rpdfs_rlock_unlock(rfi, ho_d);
	}
out:
	return ret;
}

/*
 * This only sets the in-memory inode fields (both vfs inode and our
 * inode_info) based on the persistent inode.
 */
static int rpdfs_read_inode(struct rpdfs_fs_info *rfi, struct inode *inode)
{
//	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_inode *rinode;
	struct folio *folio;
	int ret;

	folio = rpdfs_meta_get_folio(inode, MGF_READ, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	rinode = folio_address(folio);

	rpdfs_prd("ino "RIF" rm %llu vm %llu", RIA(inode), le64_to_cpu(rinode->atime_nsec),
		  ts64_to_ns(inode_get_mtime(inode)));
	copy_rinode_to_vfs_inode(inode, rinode);

	folio_put(folio);
	ret = 0;
out:
	return ret;
}


/*
 * This is fed into further hashing in the inode cache so we're just
 * trying to smush our larger identity down into the long.
 */
static unsigned long iget_hashval(struct rpdfs_iget_data *igd)
{
	return hash_64(le64_to_cpu(igd->ino.i[0]), BITS_PER_LONG) ^
	       hash_64((le64_to_cpu(igd->ino.i[1]) << 1) + !igd->is_shadow, BITS_PER_LONG);
}

static inline int rpdfs_iget_test(struct inode *inode, void *data)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_iget_data *igd = data;

	return ri->ino.i[0] == igd->ino.i[0] && ri->ino.i[1] == igd->ino.i[1] &&
	       ri->is_shadow == igd->is_shadow;
}

static int rpdfs_iget_set(struct inode *inode, void *data)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_iget_data *igd = data;

	ri->ino = igd->ino;
	ri->is_shadow = igd->is_shadow;
	inode->i_ino = rpdfs_inode_presentation(&igd->ino);

	return 0;
}

/*
 * Instantiate a specific inode_nr in the vfs inode cache.  We insert
 * the allocated I_NEW inode in the cache before we've checked that the
 * inode is live.  Users will wait on I_NEW and will find it unhashed by
 * iget_failed as they wake and won't use it.
 */
struct inode *rpdfs_iget(struct super_block *sb, struct rpdfs_inode_nr *ino)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);
	struct rpdfs_iget_data igd = { *ino, false };
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	struct inode *inode;
	bool is_mkfs;
	u8 mode;
	int ret;

	/* XXX hack until we have real mkfs in userland */
	is_mkfs = !sb->s_root && RPDFS_FSINFO_PARAM(rfi, mkfs);
	mode = is_mkfs ? RPDFS_RLOCK_MODE_EX_WR : RPDFS_RLOCK_MODE_SH_RD;

	ret = rpdfs_inode_rlock_ino(rfi, ino, mode, &hold);
	if (ret < 0)
		goto out;

	inode = iget5_locked(sb, iget_hashval(&igd), rpdfs_iget_test, rpdfs_iget_set, &igd);
	if (!inode) {
		ret = -ENOMEM;
		goto out;
	}

	if (!(inode->i_state & I_NEW)) {
		ret = 0;
		goto out;
	}

	ret = rpdfs_meta_alloc_shadow(inode);
	if (ret < 0)
		goto out;

	if (is_mkfs) {
		ret = rpdfs_mkfs_root_inode(inode);
		if (ret < 0)
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

	rpdfs_rlock_unlock(rfi, &hold);

	return inode;
}

/*
 * This is used by io completion messages to finish reads/writeback on
 * folios in the metadata shadow inode.  It's not being used by actual
 * inode operations.
 */
struct inode *rpdfs_find_inode_rcu(struct super_block *sb, struct rpdfs_iget_data *igd)
{
	return find_inode_rcu(sb, iget_hashval(igd), rpdfs_iget_test, igd);
}

/*
 * This is called by invalidation to find inodes whose caches need to be
 * dropped.  It waits for I_NEW and I_FREEING to clear which prevents
 * inode holders with those states (iget, evict) from blocking on
 * rlocks.
 */
struct inode *rpdfs_ilookup(struct super_block *sb, struct rpdfs_iget_data *igd)
{
	return ilookup5(sb, iget_hashval(igd), rpdfs_iget_test, igd);
}

/*
 * Allocate and insert a I_NEW|I_CREATING inode in the vfs at the given
 * inode number.  This can block waiting for other aliases of the inode
 * to drain, or can return errors if a duplicate was fully inserted
 * (would indicate inconsistency somewhere).
 *
 * The caller is entirely responsible for finishing initialization of
 * the inode and either instantiating it or unhashing and dropping it.
 */
struct inode *rpdfs_new_inode(struct super_block *sb, struct rpdfs_iget_data *igd)
{
	struct inode *inode = NULL;
	struct rpdfs_inode_info *ri;
	struct rpdfs_inode *rinode;
	struct timespec64 ts;
	struct folio *folio;
	int ret;

	inode = new_inode(sb);
	if (!inode) {
		ret = -ENOMEM;
		goto out;
	}

	rpdfs_iget_set(inode, igd);

	ri = RPDFS_I(inode);
	ri->refreshed = true;
	ri->xattr_creates = 0;

	ts = inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, ts);
	inode_set_atime_to_ts(inode, ts);
	ri->crtime_nsec = cpu_ts64_to_le64_ns(ts);

	ret = insert_inode_locked4(inode, iget_hashval(igd), rpdfs_iget_test, igd);
	if (ret < 0)
		goto out;

	/* done if we're allocating a shadow */
	if (igd->is_shadow)
		goto out;

	/* otherwise we're a primary inode and need shadow and new block folio */
	ret = rpdfs_meta_alloc_shadow(inode);
	if (ret < 0)
		goto out;

	folio = rpdfs_meta_get_folio(inode, MGF_NEW, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	rinode = folio_address(folio);
	copy_vfs_inode_to_rinode(rinode, inode);
	folio_zero_segment(folio, sizeof(struct rpdfs_inode), RPDFS_BLOCK_SIZE);
	folio_mark_uptodate(folio);
	rpdfs_meta_dirty_folio(folio);
	folio_unlock(folio);
	folio_put(folio);
	ret = 0;
out:
	if (ret < 0) {
		if (inode) {
			if (inode_unhashed(inode))
				iput(inode);
			else
				iget_failed(inode);
		}
		inode = ERR_PTR(ret);
	}
	return inode;
}

/*
 * The caller has modified the vfs inode and/or our inode_info.  Copy
 * them into a dirty block.
 */
void rpdfs_inode_update(struct rpdfs_fs_info *rfi, struct inode *inode)
{
//	struct rpdfs_inode_info *ri;
	struct rpdfs_inode *rinode;
	struct folio *folio;

	folio = rpdfs_meta_get_folio(inode, MGF_WRITE, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
	/* XXX we'll probably want to pre-alloc/pin so this can't fail */
	BUG_ON(IS_ERR_OR_NULL(folio));

	rinode = folio_address(folio);

	rpdfs_prd("ino "RIF" rm %llu vm %llu", RIA(inode), le64_to_cpu(rinode->atime_nsec),
		  ts64_to_ns(inode_get_mtime(inode)));
	copy_vfs_inode_to_rinode(rinode, inode);

	rpdfs_meta_dirty_folio(folio);
	folio_unlock(folio);
	folio_put(folio);
}

int rpdfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	struct rpdfs_inode_info *ri;
	int ret;

	ret = rpdfs_inode_rlock_refresh(inode, RPDFS_RLOCK_MODE_SH_RD, &hold);
	if (ret < 0)
		goto out;

	if (request_mask & STATX_BTIME) {
		ri = RPDFS_I(inode);
		stat->result_mask |= STATX_BTIME;
		stat->btime = ns_to_timespec64(le64_to_cpu(ri->crtime_nsec));
	}

	generic_fillattr(idmap, request_mask, inode, stat);
	ret = 0;
out:
	rpdfs_rlock_unlock(rfi, &hold);
	return ret;
}

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	DECLARE_RPDFS_RLOCK_HOLD(hold);
	int ret;

	ret = rpdfs_inode_rlock_refresh(inode, RPDFS_RLOCK_MODE_EX_WR, &hold);
	if (ret < 0)
		goto out;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret == 0) {
		setattr_copy(idmap, inode, attr);
		inode_inc_iversion(inode);
		rpdfs_inode_update(rfi, inode);
	}
out:
	rpdfs_rlock_unlock(rfi, &hold);
	return ret;
}

int rpdfs_inode_sb_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_inode_sb_info *iinf = NULL;
	int ret;

	iinf = kzalloc(sizeof(struct rpdfs_inode_sb_info), GFP_NOFS);
	if (iinf)
		iinf->workq = alloc_workqueue("rpdfs-inode-invalidate",
					      WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!iinf || !iinf->workq) {
		ret = -ENOMEM;
		goto out;
	}

	SET_RPDFS_IINF(rfi, iinf);
	ret = 0;
out:
	if (ret < 0)
		rpdfs_inode_sb_destroy(rfi);
	return ret;
}

void rpdfs_inode_sb_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_inode_sb_info *iinf = RPDFS_IINF(rfi);

	if (iinf) {
		if (iinf->workq)
			destroy_workqueue(iinf->workq);
		kfree(iinf);
		SET_RPDFS_IINF(rfi, NULL);
	}
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
