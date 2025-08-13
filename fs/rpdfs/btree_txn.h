/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BTREE_TXN_H
#define RPDFS_BTREE_TXN_H

#include "btree.h"
#include "txn.h"

int rpdfs_btree_txn_prepare_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_apply_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_prepare_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_apply_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_prepare_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_apply_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_prepare_lookup(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   rpdfs_btree_item_cb_t item_cb, void *cb_arg);
int rpdfs_btree_txn_prepare_copy_items(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				       struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				       void *buf, size_t size);

#endif
