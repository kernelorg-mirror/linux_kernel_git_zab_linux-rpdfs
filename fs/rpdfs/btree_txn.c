/* SPDX-License-Identifier: GPL-2.0 */

#include "balloc.h"
#include "block.h"
#include "btree.h"
#include "btree_txn.h"
#include "format-block.h"
#include "pr.h"

/*
 * While the core btree.c functions operate on the structure of the
 * btree blocks themselves, this performs the block coordination above
 * that.  It manages tree traversal, block IO, block lock ordering
 * rules, allocation, write transactions, etc.
 */

static int alloc_block_ref(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			   struct rpdfs_block_ref *ref, struct rpdfs_block_handle **hnd)
{
	int ret;

	ret = rpdfs_txn_acquire_alloc(rfi, txn, hnd);
	if (ret == 0)  {
		ref->bnr = cpu_to_le64((*hnd)->bnr);
		ref->alloc_counter = cpu_to_le64((*hnd)->alloc_ctr);
	}

	return ret;
}

static int split_block(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, u64 key, struct rpdfs_block_ref *par_ref,
		       struct rpdfs_block_ref *ref, struct rpdfs_block_handle **hnd)
{
	struct rpdfs_block_handle *par_hnd = NULL;
	struct rpdfs_block_handle *sib_hnd = NULL;
	struct rpdfs_block_ref sib_ref;
	struct rpdfs_btree_block *parent;
	struct rpdfs_btree_block *sib;
	struct rpdfs_btree_block *bt;
	bool free_parent = false;
	int ret;

	if (par_ref->bnr) {
		/* don't hold child while acquiring parent */
		rpdfs_block_release(rfi, hnd);

		ret = rpdfs_block_acquire(rfi, le64_to_cpu(par_ref->bnr), &par_hnd, RBAF_WRITE) ?:
		      rpdfs_block_acquire(rfi, le64_to_cpu(ref->bnr), hnd, RBAF_WRITE);
		if (ret < 0)
			goto out;
	} else {
		ret = alloc_block_ref(rfi, txn, par_ref, &par_hnd);
		if (ret < 0)
			goto out;

		free_parent = true;
	}

	parent = par_hnd->data;
	bt = (*hnd)->data;

	ret = alloc_block_ref(rfi, txn, &sib_ref, &sib_hnd);
	if (ret < 0)
		goto out;

	sib = sib_hnd->data;

	rpdfs_btree_split(rfi, root, par_ref, parent, &sib_ref, sib, ref, bt);

	rpdfs_txn_block_dirty(rfi, txn, par_hnd);
	rpdfs_txn_block_dirty(rfi, txn, sib_hnd);
	rpdfs_txn_block_dirty(rfi, txn, *hnd);

	if (key <= rpdfs_btree_last_key(sib)) {
		swap(sib_hnd, *hnd);
		*ref = sib_ref;
	}

	ret = 0;
out:
	rpdfs_block_release(rfi, &par_hnd);
	rpdfs_block_release(rfi, &sib_hnd);
	if (ret < 0) {
		rpdfs_block_release(rfi, hnd);
		if (free_parent)
			rpdfs_balloc_free_meta(rfi, txn, le64_to_cpu(par_ref->bnr));
	}

	return ret;
}

/*
 * Acquire a handle to a block described by a reference.   On success,
 * set acquired_ref and return the data pointer to the sibling's btree
 * block structure.  This goofy behavior helps the caller try multiple
 * attempts to get sibling references with optional flags in addition to
 * _WRITE.
 */
static struct rpdfs_btree_block *acquire_sibling(struct rpdfs_fs_info *rfi,
						 struct rpdfs_block_ref *ref,
						 struct rpdfs_block_ref *acquired_ref,
						 struct rpdfs_block_handle **hnd, rbaf_t rbaf)
{
	struct rpdfs_btree_block *bt;
	int ret;

	ret = rpdfs_block_acquire(rfi, le64_to_cpu(ref->bnr), hnd, RBAF_WRITE | rbaf);
	if (ret < 0) {
		if ((rbaf & RBAF_ALREADY_DIRTY) && ret == -ENODATA)
			bt = NULL;
		else
			bt = ERR_PTR(ret);
		*acquired_ref = (struct rpdfs_block_ref) {0,};
	} else {
		bt = (*hnd)->data;
		*acquired_ref = *ref;
	}

	return bt;
}

