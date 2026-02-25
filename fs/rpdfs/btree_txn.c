/* SPDX-License-Identifier: GPL-2.0 */

#include "balloc.h"
#include "block.h"
#include "btree.h"
#include "btree_txn.h"
#include "btree_txn.h"
#include "format-block.h"
#include "pr.h"

/*
 * While the core btree.c functions operate on the structure of the
 * btree blocks themselves, this performs the block coordination above
 * that.  It manages tree traversal, block IO, allocation, etc, in the
 * context of transactions.  It knows how to prepare blocks such that
 * atomic modifications must succeed.
 *
 * The model is kept simple by drastically limiting the combinations of
 * operations that can be performed.  In one transaction we can only
 * have two individual item changes per btree, and they must be
 * different -- insertion and deletion.  This puts a bounds on the
 * possible combinations of leaf splitting and merging that the apply
 * functions have to support.  We can still perform inline modification
 * without having to understand the aggregate changes.
 */

typedef enum {
	RBT_OP_INSERT,
	RBT_OP_DELETE,
	RBT_OP_MODIFY,
} rbt_op_t;

static int btree_txn_prepare_read(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				  struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				  struct rpdfs_block_handle **hnd)
{
	struct rpdfs_btree_block *bt;
	struct rpdfs_block_ref ref;
	int level;
	int ret;

	if (root->height == 0) {
		ret = -ENOENT;
		goto out;
	}

	level = root->height;
	ref = root->ref;

	while (--level >= 0) {
		ret = rpdfs_txn_prepare_acquire(rfi, txn, le64_to_cpu(ref.bnr), hnd);
		if (ret < 0)
			goto out;

		bt = (*hnd)->data;

		if (level > 0) {
			ret = rpdfs_btree_find_child_ref(bt, key, &ref);
			rpdfs_txn_prepare_release(rfi, txn, hnd, 0);
			if (ret < 0)
				goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

/*
 * Prepare blocks needed to modify a leaf.  Usually we'll acquire read
 * refs to parents and a write ref on the leaf containing the key.  If
 * we have to split or merge blocks then we'll prepare write refs on the
 * blocks, parents, and maybe siblings as needed.
 *
 * So far, we've gotten away with only supporting a single insert and
 * deletion to a given btree in a transaction.  This lets us treat
 * insertion and deletion mostly independently.  A given block can only
 * fill or fall under the low water mark, never both.
 */
static int btree_txn_prepare_write(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   rbt_op_t op, unsigned leaf_val_size)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_block_ref next_sib_ref;
	struct rpdfs_block_ref par_ref;
	struct rpdfs_block_ref sib_ref;
	struct rpdfs_block_ref ref;
	struct rpdfs_btree_block *bt;
	unsigned val_size;
	bool par_write;
	bool sib_write;
	rbaf_t rbaf;
	int nr_allocs;
	int level;
	int ret;

	rpdfs_prd("root %llu h %u", le64_to_cpu(root->ref.bnr), root->height);

	if (root->height == 0) {
		if (op != RBT_OP_INSERT) {
			ret = -ENOENT;
			goto out;
		}

		ret = rpdfs_txn_prepare_alloc(rfi, txn, NULL);
		if (ret < 0)
			goto out;
	}

	level = root->height;
	ref = root->ref;
	val_size = sizeof(struct rpdfs_block_ref);
	par_ref = (struct rpdfs_block_ref){0,};
	sib_ref = (struct rpdfs_block_ref){0,};

	while (--level >= 0) {
		par_write = false;
		sib_write = false;
		nr_allocs = 0;

		rpdfs_prd("level %d ref %llu", level, le64_to_cpu(ref.bnr));

		ret = rpdfs_txn_prepare_acquire(rfi, txn, le64_to_cpu(ref.bnr), &hnd);
		if (ret < 0)
			goto out;

		rbaf = level == 0 ? RBAF_WRITE : 0;
		val_size = level > 0 ?  sizeof(struct rpdfs_block_ref) : leaf_val_size;
		bt = hnd->data;

		if (op == RBT_OP_INSERT && rpdfs_btree_should_split(bt, val_size)) {
			rbaf = RBAF_WRITE;
			par_write = true;
			nr_allocs += 1 + !par_ref.bnr;

		} else if (op == RBT_OP_DELETE && sib_ref.bnr &&
			   rpdfs_btree_should_merge(bt, val_size)) {
			rbaf = RBAF_WRITE;
			par_write = true;
			sib_write = true;
		}

		/*
		 * XXX If we're preparing an insertion into a leaf with
		 * a single block we might apply after a deletion that
		 * freed the block.  A more complete allocator would
		 * satisfy that allocation from the deletion's free of
		 * the block, but we don't have that yet.  So we add an
		 * extra allocation which is often never applied.  This
		 * can be removed once the allocator can satisfy
		 * allocations from frees in a txn.
		 */
		if (op == RBT_OP_INSERT && level == 0 && le16_to_cpu(bt->nr_items) == 1)
			nr_allocs++;

		if (level > 0)
			ret = rpdfs_btree_find_child_and_sib_ref(bt, key, &ref, &next_sib_ref);
		else
			ret = 0;

		rpdfs_txn_prepare_release(rfi, txn, &hnd, rbaf);
		if (ret < 0)
			goto out;

		while (nr_allocs-- > 0) {
			ret = rpdfs_txn_prepare_alloc(rfi, txn, NULL);
			if (ret < 0)
				goto out;
		}

		if (par_write && par_ref.bnr) {
			ret = rpdfs_txn_prepare_acquire(rfi, txn, le64_to_cpu(par_ref.bnr), &hnd);
			if (ret < 0)
				goto out;
			rpdfs_txn_prepare_release(rfi, txn, &hnd, RBAF_WRITE);
		}
		if (sib_write) {
			ret = rpdfs_txn_prepare_acquire(rfi, txn, le64_to_cpu(sib_ref.bnr), &hnd);
			if (ret < 0)
				goto out;
			rpdfs_txn_prepare_release(rfi, txn, &hnd, RBAF_WRITE);
		}

		par_ref = ref;
		sib_ref = next_sib_ref;
	}

	ret = 0;
out:
	rpdfs_prd("ret %d", ret);
	return ret;
}

/*
 * Apply a prepared allocation and initialize the block ref with its
 * bnr.
 */
static int apply_alloc_ref(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			   struct rpdfs_block_ref *ref)
{
	u64 bnr;
	int ret;

	ret = rpdfs_txn_apply_alloc(rfi, txn, &bnr);
	if (ret == 0) {
		/* XXX will have more metadata, probably? */
		*ref = (struct rpdfs_block_ref) {
		       .bnr = cpu_to_le64(bnr),
		};
	}

	return ret;
}

/*
 * We split to the left so that we don't have to change the max key of
 * the existing block (which might be the max key).
 */
static int apply_split(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
		       struct rpdfs_block_ref *par_ref, struct rpdfs_block_ref *ref,
		       struct rpdfs_block_handle **hnd)
{
	struct rpdfs_block_handle *par_hnd = NULL;
	struct rpdfs_block_handle *sib_hnd = NULL;
	struct rpdfs_btree_block *sib_bt;
	struct rpdfs_block_ref sib_ref;
	int ret;

	/*
	 * XXX allocation can't fail.  That means memory allocation
	 * can't fail.  We'll need help with the block cache to arrange
	 * for inserting blocks with preallocated buffers.
	 */

	ret = (!par_ref->bnr ? apply_alloc_ref(rfi, txn, par_ref) : 0) ?:
	      apply_alloc_ref(rfi, txn, &sib_ref) ?:
	      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(par_ref->bnr), &par_hnd, RBAF_WRITE) ?:
	      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(sib_ref.bnr), &sib_hnd, RBAF_WRITE) ?:
	      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(ref->bnr), hnd, RBAF_WRITE) ?:
	      rpdfs_btree_split(rfi, root, par_ref, par_hnd->data, &sib_ref, sib_hnd->data,
			        ref, (*hnd)->data);
	if (ret == 0) {
		sib_bt = sib_hnd->data;
		if (rpdfs_btree_key_cmp(key, &sib_bt->last) <= 0) {
			*ref = sib_ref;
			*hnd = sib_hnd;
		}
	}

	return ret;
}

