/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/limits.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/sort.h>
#include <linux/compiler_attributes.h>

#include <asm/byteorder.h>

#include "btree.h"
#include "compare.h"
#include "format-block.h"
#include "string.h"

/*
 * These block btrees are used to sort items with variable size value
 * payloads and unpredictable key distribution.  In the file system,
 * that means dirents, xattrs, and global indices.
 *
 * We're leaning hard into minimalism for as long as we can get away
 * with it.  Items keys and values are stored at increasing offsets in
 * the block as they're inserted.  An array of small item headers at the
 * start of the block stores the offset of the item and are kept in key
 * sorted order.  We're spending the cost of cpu cycles on memmove to
 * maintain sorting while getting the benefit of simpler structures.
 *
 * We don't track free internal space in the blocks.  An allocation
 * offset advances towards the tail as we allocate.  We can compact
 * items in a block to free internal space before splitting a block to
 * satisfy insertion.
 */

/*
 * If a block's total_free reaches this value then we try to move items
 * from a sibling to fill it above the threshold.  If the sibling is
 * also at the threshold then the two blocks are merged.
 *
 * We want to leave some slack between the max size of a merged block
 * (80% full) and a full block so that the repeated insertion and
 * deletion of a few items doesn't bounce a pair of blocks between
 * splitting and merging.
 */
#define RPDFS_BTREE_MERGE_FREE_THRESH	(RPDFS_BTREE_MAX_FREE * (100 - 40) / 100)

/*
 * If an insertion could be performed after compacting free space, but
 * total free space is less than this threshold, then we'll split the
 * block instead.  This avoids excessive compaction if insert/delete
 * cycles constantly delete to create fragmented space and then try to
 * insert into it.  The higher we set this value the more items need to
 * be involved in the cycle before each compaction, so the lower its
 * amortized cost.
 */
#define RPDFS_BTREE_SPLIT_FREE_THRESH	(RPDFS_BTREE_MAX_FREE * 10 / 100)

static struct rpdfs_btree_key min_key = { 0, 0 };
static struct rpdfs_btree_key max_key = { cpu_to_le64(U64_MAX), cpu_to_le64(U64_MAX) };

/* Initialize an empty btree root */
void rpdfs_btree_root_init(struct rpdfs_btree_root *root)
{
	root->height = 0;
	root->ref.bnr = 0;
	root->ref.alloc_counter = 0;
}

/*
 * Callers incrementally modify the block before it's written.  We don't
 * want to write old kernel memory as block contents so we explicitly
 * zero the block before initializing.
 */
static void init_block(struct rpdfs_btree_block *bt, u8 level,
		       struct rpdfs_btree_key *first, struct rpdfs_btree_key *last)
{
	memzero_explicit(bt, RPDFS_BLOCK_SIZE);

	bt->first = *first;
	bt->last = *last;
	bt->nr_items = 0;
	bt->tail_free = cpu_to_le16(RPDFS_BTREE_MAX_FREE);
	bt->total_free = bt->tail_free;
	bt->level = level;
}

/*
 * Usually newly allocated blocks are linked into the tree as we're
 * splitting.  We handle initializing the first block in the tree with a
 * specific call during descent rather than detecting it in the core
 * leaf manipulation functions.
 */
void rpdfs_btree_init_first_block(struct rpdfs_btree_root *root, struct rpdfs_block_ref *ref,
				  struct rpdfs_btree_block *bt)
{
	init_block(bt, 0, &min_key, &max_key);
	root->ref = *ref;
	root->height = 1;
}

static void bug_on_bad_item_off(size_t off)
{
	BUG_ON(off < offsetof(struct rpdfs_btree_block, ihdrs[RPDFS_BTREE_MAX_ITEMS]));
	BUG_ON(off > (RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_btree_item)));
	BUG_ON(!IS_ALIGNED(off, RPDFS_BTREE_ITEM_ALIGN));
}

