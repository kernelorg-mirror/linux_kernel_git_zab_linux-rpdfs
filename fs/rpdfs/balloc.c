/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/atomic.h>

#include "format-block.h"
#include "balloc.h"
#include "block.h"
#include "pr.h"
#include "super.h"
#include "txn.h"

/*
 * We don't have block allocators yet.  Every mount assumes it's the
 * only mount and is starting with a fresh system.  Allocations advance
 * from the first possible block and frees are ignored.
 */
struct rpdfs_balloc_info {
	atomic64_t next_bnr;
};

static inline struct rpdfs_balloc_info *RPDFS_ALINF(struct rpdfs_fs_info *rfi)
{
	return rfi->balloc_info;
}

static inline void SET_RPDFS_ALINF(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_info *alinf)
{
	rfi->balloc_info = alinf;
}

/*
 * When we get allocator blocks this will look ahead in prepared
 * allocator blocks to prepare write references to allocated blocks.
 * Until then we just grab a chunk of the per-mount allocator atomic.
 *
 * It's tricky to use the peeked value.  The caller needs to be sure to
 * perform the apply calls in the same sequence as they consumed the
 * peeked values.  Ideally callers will be few and obvious (only the
 * first peek/apply in the txn).
 */
#define ALLOCS_PER_TXN 32
int rpdfs_balloc_prepare_alloc_peek(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				    u64 *bnr_ret)
{
	struct rpdfs_balloc_info *alinf = RPDFS_ALINF(rfi);
	struct rpdfs_block_handle *hnd = NULL;
	u64 bnr;
	int ret;

	if (txn->alloc_base == 0) {
		txn->alloc_base = atomic64_add_return(ALLOCS_PER_TXN, &alinf->next_bnr) -
				  ALLOCS_PER_TXN;
		txn->alloc_nr = ALLOCS_PER_TXN;
	}

	if (WARN_ON_ONCE(txn->alloc_used == txn->alloc_nr)) {
		ret = -ENOSPC;
		goto out;
	}

	bnr = txn->alloc_base + txn->alloc_used;

	ret = rpdfs_txn_prepare_acquire(rfi, txn, bnr, &hnd);
	if (ret == 0) {
		rpdfs_txn_prepare_release(rfi, txn, &hnd, RBAF_WRITE | RBAF_OVERWRITE);
		txn->alloc_used++;
		*bnr_ret = bnr;
	}

out:
	if (ret < 0)
		*bnr_ret = 0;

	rpdfs_prd("base %llu used %llu nr %llu bnr %llu ret %d",
		  txn->alloc_base, txn->alloc_used, txn->alloc_nr, *bnr_ret, ret);

	return ret;
}

int rpdfs_balloc_prepare_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn)
{
	u64 ignored;
	return rpdfs_balloc_prepare_alloc_peek(rfi, txn, &ignored);
}

void rpdfs_balloc_reset_prepare(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn)
{
	txn->alloc_used = 0;
}

void rpdfs_balloc_start_apply(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn)
{
	txn->alloc_nr = txn->alloc_used;
	txn->alloc_used = 0;
}

/*
 * Apply and return the result of an allocation that was prepared by
 * _prepare_alloc.  The caller can use _txn_use_prepare() to get the
 * allocated block.
 */
int rpdfs_balloc_apply_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			     u64 *bnr_ret)
{
	int ret;

	if (WARN_ON_ONCE(txn->alloc_base == 0 || txn->alloc_used == txn->alloc_nr)) {
		*bnr_ret = 0;
		ret = -ENOSPC;
	} else {
		*bnr_ret = txn->alloc_base + txn->alloc_used++;
		ret = 0;
	}

	rpdfs_prd("base %llu used %llu nr %llu bnr %llu ret %d",
		  txn->alloc_base, txn->alloc_used, txn->alloc_nr, *bnr_ret, ret);

	return ret;
}

int rpdfs_balloc_free_meta(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr)
{
	return 0;
}

int rpdfs_balloc_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_balloc_info *alinf;
	int ret;

	alinf = kzalloc(sizeof(struct rpdfs_balloc_info), GFP_KERNEL);
	if (!alinf) {
		ret = -ENOMEM;
		goto out;
	}

	atomic64_set(&alinf->next_bnr, RPDFS_ROOT_INO + 1);

	SET_RPDFS_ALINF(rfi, alinf);
	ret = 0;
out:
	return ret;
}

void rpdfs_balloc_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_balloc_info *alinf = RPDFS_ALINF(rfi);

	if (alinf) {
		SET_RPDFS_ALINF(rfi, NULL);
		kfree(alinf);
	}
}
