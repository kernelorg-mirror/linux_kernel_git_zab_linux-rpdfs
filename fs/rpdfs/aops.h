/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DATA_H
#define RPDFS_DATA_H

#include "super.h"

void rpdfs_block_key_init(struct rpdfs_block_key *key, struct rpdfs_inode_nr *ino,
			  u8 type, u64 t_index);

extern const struct address_space_operations rpdfs_aops;

int rpdfs_aops_setup(struct rpdfs_fs_info *rfi);
void rpdfs_aops_destroy(struct rpdfs_fs_info *rfi);

#endif