static struct rpdfs_btree_item *item_from_off(const struct rpdfs_btree_block *bt, u16 off)
{
	if (off == 0)
		return NULL;

	bug_on_bad_item_off(off);

	return (void *)bt + off;
}

static inline struct rpdfs_btree_item *item_from_ind(struct rpdfs_btree_block *bt, u16 ind)
{
	BUG_ON(ind >= le16_to_cpu(bt->nr_items));

	return item_from_off(bt, le16_to_cpu(bt->ihdrs[ind].off));
}

static u16 item_val_size(struct rpdfs_btree_block *bt, u16 ind)
{
	return le16_to_cpu(bt->ihdrs[ind].val_size);
}

static u16 aligned_item_size(u16 val_size)
{
	return ALIGN(sizeof(struct rpdfs_btree_item) + val_size, RPDFS_BTREE_ITEM_ALIGN);
}

static struct rpdfs_btree_item *first_item(struct rpdfs_btree_block *bt)
{
	return item_from_ind(bt, 0);
}

static struct rpdfs_btree_item *last_item(struct rpdfs_btree_block *bt)
{
	/* bad ind from nr_items == 0 caught by item_from_ind assertion */
	return item_from_ind(bt, le16_to_cpu(bt->nr_items) - 1);
}

/*
 * Initialize a bti to point to an item in the block.  As a slight
 * convenience/hack, if the caller describes an item that doesn't exist
 * then we set the key to all ones which will stop it from being used.
 */
static void init_bti(struct rpdfs_btree_item_args *bti, struct rpdfs_btree_block *bt, u16 ind)
{
	struct rpdfs_btree_item *item;

	if (ind >= le16_to_cpu(bt->nr_items)) {
		rpdfs_btree_key_set_max(&bti->key);
		bti->val = NULL;
		bti->val_size = 0;
	} else {
		item = item_from_ind(bt, ind);
		bti->key = item->key;
		bti->val = &item->val[0];
		bti->val_size = item_val_size(bt, ind);
	}
}

/*
 * Find the first index in the items array that the search key is less than.  Can return
 * the index past the current size of the array for insertion.  The caller is responsible
 * for using the returned index appropriately.
 */
static u16 find_key_ind(struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key)
{
	struct rpdfs_btree_item *item;
	int start = 0;
	int end = (int)le16_to_cpu(bt->nr_items) - 1;
	int ind = 0;
	int cmp;

	while (start <= end) {
		ind = (start + end) >> 1;
		item = item_from_ind(bt, ind);

		cmp = rpdfs_btree_key_cmp(key, &item->key);
		if (cmp == 0)
			return ind;
		else if (cmp < 0)
			end = ind - 1;
		else
			start = ++ind;
	}

	return ind;
}

static int cmp_ihdr_off(const void *A, const void *B, const void *priv)
{
	const struct rpdfs_btree_block *bt = priv;
	const struct rpdfs_btree_item_header *a = &bt->ihdrs[*(u16 *)A];
	const struct rpdfs_btree_item_header *b = &bt->ihdrs[*(u16 *)B];

	return (int)le16_to_cpu(a->off) - (int)le16_to_cpu(b->off);
}

static int cmp_ihdr_key(const void *A, const void *B, const void *priv)
{
	const struct rpdfs_btree_block *bt = priv;
	const struct rpdfs_btree_item_header *a = A;
	const struct rpdfs_btree_item_header *b = B;
	const struct rpdfs_btree_item *item_a = item_from_off(bt, le16_to_cpu(a->off));
	const struct rpdfs_btree_item *item_b = item_from_off(bt, le16_to_cpu(b->off));

	return rpdfs_btree_key_cmp(&item_a->key, &item_b->key);
}

/*
 * True if there's room in the block for an insertion but not at the
 * tail of the block.  Compaction shuffles the location of items in the
 * block so operations on a block are generally making this decision
 * before they get references to items in the block.
 */
