/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_TXN_H
#define RPDFS_TXN_H

#include "block.h"
#include "super.h"

struct rpdfx_txn_block;

struct rpdfs_transaction {
	struct rb_root blocks;
	struct list_head writes;
	struct list_head prepared_allocs;
	struct list_head applied_allocs;
	struct rpdfs_txn_block *prep_tblk;
	struct rpdfs_balloc_region *reg;
	struct list_head reg_list;
	unsigned long reg_pos;
	unsigned long force_retry:1,
		      applying:1;
};

#define DECLARE_RPDFS_TXN(name)				\
	struct rpdfs_transaction name = {		\
		.blocks = RB_ROOT,			\
		.writes = LIST_HEAD_INIT(name.writes),	\
		.prepared_allocs = LIST_HEAD_INIT(name.prepared_allocs), \
		.applied_allocs = LIST_HEAD_INIT(name.applied_allocs), \
		.reg_list = LIST_HEAD_INIT(name.reg_list), \
	}

int rpdfs_txn_prepare_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			      struct rpdfs_block_handle **hnd_ret);
void rpdfs_txn_prepare_release(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			       struct rpdfs_block_handle **hnd, rbaf_t rbaf);
int rpdfs_txn_prepare_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			     u64 *bnr_ret);
int rpdfs_txn_apply_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 *bnr_ret);
bool rpdfs_txn_retry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, int *caller_err);
void rpdfs_txn_force_retry(struct rpdfs_transaction *txn);
int rpdfs_txn_use_prepared(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			   struct rpdfs_block_handle **hnd_ret, rbaf_t rbaf);
void rpdfs_txn_reset(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);

#endif