static int merge_block(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, u64 key, struct rpdfs_block_ref *par_ref,
		       struct rpdfs_block_ref *ref, struct rpdfs_block_handle **hnd)
{
	struct rpdfs_block_handle *par_hnd = NULL;
	struct rpdfs_block_handle *sib_hnd = NULL;
	struct rpdfs_block_ref left_ref;
	struct rpdfs_block_ref right_ref;
	struct rpdfs_block_ref sib_ref;
	struct rpdfs_btree_block *parent;
	struct rpdfs_btree_block *sib;
	struct rpdfs_btree_block *bt;
	int ret;

	/* release target so we can acquire parent and possibly left sibling in order */
	rpdfs_block_release(rfi, hnd);

	ret = rpdfs_block_acquire(rfi, le64_to_cpu(par_ref->bnr), &par_hnd, RBAF_WRITE);
	if (ret < 0)
		goto out;
	parent = par_hnd->data;

	/* see if there are sibling refs on either side of our merge target */
	ret = rpdfs_btree_find_sib_refs(parent, key, &left_ref, &right_ref);
	if (ret < 0)
		goto out;

	/* see if we can avoid errors by finding dirty siblings */
	sib = NULL;
	if (left_ref.bnr)
		sib = acquire_sibling(rfi, &left_ref, &sib_ref, &sib_hnd, RBAF_ALREADY_DIRTY);
	if (!sib && right_ref.bnr)
		sib = acquire_sibling(rfi, &right_ref, &sib_ref, &sib_hnd, RBAF_ALREADY_DIRTY);
	/* no dirty, get left before target */
	if (!sib && left_ref.bnr && !right_ref.bnr)
		sib = acquire_sibling(rfi, &left_ref, &sib_ref, &sib_hnd, 0);

	ret = rpdfs_block_acquire(rfi, le64_to_cpu(ref->bnr), hnd, RBAF_WRITE);
	if (ret < 0)
		goto out;
	bt = (*hnd)->data;

	/* still no sib, get right after target */
	if (!sib && right_ref.bnr)
		sib = acquire_sibling(rfi, &right_ref, &sib_ref, &sib_hnd, 0);
	/* handling both sibling read errors here */
	if (IS_ERR(sib)) {
		ret = PTR_ERR(sib);
		goto out;
	}

	/* if we didn't find a sibling then we have an corrupt parent with a single ref? */
	if (!sib) {
		ret = -EUCLEAN;
		goto out;
	}

	rpdfs_btree_merge(rfi, root, parent, sib, bt);

	/* sib and parent might have been emptied */
	if (sib->nr_items == 0) {
		rpdfs_balloc_free_meta(rfi, txn, sib_hnd->bnr);
		if (parent->nr_items == 0)
			rpdfs_balloc_free_meta(rfi, txn, par_hnd->bnr);
	}

	rpdfs_txn_block_dirty(rfi, txn, par_hnd);
	rpdfs_txn_block_dirty(rfi, txn, sib_hnd);
	rpdfs_txn_block_dirty(rfi, txn, *hnd);

	ret = 0;
out:
	rpdfs_block_release(rfi, &par_hnd);
	rpdfs_block_release(rfi, &sib_hnd);
	if (ret < 0) {
		rpdfs_block_release(rfi, hnd);
		hnd = NULL;
	}

	return ret;
}

/*
 * The signed @val_size is an indication of what the caller will do with
 * the leaf.  When negative it's going to delete and we should check for
 * merging.  When positive we check for splitting before inserting.
 */
static int acquire_leaf(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			struct rpdfs_btree_root *root, u64 key, int val_size, u64 *last,
			struct rpdfs_block_handle **hnd_ret, rbaf_t rbaf)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	struct rpdfs_block_ref par_ref;
	struct rpdfs_block_ref ref;
	u64 ref_key;
	int level;
	int ret;

	if (WARN_ON_ONCE(val_size != 0 && !(rbaf & RBAF_WRITE))) {
		ret = -EINVAL;
		goto out;
	}

	rpdfs_prd("root %llu h %u", le64_to_cpu(root->ref.bnr), root->height);

	if (last)
		*last = RPDFS_BTREE_ITEM_KEY_MASK;

	if (root->ref.bnr == 0) {
		if (val_size <= 0) {
			ret = -ENOENT;
			goto out;
		}

		ret = alloc_block_ref(rfi, txn, &ref, &hnd);
		if (ret == 0) {
			rpdfs_btree_init_first_block(root, &ref, hnd->data);
			rpdfs_txn_block_dirty(rfi, txn, hnd);
		}
		goto out;
	}

	par_ref = (struct rpdfs_block_ref) {0,};
	ref = root->ref;

	for (level = root->height - 1; level >= 0; level--) {
		rpdfs_prd("level %d ref %llu", level, le64_to_cpu(ref.bnr));

		/* XXX should be more robust block verification on first read */
		if (ref.bnr == 0) {
			ret = -EUCLEAN;
			goto out;
		}

		ret = rpdfs_block_acquire(rfi, le64_to_cpu(ref.bnr), &hnd, level > 0 ? 0 : rbaf);
		if (ret < 0)
			goto out;
		bt = hnd->data;

		if (val_size != 0) {
			ret = 0;
			if (val_size > 0 && rpdfs_btree_must_split(bt, val_size))
				ret = split_block(rfi, txn, root, key, &par_ref, &ref, &hnd);
			else if (val_size < 0 && par_ref.bnr && rpdfs_btree_should_merge(bt))
				ret = merge_block(rfi, txn, root, key, &par_ref, &ref, &hnd);
			if (ret < 0)
				goto out;
		}

		if (level > 0) {
			bt = hnd->data;
			par_ref = ref;
			ret = rpdfs_btree_find_child_ref(bt, key, &ref_key, &ref);
			if (ret < 0)
				goto out;

			if (last)
				*last = ref_key;
			rpdfs_block_release(rfi, &hnd);
		}
	}

	ret = 0;