static bool should_compact(struct rpdfs_btree_block *bt, u16 val_size)
{
	u16 val_bytes = aligned_item_size(val_size);

	return (val_bytes > le16_to_cpu(bt->tail_free)) &&
	       (val_bytes <= le16_to_cpu(bt->total_free));
}

/*
 * True if the caller should split the block before trying to insert an
 * item with the given val size.
 *
 * We split if the item doesn't fit in free space at all.
 *
 * But we'll also split if the item doesn't fit in tail free space and
 * would fit in fragmented free space, but free space is so low that
 * we're likely to split anyway soon after compaction.
 */
bool rpdfs_btree_should_split(struct rpdfs_btree_block *bt, u16 val_size)
{
	u16 size = aligned_item_size(val_size);
	u16 total_free = le16_to_cpu(bt->total_free);

	return (size > total_free) ||
	       (size > le16_to_cpu(bt->tail_free) && total_free < RPDFS_BTREE_SPLIT_FREE_THRESH);
}

/*
 * True if the caller should merge or rebalance this block after
 * removing an item with the given value size. If the free space gets
 * large enough that the item population goes below the merge free space
 * threshold, then we want to pull items from or merge with a sibling
 * block to restore balance.
 */
bool rpdfs_btree_should_merge(struct rpdfs_btree_block *bt, u16 val_size)
{
	return (le16_to_cpu(bt->total_free) + aligned_item_size(val_size)) >=
		RPDFS_BTREE_MERGE_FREE_THRESH;
}

/*
 * Move the region of item headers from the index to the end of the
 * array in the given direction.  The index may fall outside the array
 * (when inserting into an empty block or deleting the last sorted item
 * in the block).
 */
static inline void memmove_item_headers(struct rpdfs_btree_block *bt, u16 ind, int dist)
{
	u16 nr = le16_to_cpu(bt->nr_items);

	if (ind < nr)
		memmove(&bt->ihdrs[ind + dist], &bt->ihdrs[ind], (nr - ind) * sizeof(bt->ihdrs[0]));
}

/*
 * Consume free space at the end of the block to create a new item,
 * initialize it with the caller's arguments, and link it into the tree
 * at the parent's link.
 *
 * Because this references an existing item we will not compact items
 * here. The caller must ensure that there is sufficient free space for
 * the item.
 */
static struct rpdfs_btree_item *insert_item(struct rpdfs_btree_block *bt, u16 ind,
					    struct rpdfs_btree_key *key, void *val, u16 val_size)
{
	u16 off = RPDFS_BLOCK_SIZE - le16_to_cpu(bt->tail_free);
	struct rpdfs_btree_item *item = item_from_off(bt, off);
	u16 bytes = aligned_item_size(val_size);

	BUG_ON(ind >= RPDFS_BTREE_MAX_ITEMS);
	BUG_ON(le16_to_cpu(bt->tail_free) - bytes > RPDFS_BTREE_MAX_FREE);
	BUG_ON(le16_to_cpu(bt->total_free) - bytes > RPDFS_BTREE_MAX_FREE);

	memmove_item_headers(bt, ind, 1);
	le16_add_cpu(&bt->tail_free, -bytes);
	le16_add_cpu(&bt->total_free, -bytes);
	le16_add_cpu(&bt->nr_items, 1);
	bt->ihdrs[ind].off = cpu_to_le16(off);
	bt->ihdrs[ind].val_size = cpu_to_le16(val_size);

	item->key = *key;
	if (val_size)
		memcpy_and_zero_tail(&item->val[0], ALIGN(val_size, RPDFS_BTREE_ITEM_ALIGN),
				     val, val_size);

	return item;
}

/*
 * Delete an item by removing its item header and zeroing its bytes.
 * This almost certainly leaves behind fragmented free space in the
 * block that will later be reclaimed by compaction.
 *
 * XXX There is the opportunity to remove internal fragmentation when
 * all the items have the same size.  If we could find the ind of the
 * last item before the tail free space then we could move it into the
 * space freed by the deletion, maintaining unfragmented items and free
 * space.  I'm not sure it's worth either searching for the key at that
 * offset or maintaining the metadata to always know the sort position
 * of the item at the last offset.
 */
