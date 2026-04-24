/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_INODE_H
#define RPDFS_INODE_H

#include <linux/fs.h>

struct rpdfs_transaction;

#include "format-block.h"
#include "block.h"
#include "txn.h"

struct rpdfs_inode_info {

	/* updating vfs inode when wcount is older than block contents */
	seqlock_t refresh_seqlock;
	u64 refresh_wcount;

	/* Uniquifier to avoid xattr name hash collisions */
	__le64 xattr_creates;

	/* Creation time is not tracked in VFS inode, do it here */
	__le64 crtime_nsec;

	struct rpdfs_ino_gen ig;
	struct rpdfs_btree_root dirents;
	struct rpdfs_btree_root xattrs;

	struct inode vfs_inode;
};

static inline struct rpdfs_inode_info *RPDFS_I(const struct inode *inode)
{
	return container_of(inode, struct rpdfs_inode_info, vfs_inode);
}

static inline struct rpdfs_ino_gen *rpdfs_inode_ig(struct inode *inode)
{
	return &RPDFS_I(inode)->ig;
}

static inline u64 rpdfs_ino_bnr(u64 bnr)
{
	return bnr;
}

static inline u64 rpdfs_inode_ino(const struct inode *inode)
{
	return le64_to_cpu(RPDFS_I(inode)->ig.ino);
}

static inline u64 rpdfs_inode_bnr(struct inode *inode)
{
	return rpdfs_ino_bnr(rpdfs_inode_ino(inode));
}

struct inode *rpdfs_alloc_inode(struct super_block *sb);
void rpdfs_free_inode(struct inode *inode);
int rpdfs_write_inode(struct inode *inode, struct writeback_control *wbc);

void rpdfs_inode_init_ops(struct inode *inode);

int rpdfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags);

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr);

struct inode *rpdfs_iget(struct super_block *sb, struct rpdfs_ino_gen *ig);
struct inode *rpdfs_new_inode(struct super_block *sb, struct rpdfs_ino_gen *ig);
int rpdfs_inode_acquire_ordered(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *a, struct rpdfs_block_handle **a_hnd,
				struct inode *b, struct rpdfs_block_handle **b_hnd,
				struct inode *c, struct rpdfs_block_handle **c_hnd,
				struct inode *d, struct rpdfs_block_handle **d_hnd, rbaf_t rbaf);
int rpdfs_inode_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct inode *inode, struct rpdfs_block_handle **hnd, rbaf_t rbaf);
void rpdfs_inode_update(struct rpdfs_fs_info *rfi, struct inode *inode,
			struct rpdfs_block_handle *hnd);

int rpdfs_inode_init(void);
void rpdfs_inode_exit(void);

#endif