/*
 * Merge a block's items with items from a sibling.  The common case is
 * only redistribution where the block just pulls enough items from the
 * sibling to bring them both over the minimum item number.  We can
 * empty the sibling if both blocks would be left under the minimum.
 * This frees the sibling, which removes a parent item, which can leave
 * the parent with a single item and remove it from the tree.  The core
 * btree call is updating item and root refs as blocks are freed, we're
 * only responsible for the freeing in the allocator.
 */
static int apply_merge(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, struct rpdfs_block_ref *par_ref,
		       struct rpdfs_block_ref *sib_ref, struct rpdfs_block_ref *ref,
		       struct rpdfs_block_handle *hnd)
{
	struct rpdfs_block_handle *par_hnd = NULL;
	struct rpdfs_block_handle *sib_hnd = NULL;
	struct rpdfs_btree_block *par_bt;
	struct rpdfs_btree_block *sib_bt;
	int ret;

	ret = rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(par_ref->bnr), &par_hnd, RBAF_WRITE) ?:
	      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(sib_ref->bnr), &sib_hnd, RBAF_WRITE) ?:
	      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(ref->bnr), &hnd, RBAF_WRITE) ?:
	      rpdfs_btree_merge(rfi, root, par_hnd->data, sib_hnd->data, hnd->data);
	if (ret == 0) {
		sib_bt = sib_hnd->data;
		if (sib_bt->nr_items == 0) {
			ret = rpdfs_balloc_free_meta(rfi, txn, le64_to_cpu(sib_ref->bnr));
			par_bt = par_hnd->data;
			if (le16_to_cpu(par_bt->nr_items) == 1 && ret == 0)
				ret = rpdfs_balloc_free_meta(rfi, txn, le64_to_cpu(par_ref->bnr));
		}
	}

	return ret;
}

