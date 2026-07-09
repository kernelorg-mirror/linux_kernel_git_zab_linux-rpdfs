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
#include "meta.h"
#include "mkfs.h"
#include "pr.h"
#include "rlock.h"
#include "seqlock.h"

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

static void info_ctor(void *obj)
{
	struct rpdfs_inode_info *ri = obj;

	seqlock_init(&ri->refresh_seqlock);
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
	ri->dirents = rinode->dirents;

	ri->xattrs = rinode->xattrs;
	ri->xattr_creates = rinode->xattr_creates;

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
	rinode->dirents = ri->dirents;

	rinode->xattrs = ri->xattrs;
	rinode->xattr_creates = ri->xattr_creates;

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

	while_read_seqretry(&ri->refresh_seqlock)
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

	write_seqlock(&ri->refresh_seqlock);
	if (!ri->refreshed)
		copy_rinode_to_vfs_inode(inode, rinode);
	write_sequnlock(&ri->refresh_seqlock);

	folio_put(folio);
	ret = 0;
out:
	return ret;
}

#if 0
int rpdfs_inode_invalidate(struct super_block *sb, struct rpdfs_inode_nr *ino)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);
	struct inode *inode;
	int ret;

	inode = rpdfs_ilookup(sb, ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ri = RPDFS_I(inode);
	ri->refreshed = false;

	truncate_inode_pages(inode->i_mapping, 0);
	iput(inode);
	ret = 0;
out:
	return ret;
}
#endif

int rpdfs_inode_rlock_refresh(struct inode *inode, u8 mode, struct rpdfs_rlock_hold *hold)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_rlock_key key;
	int ret;

	key.k[0] = ri->ino.i[0];
	key.k[1] = ri->ino.i[1];

	ret = rpdfs_rlock_lock(rfi, &key, mode, hold);
	if (ret == 0) {
		ret = check_refresh_inode(rfi, inode);
		if (ret < 0)
			rpdfs_rlock_unlock(rfi, hold);
	}

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
	struct inode *inode;
	int ret;

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

	/* XXX hack until we have real mkfs in userland */
	if (!sb->s_root && RPDFS_FSINFO_PARAM(rfi, mkfs)) {
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
 * Allocate and insert a I_NEW|I_CREATING inode in the vfs at the given
 * ig.  This can block waiting for other aliases of the ig to drain, or
 * can return errors if a duplicate ig was fully inserted (would
 * indicate inconsistency somewhere).
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
	struct rpdfs_inode_info *ri;

	/* XXX rlock/refresh */

	if (request_mask & STATX_BTIME) {
		ri = RPDFS_I(inode);
		stat->result_mask |= STATX_BTIME;
		stat->btime = ns_to_timespec64(le64_to_cpu(ri->crtime_nsec));
	}

	generic_fillattr(idmap, request_mask, inode, stat);

	return 0;
}

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	int ret;

	/* XXX rlock, checkpoint */

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret == 0) {
		setattr_copy(idmap, inode, attr);
		inode_inc_iversion(inode);
		rpdfs_inode_update(rfi, inode);
	}

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
