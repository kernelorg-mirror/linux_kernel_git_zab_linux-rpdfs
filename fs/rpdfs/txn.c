/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/percpu.h>

#include "balloc.h"
#include "block.h"
#include "pr.h"
#include "txn.h"

/*
 * Each transaction is associated with and is private to an individual
 * task that is making changes to multiple blocks.
 *
 * It makes sure that all the blocks in the transaction have dirty
 * boundaries that cover each other so that they're all written out as
 * one atomic write command on the wire.
 *
 * It provides a context for contiguous allocation.  Each transaction
 * gets an exclusive region so a sequence of allocations will see
 * contiguous blocks while suitable regions are available.
 *
 * Finally, it could help unwinding error paths undo frees with
 * allocations that can't fail.  It could record a pool of block numbers
 * that were freed during the transaction.  But we haven't needed that
 * yet (*fingers crossed*).
 */

void rpdfs_txn_block_dirty(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			   struct rpdfs_block_handle *hnd)
{
	rpdfs_block_dirty(rfi, txn->dirty_bnr, hnd);
	if (txn->dirty_bnr == 0)
		txn->dirty_bnr = hnd->bnr;
}

static int alloc_from_region(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 *bnr)
{
	int ret;

	if (txn->reg == NULL) {
		txn->reg = rpdfs_balloc_take_region(rfi);
		if (IS_ERR(txn->reg)) {
			ret = PTR_ERR(txn->reg);
			txn->reg = NULL;
			goto out;
		}
	}

	ret = rpdfs_balloc_alloc_bnr(txn->reg, bnr);
	if (ret < 0 || txn->reg->nr_set == 0) {
		rpdfs_balloc_free_region(txn->reg);
		txn->reg = NULL;
	}
out:
	return ret;
}

/*
 * Acquire a write handle on a newly allocated block.  Retry while we
 * consume block numbers that aren't still granted and free in the block
 * cache.
 */
int rpdfs_txn_acquire_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			    struct rpdfs_block_handle **hnd)
{
	u64 bnr;
	int ret;

	do {
		ret = alloc_from_region(rfi, txn, &bnr);
		if (ret == 0)
			ret = rpdfs_block_acquire(rfi, bnr, hnd,
						  RBAF_ALLOC | RBAF_WRITE | RBAF_OVERWRITE);
	} while (ret == -ENODATA);

	return ret;
}

/*
 * Freeing a block marks it dirty and updates its details to be sent
 * with a distributed write.
 *
 * To free the caller must have a write handle.  It lets us implement a
 * free that can't fail.  At least, as much as modifying and dirtying
 * can't fail.  Acquiring the handle also revoked other cached blocks
 * across the cluster and prepares the block to be visible to free
 * stripe requests once its details indicate that it's free.
 *
 * The free place ordered after all other block types so that flush
 * won't touch free blocks until the txn has released the write handles
 * on all other blocks in the txn.  This would let the txn satisfy
 * allocations without errors by using blocks that were freed in its
 * txn.  (But that's not implemented..  we'd need some storage in the
 * txn to remember the freed blocks.)
 */
void rpdfs_txn_free_block(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			  struct rpdfs_block_handle *hnd)
{
	/* set place first, dirtying checks place to update alloc_ctr */
	rpdfs_block_set_place(hnd, RPDFS_PLACE_FREE, 0, 0, hnd->bnr);
	rpdfs_txn_block_dirty(rfi, txn, hnd);
}

/*
 * The transaction will not be used again.  Clean up any remaining state
 * so that the caller can destroy it.
 */
void rpdfs_txn_finish(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn)
{
	if (txn->reg)
		rpdfs_balloc_return_region(rfi, txn->reg);
}