/*
 * Return a write handle on the leaf needed to perform the operation.
 * We'll allocate, split, and merge along the way.
 *
 * This relies on the block references that were previously prepared.
 * Once we're applying a transaction we can't unwind so any errors
 * returned amount to catastrophic corruption.  (They should only stem
 * from bad logic bugs or, say, memory corruption.)
 */
static int btree_txn_apply_write(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 rbt_op_t op, unsigned leaf_val_size,
				 struct rpdfs_block_handle **hnd_ret)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_block_ref par_ref;
	struct rpdfs_block_ref sib_ref;
	struct rpdfs_block_ref ref;
	struct rpdfs_btree_block *bt;
	unsigned val_size;
	rbaf_t rbaf;
	int level;
	int ret;

	rpdfs_prd("root %llu h %u", le64_to_cpu(root->ref.bnr), root->height);

	if (root->height == 0) {
		if (op != RBT_OP_INSERT) {
			ret = -ENOENT;
			goto out;
		}
		ret = apply_alloc_ref(rfi, txn, &ref) ?:
		      rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(ref.bnr), &hnd, RBAF_WRITE);
		if (ret == 0)
			rpdfs_btree_init_first_block(root, &ref, hnd->data);
		goto out;
	}

	level = root->height;
	ref = root->ref;
	par_ref = (struct rpdfs_block_ref){0,};
	sib_ref = (struct rpdfs_block_ref){0,};

	while (--level >= 0) {
		rpdfs_prd("level %d ref %llu", level, le64_to_cpu(ref.bnr));

		/* initially only try to use write ref on leaf, split/merge will also use */
		rbaf = level == 0 ? RBAF_WRITE : 0;
		ret = rpdfs_txn_use_prepared(rfi, txn, le64_to_cpu(ref.bnr), &hnd, rbaf);
		if (ret < 0)
			goto out;

		val_size = level > 0 ? sizeof(struct rpdfs_block_ref) : leaf_val_size;
		bt = hnd->data;

		if (op == RBT_OP_INSERT && rpdfs_btree_should_split(bt, val_size))
			ret = apply_split(rfi, txn, root, key, &par_ref, &ref, &hnd);
		else if (op == RBT_OP_DELETE && par_ref.bnr &&
				rpdfs_btree_should_merge(bt, val_size))
			ret = apply_merge(rfi, txn, root, &par_ref, &sib_ref, &ref, hnd);
		if (ret < 0)
			goto out;

		bt = hnd->data;

		if (level > 0) {
			par_ref = ref;
			ret = rpdfs_btree_find_child_and_sib_ref(bt, key, &ref, &sib_ref);
			if (ret < 0)
				goto out;
		}
	}

	ret = 0;
