/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BALLOC_H
#define RPDFS_BALLOC_H

#include "txn.h"

int rpdfs_balloc_prepare_alloc_peek(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				    u64 *bnr_ret);
int rpdfs_balloc_prepare_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);
void rpdfs_balloc_reset_prepare(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);
void rpdfs_balloc_start_apply(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn);
int rpdfs_balloc_apply_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			     u64 *bnr_ret);

int rpdfs_balloc_free_meta(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr);

int rpdfs_balloc_setup(struct rpdfs_fs_info *rfi);
void rpdfs_balloc_destroy(struct rpdfs_fs_info *rfi);

#endif
