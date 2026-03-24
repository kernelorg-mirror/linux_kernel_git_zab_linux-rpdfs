/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_TXN_H
#define RPDFS_TXN_H

struct rpdfs_balloc_region;

#include "block.h"
#include "super.h"

struct rpdfs_transaction {
	u64 dirty_bnr;
	struct rpdfs_balloc_region *reg;
};

#define RPDFS_INIT_TXN {0,}

void rpdfs_txn_block_dirty(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			   struct rpdfs_block_handle *hnd);
int rpdfs_txn_acquire_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			    struct rpdfs_block_handle **hnd_ret);
void rpdfs_txn_finish(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);

#endif