out:
	if (ret < 0)
		rpdfs_block_release(rfi, &hnd);
	*hnd_ret = hnd;
	return ret;
}

int rpdfs_btree_lookup(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       u64 key, rpdfs_btree_item_cb_t cb, void *arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	int ret;

	ret = acquire_leaf(rfi, NULL, root, key, 0, NULL, &hnd, 0);
	if (ret == 0) {
		bt = hnd->data;
		ret = rpdfs_btree_collisions_cb(rfi, bt, key, cb, arg);
		rpdfs_block_release(rfi, &hnd);
	}
	return ret;
}

int rpdfs_btree_insert(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb, void *arg,
		       struct kvec *kv, unsigned nr_segs)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	u16 val_size = iov_length((struct iovec *)kv, nr_segs);
	int ret;

	ret = acquire_leaf(rfi, txn, root, key, val_size, NULL, &hnd, RBAF_WRITE);
	if (ret == 0) {
		bt = hnd->data;
		ret = rpdfs_btree_insert_cb(rfi, bt, key, cb, arg, kv, nr_segs, val_size);
		if (ret >= 0)
			rpdfs_txn_block_dirty(rfi, txn, hnd);
		rpdfs_block_release(rfi, &hnd);
	}
	return ret;
}

int rpdfs_btree_modify(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb,
		       void *arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	int ret;

	ret = acquire_leaf(rfi, txn, root, key, 0, NULL, &hnd, RBAF_WRITE);
	if (ret == 0) {
		bt = hnd->data;
		ret = rpdfs_btree_collisions_cb(rfi, bt, key, cb, arg);
		if (ret >= 0)
			rpdfs_txn_block_dirty(rfi, txn, hnd);
		rpdfs_block_release(rfi, &hnd);
	}
	return ret;
}

int rpdfs_btree_delete(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
		       struct rpdfs_btree_root *root, u64 key, rpdfs_btree_item_cb_t cb,
		       void *arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	int ret;

	ret = acquire_leaf(rfi, txn, root, key, -1, NULL, &hnd, RBAF_WRITE);
	if (ret == 0) {
		bt = hnd->data;
		ret = rpdfs_btree_delete_cb(rfi, root, bt, key, cb, arg);
		if (ret >= 0) {
			rpdfs_txn_block_dirty(rfi, txn, hnd);
			if (bt->nr_items == 0)
				rpdfs_balloc_free_meta(rfi, txn, hnd->bnr);
		}
		rpdfs_block_release(rfi, &hnd);
	}
	return ret;
}

/*
 * Call the callback on all the items present in the tree after the
 * given full precision key.  This will continue through leaf blocks as
 * long as the callback returns -ELOOP.  If we run out of keys we'll
 * return into 0.
 */
int rpdfs_btree_read_items(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			   u64 key, rpdfs_btree_item_cb_t cb, void *arg)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_btree_block *bt;
	u64 last;
	int ret;

	for (;;) {
		ret = acquire_leaf(rfi, NULL, root, key, 0, &last, &hnd, 0);
		if (ret < 0)
			break;

		bt = hnd->data;
		ret = rpdfs_btree_items_cb(rfi, bt, key, cb, arg);
		rpdfs_block_release(rfi, &hnd);
		if (ret != -ELOOP)
			break;

		if (last >= RPDFS_BTREE_ITEM_KEY_MASK) {
			ret = 0;
			break;
		}

		key = last + 1;
	}

	return ret;
}
