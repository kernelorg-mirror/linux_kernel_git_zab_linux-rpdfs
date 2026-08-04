/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_INODE_H
#define RPDFS_INODE_H

#include <linux/fs.h>
#include <linux/pagemap.h>

struct rpdfs_transaction;
struct rpdfs_rlock_hold;

#include "format-block.h"
#include "super.h"

struct rpdfs_iget_data {
	struct rpdfs_inode_nr ino;
	bool is_shadow;
};

#define RIF "%llu.%llu:%u"
#define RIA(inode) \
	le64_to_cpu(RPDFS_I(inode)->ino.i[0]), le64_to_cpu(RPDFS_I(inode)->ino.i[1]), \
	RPDFS_I(inode)->is_shadow

struct rpdfs_inode_info {

	/* updating vfs inode after acquiring rlock */
	seqlock_t refresh_seqlock;
	bool refreshed;

	/* Uniquifier to avoid xattr name hash collisions */
	__le64 xattr_creates;

	/* Creation time is not tracked in VFS inode, do it here */
	__le64 crtime_nsec;

	struct rpdfs_inode_nr ino;
	struct inode *shadow_inode;
	bool is_shadow;

	struct rpdfs_ehtable_desc dirent_eht;
	struct rpdfs_ehtable_desc xattr_eht;

	struct inode vfs_inode;
};

static inline struct rpdfs_inode_info *RPDFS_I(const struct inode *inode)
{
	return container_of(inode, struct rpdfs_inode_info, vfs_inode);
}

static inline struct rpdfs_inode_info *RPDFS_FOLIO_I(struct folio *folio)
{
	return RPDFS_I(folio_inode(folio));
}

static inline struct rpdfs_inode_nr rpdfs_inode_ino(struct inode *inode)
{
	return RPDFS_I(inode)->ino;
}

void rpdfs_alloc_inode_nr(struct rpdfs_fs_info *rfi, struct rpdfs_inode_nr *ino);
ino_t rpdfs_inode_presentation(const struct rpdfs_inode_nr *ino);

struct inode *rpdfs_alloc_inode(struct super_block *sb);
void rpdfs_free_inode(struct inode *inode);
void rpdfs_evict_inode(struct inode *inode);

void rpdfs_inode_init_ops(struct inode *inode);

int rpdfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags);

int rpdfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr);

struct inode *rpdfs_iget(struct super_block *sb, struct rpdfs_inode_nr *ino);
struct inode *rpdfs_find_inode_rcu(struct super_block *sb, struct rpdfs_iget_data *igd);
struct inode *rpdfs_new_inode(struct super_block *sb, struct rpdfs_iget_data *igd);
int rpdfs_inode_rlock_refresh(struct inode *inode, u8 mode, struct rpdfs_rlock_hold *hold);
int rpdfs_inode_rlock_refresh_many(struct inode *in_a, struct rpdfs_rlock_hold *ho_a,
				   struct inode *in_b, struct rpdfs_rlock_hold *ho_b,
				   struct inode *in_c, struct rpdfs_rlock_hold *ho_c,
				   struct inode *in_d, struct rpdfs_rlock_hold *ho_d, u8 mode);
void rpdfs_inode_update(struct rpdfs_fs_info *rfi, struct inode *inode);

int rpdfs_inode_init(void);
void rpdfs_inode_exit(void);

#endif
