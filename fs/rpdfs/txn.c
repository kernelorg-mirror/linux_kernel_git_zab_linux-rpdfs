/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/percpu.h>

#include "balloc.h"
#include "block.h"
#include "pr.h"
#include "super.h"
#include "txn.h"

struct rpdfs_txn_block {
	struct rb_node node;
	struct list_head write_head;
	struct list_head alloc_head;
	struct rpdfs_block_handle *hnd;
	struct rpdfs_balloc_region *reg;
	u64 wcount;
	u64 bnr;
	rbaf_t rbaf;
	bool prepared;
};

/*
 * Return a tblk at the given bnr, allocating a new tblk if one wasn't
 * already present.  The wcount is only used to initialize an allocated
 * tblk so that callers can always test it.
 */
static struct rpdfs_txn_block *get_tblk(struct rb_root *root, u64 bnr, u64 wcount, bool alloc)
{
	struct rb_node *parent = NULL;
	struct rb_node **link = &root->rb_node;
	struct rpdfs_txn_block *tblk = NULL;

	while (*link) {
		parent = *link;
		tblk = container_of(*link, struct rpdfs_txn_block, node);

		if (bnr < tblk->bnr)
			link = &(*link)->rb_left;
		else if (bnr > tblk->bnr)
			link = &(*link)->rb_right;
		else
			break;
		tblk = NULL;
	}

	if (!tblk && alloc) {
		tblk = kzalloc(sizeof(struct rpdfs_txn_block), GFP_NOFS);
		if (!tblk) {
			tblk = ERR_PTR(-ENOMEM);
		} else  {
			INIT_LIST_HEAD(&tblk->write_head);
			INIT_LIST_HEAD(&tblk->alloc_head);
			tblk->bnr = bnr;
			tblk->wcount = wcount;

			rb_link_node(&tblk->node, parent, link);
			rb_insert_color(&tblk->node, root);
		}
	}

	return tblk;
}

/*
 * Preparing the blocks for a transaction involves examining each block
 * individually with a read ref.  Examining the block can determine if
 * the transaction wants to read or write the block.  The release of
 * these preparing acquisition can specify the rbaf flags that will be
 * used to properly acquire all the blocks to apply the transaction.
 *
 * As we first acquire each block we record its version.  On future
 * acquisitions we return an error if it's changed.  The caller will
 * stop and attempt to retry rather than proceeding with inconsistent
 * blocks.
 */
int rpdfs_txn_prepare_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			      struct rpdfs_block_handle **hnd_ret)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_txn_block *tblk;
	int ret;

	if (WARN_ON_ONCE(txn->prep_tblk) ||
	    WARN_ON_ONCE(*hnd_ret != NULL)) {
		ret = -EDEADLK;
		goto out;
	}

	ret = rpdfs_block_acquire(rfi, bnr, &hnd, 0);
	if (ret < 0)
		goto out;

	tblk = get_tblk(&txn->blocks, bnr, hnd->wcount, true);
	if (IS_ERR(tblk)) {
		ret = PTR_ERR(tblk);
		goto out;
	}

	if (hnd->wcount != tblk->wcount) {
		ret = -EAGAIN;
		goto out;
	}

	txn->prep_tblk = tblk;
	tblk->hnd = hnd;
	tblk->prepared = true;
	*hnd_ret = hnd;
	ret = 0;
out:
	if (ret < 0) {
		rpdfs_block_release(rfi, &hnd);
		*hnd_ret = NULL;
	}

	rpdfs_prd("txn %p bnr %llu ret %d", txn, bnr, ret);
	return ret;
}

/*
 * As the caller releases a reference during preparation it can specify
 * additional flags to use when finally acquiring the block for apply.
 * Typically this lets prepare decide that it will want to modify a
 * block during apply after examining the contents of the block.  (f.e.
 * discovering that a btree block is full and needs to be split.)
 */
