/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_TXN_H
#define RPDFS_TXN_H

#include "block.h"
#include "super.h"

struct rpdfx_txn_block;

struct rpdfs_transaction {
	struct rb_root blocks;
	struct list_head writes;
	struct rpdfs_txn_block *prep_tblk;
	unsigned long force_retry:1;
	u64 alloc_base;
	u64 alloc_used;
	u64 alloc_nr;
};

#define DECLARE_RPDFS_TXN(name)				\
	struct rpdfs_transaction name = {		\
		.blocks = RB_ROOT,			\
		.writes = LIST_HEAD_INIT(name.writes),	\
	}

int rpdfs_txn_prepare_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			      struct rpdfs_block_handle **hnd_ret);
void rpdfs_txn_prepare_release(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			       struct rpdfs_block_handle **hnd, rbaf_t rbaf);
bool rpdfs_txn_retry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, int *caller_err);
void rpdfs_txn_force_retry(struct rpdfs_transaction *txn);
int rpdfs_txn_use_prepared(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			   struct rpdfs_block_handle **hnd_ret, rbaf_t rbaf);
void rpdfs_txn_reset(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);

#endif