static void delete_item(struct rpdfs_btree_block *bt, u16 ind)
{
	struct rpdfs_btree_item *item = item_from_ind(bt, ind);
	u16 bytes = aligned_item_size(item_val_size(bt, ind));
	u16 off = le16_to_cpu(bt->ihdrs[ind].off);
	u16 nr;

	BUG_ON(le16_to_cpu(bt->tail_free) + bytes > RPDFS_BTREE_MAX_FREE);
	BUG_ON(le16_to_cpu(bt->total_free) + bytes > RPDFS_BTREE_MAX_FREE);

	if (off == RPDFS_BLOCK_SIZE - le16_to_cpu(bt->tail_free) - bytes)
		le16_add_cpu(&bt->tail_free, bytes);
	le16_add_cpu(&bt->total_free, bytes);

	memmove_item_headers(bt, ind + 1, -1);
	le16_add_cpu(&bt->nr_items, -1);

	nr = le16_to_cpu(bt->nr_items);
	memset(&bt->ihdrs[nr], 0, sizeof(struct rpdfs_btree_item_header));
	memset(item, 0, bytes);
}

/*
 * Defragment internal free space by moving all the items towards the
 * front of the block, gathering all free space to the end.  We sort the
 * item headers by offset so we can move the items by iterating in
 * offset order.  Then we return the item headers to being sorted by
 * key.
 *
 * We could use per-cpu resources to have an external offset sort index,
 * but that could get obnoxious if the blocks got significantly larger
 * so we haven't bothered.  Compaction is rare so hopefully the doubled
 * sort cost isn't a problem.
 */
static void compact_items(struct rpdfs_btree_block *bt)
{
	struct rpdfs_btree_item *item;
	struct rpdfs_btree_item *dst;
	u16 bytes;
	u16 ind;
	u16 off;
	u16 nr;

	if (bt->nr_items == 0 || bt->tail_free == bt->total_free)
		return;

	nr = le16_to_cpu(bt->nr_items);
	sort_r(bt->ihdrs, nr, sizeof(bt->ihdrs[0]), cmp_ihdr_off, NULL, bt);

	off = RPDFS_BLOCK_SIZE - RPDFS_BTREE_MAX_FREE;
	for (ind = 0; ind < nr; ind++) {
		item = item_from_ind(bt, ind);
		bytes = aligned_item_size(item_val_size(bt, ind));

		if (le16_to_cpu(bt->ihdrs[ind].off) != off) {
			dst = item_from_off(bt, off);
			bt->ihdrs[ind].off = cpu_to_le16(off);
			memmove(dst, item, bytes);
		}

		off += bytes;
	}

	/* zero newly free region before the existing free region at the tail */
	bytes = le16_to_cpu(bt->total_free) - le16_to_cpu(bt->tail_free);
	memset(item_from_off(bt, off), 0, bytes);

	bt->tail_free = bt->total_free;
	sort_r(bt->ihdrs, nr, sizeof(bt->ihdrs[0]), cmp_ihdr_key, NULL, bt);
}

/*
 * The tree has a surprising invariant that I may live to regret: items
 * whose keys differ only by the least significant bit will be found in
 * the same leaf.  The separator key between nodes must have the LSB
 * set.
 *
 * We encode dirents with only one collision bit so that we can always
 * make dirent operation decisions by looking at a pair of items.  This
 * invariant ensures that we can find these items in one leaf.
 *
 * This test is being called while moving items as it decides to stop.
 * The invariant is violated, and moving must continue, if the items
 * straddling the two blocks have keys that only differ by the least
 * significant bit.
 */