void rpdfs_txn_prepare_release(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			       struct rpdfs_block_handle **hnd, rbaf_t rbaf)
{
	struct rpdfs_txn_block *tblk;

	if (WARN_ON_ONCE(*hnd == NULL) ||
	    WARN_ON_ONCE(txn->prep_tblk == NULL))
		return;

	rpdfs_prd("txn %p bnr %llu rbaf 0x%x", txn, (*hnd)->bnr, rbaf);

	tblk = txn->prep_tblk;
	if (!WARN_ON_ONCE(tblk->hnd != *hnd)) {
		tblk->rbaf |= rbaf;
		tblk->hnd = NULL;
		txn->prep_tblk = NULL;
	}

	rpdfs_block_release(rfi, hnd);
}

/*
 * Prepare a block allocation.  On success, a writable block will be
 * available during apply.
 *
 * If bnr_ret is provided then the caller is given the bnr and they will
 * call _txn_use_prepared to get the returned block during apply.  This
 * is rarely used when the caller must prepare resources before apply
 * that depend on the bnr that will be allocated (allocating vfs inode
 * resources f.e.).  Block numbers returned in this way are considered
 * allocated during applied and so must be used or they will be lost.
 *
 * If bnr_ret is NULL then the block is stored in the txn and the caller
 * will use _txn_apply_alloc which returns the next prepared block
 * during apply.  This is typically used when the allocated bnr is only
 * used during apply itself.  It is safe to prepare more of these blocks
 * than are applied.
 */
int rpdfs_txn_prepare_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 *bnr_ret)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_txn_block *tblk;
	u64 bnr;
	int ret;

	for (;;) {
		if (txn->reg == NULL) {
			/* get region from balloc via block cache messaging */
			txn->reg = rpdfs_balloc_take_region(rfi);
			if (IS_ERR(txn->reg)) {
				ret = PTR_ERR(txn->reg);
				txn->reg = NULL;
				goto out;
			}
			txn->reg_pos = 0;
			list_add_tail(&txn->reg->head, &txn->reg_list);
		}

		ret = rpdfs_balloc_find_next(txn->reg, &txn->reg_pos, &bnr);
		if (ret < 0) {
			txn->reg = NULL;
			/* retry after consuming regions, hard enospc comes from block/balloc */
			if (ret == -ENOSPC)
				continue;
			goto out;
		}

		ret = rpdfs_block_acquire(rfi, bnr, &hnd, RBAF_ALLOC | RBAF_WRITE | RBAF_OVERWRITE);
		if (ret < 0) {
			/* bnr might not still be free, clear and try again with next */
			if (ret == -ENODATA) {
				rpdfs_balloc_clear(txn->reg, bnr);
				continue;
			}
			goto out;
		}
		break;
	}

	tblk = get_tblk(&txn->blocks, bnr, hnd->wcount, true);
	if (IS_ERR(tblk)) {
		ret = PTR_ERR(tblk);
		goto out;
	}

	if (hnd->wcount != tblk->wcount) {
		ret = -EAGAIN;
		goto out;
	}

	/* only initial has _ALLOC restrictions, retry can block, wcount protects reuse */
	tblk->prepared = true;
	tblk->rbaf = RBAF_WRITE | RBAF_OVERWRITE;
	tblk->reg = txn->reg;

	if (bnr_ret) {
		*bnr_ret = hnd->bnr;
		list_add_tail(&tblk->alloc_head, &txn->applied_allocs);
	} else {
		list_add_tail(&tblk->alloc_head, &txn->prepared_allocs);
	}
	ret = 0;
out:
	rpdfs_block_release(rfi, &hnd);
	return ret;
}

int rpdfs_txn_apply_alloc(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 *bnr_ret)
{
	struct rpdfs_txn_block *tblk;
	int ret;

	if (WARN_ON_ONCE(!txn->applying))
		return -EINVAL;

	tblk = list_first_entry_or_null(&txn->prepared_allocs, struct rpdfs_txn_block, alloc_head);
	if (tblk) {
		*bnr_ret = tblk->bnr;
		list_move_tail(&tblk->alloc_head, &txn->applied_allocs);
		ret = 0;
	} else {
		ret = -ENOSPC;
	}

	return ret;
}

