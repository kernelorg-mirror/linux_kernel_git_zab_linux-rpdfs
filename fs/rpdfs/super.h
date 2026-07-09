/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SUPER_H
#define RPDFS_SUPER_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/inet.h>
#include <linux/uuid.h>

struct rpdfs_fs_info;

#include "params.h"

struct rpdfs_balloc_info;
struct rpdfs_block_info;
struct rpdfs_map_info;
struct rpdfs_net_info;
struct rpdfs_preq_info;
struct rpdfs_rlock_info;

struct rpdfs_fs_info {
	struct super_block *sb;
	u8 client_uuid[UUID_SIZE];
	struct rpdfs_params params;
	atomic64_t next_free_inode_nr;

	struct rpdfs_balloc_info *balloc_info;
	struct rpdfs_block_info *block_info;
	struct rpdfs_map_info *map_info;
	struct rpdfs_net_info *net_info;
	struct rpdfs_preq_info *preq_info;
	struct rpdfs_rlock_info *rlock_info;

	struct percpu_counter pc_creates;
};

static inline struct rpdfs_fs_info *RPDFS_SB_FS(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct rpdfs_fs_info *RPDFS_INODE_FS(struct inode *inode)
{
	return RPDFS_SB_FS(inode->i_sb);
}

static inline struct rpdfs_fs_info *RPDFS_DENTRY_FS(struct dentry *dentry)
{
	return RPDFS_INODE_FS(d_inode(dentry));
}

static inline struct rpdfs_fs_info *RPDFS_MAPPING_FS(struct address_space *mapping)
{
	return RPDFS_INODE_FS(mapping->host);
}

static inline struct rpdfs_fs_info *RPDFS_FOLIO_FS(struct folio *folio)
{
	return RPDFS_INODE_FS(folio_inode(folio));
}

#endif