static bool violating_lsb_pair_invariant(struct rpdfs_btree_block *dst,
					 struct rpdfs_btree_block *src, bool to_right)
{
	struct rpdfs_btree_item *s;
	struct rpdfs_btree_item *d;

	if (to_right) {
		s = last_item(src);
		d = first_item(dst);
	} else {
		s = first_item(src);
		d = last_item(dst);
	}

	return (s->key.msq == d->key.msq) &&
	       ((s->key.lsq ^ d->key.lsq) == cpu_to_le64(1));
}

/*
 * Move items from the source block to the destination block.
 *
 * We need to compact the destination before we move so that there's
 * room for the moving items.  This is used by splitting and merging
 * which also has to ensure that both of its output blocks are
 * sufficiently compacted to receive an insertion.  We always compact
 * the source after moving items.
 *
 * @to_right moves items in descending order from the end of the src
 * block to the front of the dst block.  When false it moves in the
 * opposite direction: in ascending order from the start of the src
 * block to the end of the dst block.
 *
 * @until_balanced stops moving once the consumed space in the two
 * blocks are roughly equal, rather than trying to move all items from
 * src to dst.  It checks after moving each item, and potentially must
 * move an additional item to maintain our separator key invariant, so
 * the balance can be off by two max items at most.
 */
static void move_items(struct rpdfs_btree_block *dst, struct rpdfs_btree_block *src,
		       bool to_right, bool until_balanced)
{
	struct rpdfs_btree_item *item;
	u16 src_ind;
	u16 dst_ind;

	if (src->nr_items == 0)
		return;

	compact_items(dst);

	if (to_right) {
		src_ind = le16_to_cpu(src->nr_items) - 1;
		dst_ind = 0;
	} else {
		src_ind = 0;
		dst_ind = le16_to_cpu(dst->nr_items);
	}

	while (src->nr_items != 0) {
		item = item_from_ind(src, src_ind);
		insert_item(dst, dst_ind, &item->key, item->val, item_val_size(src, src_ind));
		delete_item(src, src_ind);

		if (until_balanced &&
		    (le16_to_cpu(dst->total_free) <= le16_to_cpu(src->total_free)) &&
		    !violating_lsb_pair_invariant(dst, src, to_right))
			break;

		if (to_right)
			src_ind--;
		else
			dst_ind++;
	}
}

/*
 * The ordered blocks have had items moved between them.  Reset their
 * inner last,first key range boundary to reflect the key of the last
 * item in the left block.
 *
 * This also has to maintain our separator invariant.  Item motion has
 * ensured that it's safe for us to always set the lsb of the separator
 * key. (see violating_lsb_pair_invariant()).
 */
static void reset_key_range_boundary(struct rpdfs_btree_block *left,
				     struct rpdfs_btree_block *right)
{
	BUG_ON(rpdfs_btree_key_cmp(&left->first, &right->last) >= 0);

	left->last = last_item(left)->key;
	left->last.lsq |= cpu_to_le64(1);
	right->first = left->last;
	rpdfs_btree_key_inc(&right->first);
}

static int copy_item_ref(struct rpdfs_btree_block *bt, u16 ind, struct rpdfs_block_ref *ref)
{
	const u16 sz = sizeof(struct rpdfs_block_ref);
	struct rpdfs_btree_item *item;

	if (ind >= le16_to_cpu(bt->nr_items))
		return -EUCLEAN;

	if (item_val_size(bt, ind) != sz)
		return -EUCLEAN;

	item = item_from_ind(bt, ind);
	memcpy(ref, item->val, sz);
	return 0;
}

/*
 * Given a parent btree block, set the caller's reference to the child
 * block that will contain the search key.
 */
int rpdfs_btree_find_child_ref(struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
			       struct rpdfs_block_ref *ref)
{
	u16 ind = find_key_ind(bt, key);

	return copy_item_ref(bt, ind, ref);
}

int rpdfs_btree_find_child_and_sib_ref(struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
				       struct rpdfs_block_ref *ref,
				       struct rpdfs_block_ref *sib_ref)
{
	u16 ind = find_key_ind(bt, key);
	int ret;