/* XXX might be nice to get this upstream? */
#define rbtree_for_each_entry_safe(pos, n, root, field)						\
	for (pos = rb_entry_safe(rb_first(root), typeof(*pos), field);				\
	     pos && ({ n = rb_entry_safe(rb_next(&pos->field), typeof(*pos), field); 1; });	\
	     pos = n)

static void free_tblk(struct rb_root *root, struct rpdfs_txn_block *tblk)
{
	if (!IS_ERR_OR_NULL(tblk)) {
		if (!RB_EMPTY_NODE(&tblk->node))
			rb_erase(&tblk->node, root);
		if (!list_empty(&tblk->write_head))
			list_del_init(&tblk->write_head);
		if (!list_empty(&tblk->alloc_head))
			list_del_init(&tblk->alloc_head);
		kfree(tblk);
	}
}

static struct rpdfs_block_handle *tblk_block_entry_handle_fn(struct list_head *pos)
{
	struct rpdfs_txn_block *tblk = list_entry(pos, struct rpdfs_txn_block, write_head);

	return tblk->hnd;
}

typedef enum {
	RTF_RELEASE	= (1 << 0),
	RTF_CLEAR_PREP	= (1 << 1),
	RTF_FREE	= (1 << 2),
	RTF_STOP_AT	= (1 << 3),
} rtf_t;

static void reset_txn(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		      rtf_t rtf, struct rpdfs_txn_block *stop)
{
	struct rpdfs_balloc_region *reg;
	struct rpdfs_balloc_region *_reg_;
	struct rpdfs_txn_block *tblk;
	struct rpdfs_txn_block *_tblk_;

	rbtree_for_each_entry_safe(tblk, _tblk_, &txn->blocks, node) {
		if (!list_empty(&tblk->write_head))
			list_del_init(&tblk->write_head);
		if (!list_empty(&tblk->alloc_head))
			list_del_init(&tblk->alloc_head);

		tblk->reg = NULL;

		if (rtf & RTF_RELEASE)
			rpdfs_block_release(rfi, &tblk->hnd);
		if (rtf & RTF_CLEAR_PREP)
			tblk->prepared = false;
		if (rtf & RTF_FREE)
			free_tblk(&txn->blocks, tblk);
		if ((rtf & RTF_STOP_AT) && tblk == stop)
			break;
	}

	txn->reg = NULL;
	list_for_each_entry_safe_reverse(reg, _reg_, &txn->reg_list, head) {
		list_del_init(&reg->head);
		rpdfs_balloc_return_region(rfi, reg);
	}

	txn->applying = 0;
}

/*
 * Called after the caller has initialized and prepared block references
 * in the transaction.  We try and acquire all the prepared references
 * for the caller to use to apply their transaction.
 *
 * Returns true if the prepared references were successfully acquired.
 * The caller can then use the prepared block references to apply the
 * transaction.  The txn holds the acquired references and
 * _txn_destroy() must be called later to release the references.  The
 * caller can also trust an error found during preparation in this case
 * and return their error without applying the transaction.
 *
 * Returns false if the references could not be acquired and the caller
 * should retry.  In this case the caller can not trust any side-effects
 * from preparing the blocks, including potential errors.
 *
 * If there's an error that prevents acquiring the references, this can
 * return false and set the caller's error value.  In this case the
 * blocks have not been acquired and the transaction can not be applied.
 * We have to be careful to not cause spurious errors by trying to
 * acquire references from inconsistent prepared blocks.  So this only
 * happens when all the acquired references were either successful or
 * saw errors.  We can't return an acquisition error if any of the
 * references had to be retried.
 *
 * In summary:
 *  - return false, caller_err == 0: use references to apply transaction
 *  - return false, caller_err < 0: return error (we may have set), can't use references
 *  - return true: retry preparation, ignore any prepare outputs including caller_err
 */