out:
	if (ret < 0)
		hnd = NULL;
	*hnd_ret = hnd;
	rpdfs_prd("ret %d", ret);
	return ret;
}

int rpdfs_btree_txn_prepare_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	return btree_txn_prepare_write(rfi, txn, root, key, RBT_OP_INSERT, val_size);
}

int rpdfs_btree_txn_apply_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	struct rpdfs_block_handle *hnd = NULL;

	return btree_txn_apply_write(rfi, txn, root, key, RBT_OP_INSERT, val_size, &hnd) ?:
	       rpdfs_btree_insert_cb(rfi, hnd->data, key, item_cb, cb_arg);
}

int rpdfs_btree_txn_prepare_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	return btree_txn_prepare_write(rfi, txn, root, key, RBT_OP_DELETE, val_size);
}

int rpdfs_btree_txn_apply_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 unsigned val_size, rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	struct rpdfs_block_ref ref;
	int ret;

	/* save ref in case core empties tree and we have to free only block */
	ref = root->ref;

	ret = btree_txn_apply_write(rfi, txn, root, key, RBT_OP_DELETE, val_size, &hnd) ?:
	      rpdfs_btree_delete_cb(rfi, root, hnd->data, key, item_cb, cb_arg);
	if (ret == 0) {
		bt = hnd->data;
		if (bt->nr_items == 0)
			ret = rpdfs_balloc_free_meta(rfi, txn, le64_to_cpu(ref.bnr));
	}

	return ret;
}

int rpdfs_btree_txn_prepare_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	return btree_txn_prepare_write(rfi, txn, root, key, RBT_OP_MODIFY, 0);
}

int rpdfs_btree_txn_apply_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				 struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				 rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	struct rpdfs_block_handle *hnd = NULL;

	return btree_txn_apply_write(rfi, txn, root, key, RBT_OP_MODIFY, 0, &hnd) ?:
	       rpdfs_btree_modify_cb(rfi, hnd->data, key, item_cb, cb_arg);
}

int rpdfs_btree_txn_prepare_lookup(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				   struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				   rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	int ret;

	ret = btree_txn_prepare_read(rfi, txn, root, key, &hnd);
	if (ret == 0) {
		ret = rpdfs_btree_lookup_cb(rfi, hnd->data, key, item_cb, cb_arg);
		rpdfs_txn_prepare_release(rfi, txn, &hnd, 0);
	}

	return ret;
}

/*
 * We copy the items while we're preparing the blocks.  If the
 * transaction retries we'll try to copy again, but that's rare, and
 * this way we avoid a second traversal while applying a read
 * transaction.
 *
 * The caller's key can land after the last item in a leaf block so we
 * won't copy any items even though there are more items in the tree.  If
 * this happens we'll advance the search key past the end of the first
 * leaf and try again, but only once.
 */
int rpdfs_btree_txn_prepare_copy_items(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				       struct rpdfs_btree_root *root, struct rpdfs_btree_key *key,
				       void *buf, size_t size)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_key search;
	struct rpdfs_btree_block *bt;
	int ret;

	search = *key;
retry:
	ret = btree_txn_prepare_read(rfi, txn, root, &search, &hnd);
	if (ret < 0)
		goto out;

	bt = hnd->data;
	ret = rpdfs_btree_copy_items(rfi, bt, &search, buf, size);
	if (ret == 0 && !rpdfs_btree_key_is_max(&bt->last) && !rpdfs_btree_key_cmp(&search, key)) {
		search = bt->last;
		rpdfs_txn_prepare_release(rfi, txn, &hnd, 0);
		rpdfs_btree_key_inc(&search);
		goto retry;
	}

	rpdfs_txn_prepare_release(rfi, txn, &hnd, 0);
out:
	return ret;
}