	ret = copy_item_ref(bt, ind, ref);
	if (ret == 0) {
		ind = ind > 0 ? ind - 1 : ind + 1;
		ret = copy_item_ref(bt, ind, sib_ref);
	}

	return ret;
}

/*
 * Split a block, moving items to a newly allocated block.  We move
 * items to balance the space they take up, not the number of items.
 * The new block is always empty so we can always move items.  We move
 * items to a new empty block to the left so that we only have to insert
 * a new parent item and don't have to modify the existing parent item's
 * key.
 */
int rpdfs_btree_split(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		      struct rpdfs_block_ref *par_ref, struct rpdfs_btree_block *parent,
		      struct rpdfs_block_ref *sib_ref, struct rpdfs_btree_block *sib,
		      struct rpdfs_block_ref *ref, struct rpdfs_btree_block *bt)
{
	/* link in allocated parent if we're splitting first block */
	if (root->ref.bnr == ref->bnr) {
		init_block(parent, bt->level + 1, &bt->first, &bt->last);
		root->ref = *par_ref;
		insert_item(parent, 0, &bt->last, ref, sizeof(struct rpdfs_block_ref));
	}

	init_block(sib, bt->level, &bt->first, &bt->last);
	move_items(sib, bt, false, true);
	reset_key_range_boundary(sib, bt);

	if (should_compact(parent, sizeof(struct rpdfs_block_ref)))
		compact_items(parent);
	insert_item(parent, find_key_ind(parent, &sib->last), &sib->last, sib_ref,
		    sizeof(struct rpdfs_block_ref));

	return 0;
}

/*
 * Merge items from a sibling block into our block.  This is only called
 * if there is a parent block so there must be at least one sibling.
 *
 * Our block can be on either spine of the tree so we need to be able to
 * pull from a neighbor on either side.  We have to update the key in
 * the parent reference item that separates the items in the two child
 * blocks, regardless.
 *
 * This can remove the sibling or parent from the tree.  If the sibling
 * loses its items it's removed and then if the parent only has one
 * remaining item it's removed.  The item counts in the block buffers
 * remain at those values for the caller to discover that this has
 * happened.
 */
int rpdfs_btree_merge(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		      struct rpdfs_btree_block *par_bt, struct rpdfs_btree_block *sib_bt,
		      struct rpdfs_btree_block *bt)
{
	struct rpdfs_btree_item *sib_ref_item;
	struct rpdfs_btree_item *ref_item;
	bool until_balanced;
	bool to_right;
	u16 sib_ind;
	u16 bt_ind;

	/* find our and sibling ref items */
	bt_ind = find_key_ind(par_bt, &bt->first);
	to_right = bt_ind > 0;
	sib_ind = to_right ? bt_ind - 1 : bt_ind + 1;

	ref_item = item_from_ind(par_bt, bt_ind);
	sib_ref_item = item_from_ind(par_bt, sib_ind);

	/*
	 * Balance items between blocks if the result is two blocks
	 * whose average of free space is less than the merge free
	 * threshold, otherwise merge them.
	 */
	until_balanced = (le16_to_cpu(bt->total_free) + le16_to_cpu(sib_bt->total_free)) <
			 (RPDFS_BTREE_MERGE_FREE_THRESH * 2);

	/* expand our range for insertion assertions, will keep if we empty sib */
	if (to_right)
		bt->first = sib_bt->first;
	else
		bt->last = sib_bt->last;

	move_items(bt, sib_bt, to_right, until_balanced);

	/* if sib has items then update mid separator and maybe update sib ref */
	if (sib_bt->nr_items != 0) {
		if (to_right)
			reset_key_range_boundary(sib_bt, bt);
		else
			reset_key_range_boundary(bt, sib_bt);
		if (to_right)
			sib_ref_item->key = sib_bt->last;
	}

