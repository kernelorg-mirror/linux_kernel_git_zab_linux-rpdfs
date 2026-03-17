/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BTREE_H
#define RPDFS_BTREE_H

#include "format-block.h"
#include "super.h"

/*
 * This is a convenience to gather the item arguments into one pointer
 * for callers in to and out of the btree API.
 */
struct rpdfs_btree_item_args {
	struct rpdfs_btree_key key;
	void *val;
	u16 val_size;
};

/*
 * A function provided to API calls that lets the btree call back out to
 * the caller to describe actions that should be taken based on a set
 * of items.
 * The specific calling convention depends on the call.
 */
typedef int (*rpdfs_btree_item_cb_t)(struct rpdfs_fs_info *rfi, struct rpdfs_btree_item_args *a,
				     struct rpdfs_btree_item_args *b,
				     struct rpdfs_btree_item_args *c, void *fn_arg);

void rpdfs_btree_root_init(struct rpdfs_btree_root *root);

void rpdfs_btree_init_first_block(struct rpdfs_btree_root *root, struct rpdfs_block_ref *ref,
				  struct rpdfs_btree_block *bt);

bool rpdfs_btree_should_split(struct rpdfs_btree_block *bt, u16 val_size);
bool rpdfs_btree_should_merge(struct rpdfs_btree_block *bt, u16 val_size);
int rpdfs_btree_find_child_ref(struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
			       struct rpdfs_block_ref *ref);
int rpdfs_btree_find_child_and_sib_ref(struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
				       struct rpdfs_block_ref *ref,
				       struct rpdfs_block_ref *sib_ref);
int rpdfs_btree_split(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		      struct rpdfs_block_ref *par_ref, struct rpdfs_btree_block *parent,
		      struct rpdfs_block_ref *sib_ref, struct rpdfs_btree_block *sib,
		      struct rpdfs_block_ref *ref, struct rpdfs_btree_block *bt);
int rpdfs_btree_merge(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		      struct rpdfs_btree_block *par_bt, struct rpdfs_btree_block *sib_bt,
		      struct rpdfs_btree_block *bt);

void rpdfs_btree_key_set_min(struct rpdfs_btree_key *key);
bool rpdfs_btree_key_is_min(struct rpdfs_btree_key *key);
void rpdfs_btree_key_set_max(struct rpdfs_btree_key *key);
bool rpdfs_btree_key_is_max(struct rpdfs_btree_key *key);
void rpdfs_btree_key_inc(struct rpdfs_btree_key *key);
int rpdfs_btree_key_cmp(const struct rpdfs_btree_key *a, const struct rpdfs_btree_key *b);

int rpdfs_btree_lookup_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg);
int rpdfs_btree_insert_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg);
int rpdfs_btree_delete_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			  struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
			  rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_modify_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg);

int rpdfs_btree_copy_items(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			   struct rpdfs_btree_key *key, void *buf, size_t size);
struct rpdfs_btree_item_args *rpdfs_btree_next_copied_item(struct rpdfs_btree_item_args *bti);

#endif