bool rpdfs_txn_retry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, int *caller_err)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_txn_block *tblk;
	struct rpdfs_txn_block *_tblk_;
	bool retry;
	int err;
	int ret;

	if (txn->force_retry) {
		retry = 1;
		err = 0;
		goto out;
	}

restart:
	retry = 0;
	err = 0;

	rbtree_for_each_entry_safe(tblk, _tblk_, &txn->blocks, node) {
		if (!tblk->prepared) {
			free_tblk(&txn->blocks, tblk);
			continue;
		}

		ret = rpdfs_block_acquire(rfi, tblk->bnr, &hnd, tblk->rbaf | RBAF_NONBLOCK_MODE |
					  RBAF_NONBLOCK_FLUSH);
		if (ret < 0) {
			if (ret == -EAGAIN) {
				reset_txn(rfi, txn, RTF_RELEASE | RTF_STOP_AT, tblk);
				ret = rpdfs_block_acquire(rfi, tblk->bnr, &hnd, tblk->rbaf);
				if (ret == 0)
					rpdfs_block_release(rfi, &hnd);
				goto restart;
			}
			if (err == 0)
				err = ret;
			continue;
		}

		if (hnd->wcount != tblk->wcount) {
			rpdfs_block_release(rfi, &hnd);
			retry = 1;
			goto out;
		}

		tblk->hnd = hnd;
		hnd = NULL;

		/* gather all write refs for dirtying */
		if (tblk->rbaf & RBAF_WRITE)
			list_add_tail(&tblk->write_head, &txn->writes);
	}

	if (err == 0) {
		txn->applying = 1;
		if (!list_empty(&txn->writes))
			rpdfs_block_make_dirty(rfi, &txn->writes, tblk_block_entry_handle_fn);
	}
out:
	txn->force_retry = 0;

	if (retry) {
		reset_txn(rfi, txn, RTF_RELEASE | RTF_CLEAR_PREP, NULL);
	} else {
		if (err < 0 && *caller_err == 0)
			*caller_err = err;
	}

	return retry;
}

/*
 * The next _retry will reset the txn and return true.
 */
void rpdfs_txn_force_retry(struct rpdfs_transaction *txn)
{
	txn->force_retry = 1;
}

/*
 * The caller is applying the transaction and is using the block
 * references that were prepared and then acquired.
 */
int rpdfs_txn_use_prepared(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			   struct rpdfs_block_handle **hnd_ret, rbaf_t rbaf)
{
	struct rpdfs_txn_block *tblk;
	int ret;

	tblk = get_tblk(&txn->blocks, bnr, 0, false);
	if (WARN_ON_ONCE(IS_ERR_OR_NULL(tblk))) {
		ret = -EINVAL;
		goto out;
	}

	rpdfs_prd("bnr %llu rbaf 0x%x tblk %p [prep %u rbaf 0x%x]",
		  bnr, rbaf, tblk, tblk ? tblk->prepared : 0, tblk ? tblk->rbaf : 0);

	/* caller must have prepared and with _WRITE if needed */
	if (WARN_ON_ONCE(!tblk->prepared || ((rbaf & RBAF_WRITE) && !(tblk->rbaf & RBAF_WRITE)))) {
		ret = -EINVAL;
		goto out;
	}

	*hnd_ret = tblk->hnd;
	ret = 0;
out:
	if (ret < 0)
		*hnd_ret = NULL;
	return ret;
}

/*
 * Clear state associated with a txn.  Can be called on newly
 * initialized txns and repeatedly.
 *
 * Suitable after applying to prepare again or for teardown with the
 * caller responsible for the txn memory itself (likely on-stack).
 */
void rpdfs_txn_reset(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn)
{
	struct rpdfs_txn_block *tblk;
	struct rpdfs_txn_block *_tblk_;

	if (txn->applying) {
		list_for_each_entry_safe(tblk, _tblk_, &txn->applied_allocs, alloc_head)
			rpdfs_balloc_clear(tblk->reg, tblk->bnr);
	}

	reset_txn(rfi, txn, RTF_RELEASE | RTF_FREE, NULL);
}