	/* update our key if our last changed */
	if (!to_right)
		ref_item->key = bt->last;

	/* delete ref to empty sibling, maybe also drop parent with single ref to us */
	if (sib_bt->nr_items == 0) {
		delete_item(par_bt, sib_ind);
		if (le16_to_cpu(par_bt->nr_items) == 1) {
			copy_item_ref(par_bt, 0, &root->ref);
			root->height = bt->level + 1;
		}
	}

	return 0;
}

void rpdfs_btree_key_set_min(struct rpdfs_btree_key *key)
{
	*key = min_key;
}

bool rpdfs_btree_key_is_min(struct rpdfs_btree_key *key)
{
	return rpdfs_btree_key_cmp(key, &min_key) == 0;
}

void rpdfs_btree_key_set_max(struct rpdfs_btree_key *key)
{
	*key = max_key;
}

bool rpdfs_btree_key_is_max(struct rpdfs_btree_key *key)
{
	return rpdfs_btree_key_cmp(key, &max_key) == 0;
}

void rpdfs_btree_key_inc(struct rpdfs_btree_key *key)
{
	le64_add_cpu(&key->lsq, 1);
	if (key->lsq == 0)
		le64_add_cpu(&key->msq, 1);
}

int rpdfs_btree_key_cmp(const struct rpdfs_btree_key *a, const struct rpdfs_btree_key *b)
{
	return rpdfs_compare(le64_to_cpu(a->msq), le64_to_cpu(b->msq)) ?:
	       rpdfs_compare(le64_to_cpu(a->lsq), le64_to_cpu(b->lsq));
}

/*
 * Lookup a key in the block.  Call the caller's callback with as many
 * items as exist in the block from the key.
 */
int rpdfs_btree_lookup_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg)
{
	struct rpdfs_btree_item_args a;
	struct rpdfs_btree_item_args b;
	struct rpdfs_btree_item_args c;
	u16 ind;

	ind = find_key_ind(bt, key);
	init_bti(&a, bt, ind);
	init_bti(&b, bt, ind + 1);
	init_bti(&c, bt, ind + 2);

	return item_cb(rfi, &a, &b, &c, cb_arg);
}

/*
 * Insert an item as described through a callback.
 *
 * In the callback, a and b may be existing items in the tree.  If 0 is
 * returned then c must describe the item to be inserted.  c's insertion
 * key must be >= the search key, != a's key, and < b's key.
 */
int rpdfs_btree_insert_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg)
{
	struct rpdfs_btree_item_args ins;
	struct rpdfs_btree_item_args a;
	struct rpdfs_btree_item_args b;
	u16 ind;
	int cmp;
	int ret;

	ind = find_key_ind(bt, key);
	init_bti(&a, bt, ind);
	init_bti(&b, bt, ind + 1);

	ret = item_cb(rfi, &a, &b, &ins, cb_arg);
	if (ret == 0) {
		cmp = rpdfs_btree_key_cmp(&ins.key, &a.key);
		if (WARN_ON_ONCE(rpdfs_btree_key_cmp(&ins.key, key) < 0 || cmp == 0 ||
				 rpdfs_btree_key_cmp(&ins.key, &b.key) > 0)) {
			ret = -EINVAL;
		} else {
			if (cmp > 0)
				ind++;
			if (should_compact(bt, ins.val_size))
				compact_items(bt);
			insert_item(bt, ind, &ins.key, ins.val, ins.val_size);
		}
	}

	return ret;
}

/*
 * Delete an item as described by a callback.  The callback is given the
 * next three items in a leaf whose keys are greater than or equal to
 * the search key.  The callback returns errors or a positive index of
 * the item to delete, with 0 indicating a.
 *
 * The caller must deal with the callback arguments being limited to a
 * leaf block. They're not necessarily the next three keys in the btree.
 * If there are fewer than 3 items in the block after the search key
 * then the later bti arguments will have their key set to max and the
 * cb must not try to delete them.
 */
