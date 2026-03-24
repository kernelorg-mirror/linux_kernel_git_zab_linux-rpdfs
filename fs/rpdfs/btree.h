/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BTREE_H
#define RPDFS_BTREE_H

#include "format-block.h"
#include "super.h"

/*
 * A callback for iterating over items.  Iteration continues as long as
 * -ELOOP is returned.  Any other value stops iteration and returns that
 * value.  Some operations (delete) can perform additional work on
 * success before returning.  -ELOOP is never returned to the caller and
 * is translated into an error that makes sense for the operation that
 * iterated over all the items.  (Searches return -ENOENT, reads return
 * 0).
 */
typedef int (*rpdfs_btree_item_cb_t)(struct rpdfs_fs_info *rfi, u64 key, void *val, u16 val_len,
				     void *arg);

void rpdfs_btree_root_init(struct rpdfs_btree_root *root);

void rpdfs_btree_init_first_block(struct rpdfs_btree_root *root, struct rpdfs_block_ref *ref,
				  struct rpdfs_btree_block *bt);
u64 rpdfs_btree_last_key(struct rpdfs_btree_block *bt);

bool rpdfs_btree_must_split(struct rpdfs_btree_block *bt, u16 val_size);
bool rpdfs_btree_should_merge(struct rpdfs_btree_block *bt);
int rpdfs_btree_find_child_ref(struct rpdfs_btree_block *bt, u64 key, u64 *ref_key,
			       struct rpdfs_block_ref *ref);
int rpdfs_btree_find_sib_refs(struct rpdfs_btree_block *bt, u64 key, struct rpdfs_block_ref *left,
			      struct rpdfs_block_ref *right);
void rpdfs_btree_split(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       struct rpdfs_block_ref *par_ref, struct rpdfs_btree_block *parent,
		       struct rpdfs_block_ref *sib_ref, struct rpdfs_btree_block *sib,
		       struct rpdfs_block_ref *ref, struct rpdfs_btree_block *bt);
void rpdfs_btree_merge(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       struct rpdfs_btree_block *parent, struct rpdfs_btree_block *sib,
		       struct rpdfs_btree_block *bt);

int rpdfs_btree_collisions_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			      u64 key, rpdfs_btree_item_cb_t cb, void *arg);
int rpdfs_btree_insert_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt, u64 key,
			  rpdfs_btree_item_cb_t cb, void *arg, struct kvec *kv,
			  unsigned long nr_segs, u16 val_size);
int rpdfs_btree_delete_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			  struct rpdfs_btree_block *bt, u64 key, rpdfs_btree_item_cb_t cb,
			  void *arg);
int rpdfs_btree_items_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt, u64 key,
			 rpdfs_btree_item_cb_t cb, void *arg);

#endif
