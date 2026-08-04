/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_KEYS_H
#define RPDFS_KEYS_H

#include "format-block.h"
#include "format-msg.h"

/*
 * Provide translations between the handful of structs that act as keys
 * for various subsystems.  I tried passing input and output structs as
 * values and it created a bunch of friction with other calls that use
 * pointers.  They're all built with memcpy()'s dst = src argument order
 * so we use _from_ to maintain the ordering of dst and src in the
 * names.
 */

static inline void rpdfs_block_key_from_meta(struct rpdfs_block_key *bkey,
					     struct rpdfs_inode_nr *ino, u8 type, u64 t_index)
{
	bkey->k[0] = ino->i[0];
	bkey->k[1] = ino->i[1];
	bkey->k[2] = cpu_to_le64((((u64)type) << RPDFS_BLOCK_KEY_TYPE__SHIFT) |
				 (t_index & RPDFS_BLOCK_KEY_INDEX__MASK));
}

static inline void rpdfs_block_key_from_inode_nr(struct rpdfs_block_key *bkey,
						 struct rpdfs_inode_nr *ino)
{
	rpdfs_block_key_from_meta(bkey, ino, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
}

static inline void rpdfs_rlock_key_from_inode_nr(struct rpdfs_rlock_key *rkey,
						 struct rpdfs_inode_nr *ino)
{
	rkey->k[0] = ino->i[0];
	rkey->k[1] = ino->i[1];
}

static inline void rpdfs_inode_nr_from_rlock_key(struct rpdfs_inode_nr *ino,
						 struct rpdfs_rlock_key *rkey)
{
	ino->i[0] = rkey->k[0];
	ino->i[1] = rkey->k[1];
}

#endif