int rpdfs_btree_delete_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			  struct rpdfs_btree_block *bt, struct rpdfs_btree_key *key,
			  rpdfs_btree_item_cb_t item_cb, void *cb_arg)
{
	struct rpdfs_btree_item_args a;
	struct rpdfs_btree_item_args b;
	struct rpdfs_btree_item_args c;
	u16 ind;
	int ret;

	ind = find_key_ind(bt, key);
	init_bti(&a, bt, ind);
	init_bti(&b, bt, ind + 1);
	init_bti(&c, bt, ind + 2);

	ret = item_cb(rfi, &a, &b, &c, cb_arg);
	if (WARN_ON_ONCE(ret >= 0 && (ret >= 3 || (ind + ret) >= le16_to_cpu(bt->nr_items))))
		ret = -EINVAL;
	if (ret >= 0) {
		delete_item(bt, ind + ret);
		if (bt->nr_items == 0) {
			memset(&root->ref, 0, sizeof(struct rpdfs_block_ref));
			root->height = 0;
		}
	}

	return ret;
}

/*
 * The caller's callback is given a pointer to the next item from the
 * search key.  The callback can modify only the contents of the value
 * of that item, or return an error.
 */
int rpdfs_btree_modify_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			  struct rpdfs_btree_key *key, rpdfs_btree_item_cb_t item_cb,
			  void *cb_arg)
{
	struct rpdfs_btree_item_args a;
	u16 ind;
	int ret;

	ind = find_key_ind(bt, key);
	if (ind >= le16_to_cpu(bt->nr_items)) {
		ret = -ENOENT;
	} else {
		init_bti(&a, bt, ind);
		ret = item_cb(rfi, &a, NULL, NULL, cb_arg);
	}

	return ret;
}

static size_t copied_bti_size(u16 val_size)
{
	/* surely alignment must be a power of two.. */
	BUILD_BUG_ON(!is_power_of_2(__alignof__(struct rpdfs_btree_item_args)));

	return ALIGN(sizeof(struct rpdfs_btree_item_args) + val_size,
		     __alignof__(struct rpdfs_btree_item_args));
}

/*
 * Copy items from the block into contiguous rpdfs_btree_item_args
 * structs in the caller's buffer.  They can then iterate over the
 * copied items.
 *
 * The buffer must be aligned so the bti struct.  The number of copied
 * items is returned.
 *
 * The buffer must fit a maximum size item.  This avoids the error case
 * where the caller couldn't get their first item because it didn't fit.
 * Each call either gets items or has run out of items.
 */
int rpdfs_btree_copy_items(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			   struct rpdfs_btree_key *key, void *buf, size_t size)
{
	struct rpdfs_btree_item_args *bti;
	struct rpdfs_btree_item *item;
	size_t bti_size;
	u16 val_size;
	u16 ind;
	int ret;

	if (WARN_ON_ONCE(!IS_ALIGNED((unsigned long)buf,
				     __alignof__(struct rpdfs_btree_item_args))) ||
	    WARN_ON_ONCE(size < copied_bti_size(RPDFS_BTREE_MAX_VAL_SIZE)))
		return -EINVAL;

	ret = 0;
	for (ind = find_key_ind(bt, key); ind < le16_to_cpu(bt->nr_items); ind++) {

		val_size = item_val_size(bt,  ind);
		bti_size = copied_bti_size(val_size);
		if (size < bti_size)
			break;

		bti = buf;
		item = item_from_ind(bt, ind);

		bti->key = item->key;
		bti->val = (bti + 1);
		bti->val_size = val_size;
		if (val_size)
			memcpy(bti->val, item->val, val_size);

		buf += bti_size;
		size -= bti_size;
		ret++;
	}

	return ret;
}

struct rpdfs_btree_item_args *rpdfs_btree_next_copied_item(struct rpdfs_btree_item_args *bti)
{
	return (void *)bti + copied_bti_size(bti->val_size);
}
