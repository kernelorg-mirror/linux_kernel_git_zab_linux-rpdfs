/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_EHTABLE_H
#define RPDFS_EHTABLE_H

#include "format-block.h"
#include "super.h"

/*
 * key, key_size, and hash are always set to specify the key.
 *
 * val and val_size can be set to specify an inserting value or to get a
 * copy of a value from lookup.
 *
 * pos is only set in the iargs returned from reading items.
 */
struct rpdfs_ehtable_item_args {
	const void *key;
	void *val;
	u32 hash;
	u32 pos;
	size_t key_size;
	size_t val_size;
};

enum {
	/* return -EEXIST if an item with the key already exists */
	RPDFS_EHT_EEXIST = 1 << 0,
	/* return -ENOENT if an item with the key does not exist */
	RPDFS_EHT_ENOENT = 1 << 1,
};

int rpdfs_ehtable_lookup(struct inode *inode, struct rpdfs_ehtable_desc *desc,
			 u8 type, struct rpdfs_ehtable_item_args *iargs);
int rpdfs_ehtable_insert(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			 struct rpdfs_ehtable_item_args *iargs);
int rpdfs_ehtable_set(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
		      struct rpdfs_ehtable_item_args *iargs, int flags);
int rpdfs_ehtable_delete(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			 struct rpdfs_ehtable_item_args *iargs);

#define rpdfs_ehtable_for_each_buf_iargs(buf, nr, iargs) \
	for (__typeof__(iargs) _end_ = (iargs = (buf)) + (nr); iargs < _end_; iargs++)
int rpdfs_ehtable_read_items(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			     u32 pos, void *buf, size_t size);

#endif
