/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BTREE_TXN_H
#define RPDFS_BTREE_TXN_H

#include "format-block.h"
#include "btree.h"
#include "txn.h"

int rpdfs_btree_lookup(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       u64 key, rpdfs_btree_item_cb_t cb, void *arg);
int rpdfs_btree_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 place_hi,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb, void *arg,
		       struct kvec *kv, unsigned nr_segs);
int rpdfs_btree_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 place_hi,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb,
		       void *arg);
int rpdfs_btree_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 place_hi,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb,
		       void *arg);
int rpdfs_btree_read_items(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			   u64 key, rpdfs_btree_item_cb_t cb, void *arg);

#endif
