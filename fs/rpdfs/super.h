/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SUPER_H
#define RPDFS_SUPER_H

#include <linux/fs.h>
#include <linux/inet.h>
#include <linux/uuid.h>

struct rpdfs_fs_info;

#include "net.h"

struct rpdfs_balloc_info;
struct rpdfs_block_info;
struct rpdfs_map_info;
struct rpdfs_net_info;

struct rpdfs_fs_info {
	u8 client_uuid[UUID_SIZE];
	struct rpdfs_balloc_info *balloc_info;
	struct rpdfs_block_info *block_info;
	struct rpdfs_map_info *map_info;
	struct rpdfs_net_info *net_info;

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
#endif
