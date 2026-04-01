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
#include "pr.h"
#include "string.h"

/*
 * These block btrees are used to index relatively small variable size
 * items with keys that are hashes of larger unique identifiers.
 * Classically, dirents and xattrs by their name.
 *
 * The interface specifically supports hash collisions.  Low bits of the
 * stored keys are used to track a limited number of collisions without
 * returning errors.  In the calling API keys lead to leaf blocks and
 * callbacks are used to let the callers perform their search of deeper
 * item identity amongst collisions.
 *
 * We're leaning hard into minimalism for as long as we can get away
 * with it.  Variable length values are allocated from free space at the
 * end of the block.  An array of words at the start of the block stores
 * the minimal fixed size item fields and are kept in key sorted order.
 *
 * We don't index free internal space in the blocks.  A value allocation
 * offset moves from the end of the block towards the front.  We can
 * compact values to reclaim fragmented free space created by deletions.
 */

/*
 * Once a block's total_free is at or above this value we try to merge
 * items from siblings.  If a block and its sibling are both at the
 * value then they can be merged into one block.
 *
 * We round up half the max possible free space to the value alignment.
 * As free space increases used space decreases, so free greater than
 * half must have used less than half so they can combine.
 */
#define RPDFS_BTREE_MERGE_FREE_THRESH \
	round_up((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_btree_block)) / 2, RPDFS_BTREE_VAL_ALIGN)

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
static void init_block(struct rpdfs_btree_block *bt, u8 level)
{
	memzero_explicit(bt, RPDFS_BLOCK_SIZE);

	bt->nr_items = 0;
	bt->avail_free = cpu_to_le16(RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_btree_block));
	bt->total_free = bt->avail_free;
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
	init_block(bt, 0);
	root->ref = *ref;
	root->height = 1;
}

static inline u64 key_coll(u64 key)
{
	return key & RPDFS_BTREE_KEY_COLL_MASK;
}

static inline u64 key_no_coll(u64 key)
{
	return key & ~RPDFS_BTREE_KEY_COLL_MASK;
}

static u16 item_off(struct rpdfs_btree_block *bt, u16 ind)
{
	return (le64_to_cpu(bt->items[ind]) << RPDFS_BTREE_ITEM_OFF_SHIFT) &
		RPDFS_BTREE_ITEM_OFF_MASK;
}

static void set_item_off(struct rpdfs_btree_block *bt, u16 ind, u16 off)
{
	bt->items[ind] = (bt->items[ind] & cpu_to_le64(~RPDFS_BTREE_ITEM_OFF_PACK_MASK)) |
			 cpu_to_le64(off >> RPDFS_BTREE_ITEM_OFF_SHIFT);
}

static u16 item_size(struct rpdfs_btree_block *bt, u16 ind)
{
	return (le64_to_cpu(bt->items[ind]) >> RPDFS_BTREE_ITEM_SIZE_SHIFT) &
		RPDFS_BTREE_ITEM_SIZE_MASK;
}

static u64 item_key(struct rpdfs_btree_block *bt, u16 ind)
{
	return le64_to_cpu(bt->items[ind]) >> RPDFS_BTREE_ITEM_KEY_SHIFT;
}

static void set_item_key(struct rpdfs_btree_block *bt, u16 ind, u64 key)
{
	bt->items[ind] = (bt->items[ind] &
			  cpu_to_le64(~(RPDFS_BTREE_ITEM_KEY_MASK << RPDFS_BTREE_ITEM_KEY_SHIFT))) |
			 cpu_to_le64(key << RPDFS_BTREE_ITEM_KEY_SHIFT);
}

static __le64 pack_item(u64 key, u16 off, u16 size)
{
	WARN_ON_ONCE(off & ~RPDFS_BTREE_ITEM_OFF_MASK);
	WARN_ON_ONCE(size & ~RPDFS_BTREE_ITEM_SIZE_MASK);

	return cpu_to_le64((key << RPDFS_BTREE_ITEM_KEY_SHIFT) |
			   ((size & RPDFS_BTREE_ITEM_SIZE_MASK) << RPDFS_BTREE_ITEM_SIZE_SHIFT) |
			   ((off & RPDFS_BTREE_ITEM_OFF_MASK) >> RPDFS_BTREE_ITEM_OFF_SHIFT));
}

static void *item_val(struct rpdfs_btree_block *bt, u16 ind)
{
	u16 off = item_off(bt, ind);

	BUG_ON(off < offsetof(struct rpdfs_btree_block, items[le16_to_cpu(bt->nr_items)]));
	BUG_ON(off > (RPDFS_BLOCK_SIZE - RPDFS_BTREE_VAL_ALIGN));
	BUG_ON(!IS_ALIGNED(off, RPDFS_BTREE_VAL_ALIGN));

	return (void *)bt + off;
}

static u16 aligned_val_size(u16 val_size)
{
	return ALIGN(val_size, RPDFS_BTREE_VAL_ALIGN);
}

static u16 full_item_size(u16 val_size)
{
	return sizeof_field(struct rpdfs_btree_block, items[0]) + aligned_val_size(val_size);
}

static u16 last_ind(struct rpdfs_btree_block *bt)
{
	if (WARN_ON_ONCE(bt->nr_items == 0))
		return 0;
	else
		return le16_to_cpu(bt->nr_items) - 1;
}

u64 rpdfs_btree_last_key(struct rpdfs_btree_block *bt)
{
	if (bt->nr_items != 0)
		return item_key(bt, last_ind(bt));
	else
		return 0;
}

/*
 * Find the first index in the item array whose key is greater than or
 * equal to the search key.  Callers can use the full precision key or
 * can mask off the collision bits depending on what they're looking
 * for.
 */
static u16 find_key_ind(struct rpdfs_btree_block *bt, u64 key)
{
	int start = 0;
	int end = (int)le16_to_cpu(bt->nr_items) - 1;
	int ind = 0;

	/* shift the key into the packed item position */
	key <<= RPDFS_BTREE_ITEM_KEY_SHIFT;

	while (start <= end) {
		ind = (start + end) >> 1;

		if (key > le64_to_cpu(bt->items[ind]))
			start = ++ind;
		else
			end = ind - 1;
	}

	return ind;
}

static int cmp_item_off(const void *A, const void *B)
{
	const __le64 *item_a = A;
	const __le64 *item_b = B;

	return rpdfs_compare(le64_to_cpu(*item_a) & RPDFS_BTREE_ITEM_OFF_PACK_MASK,
			     le64_to_cpu(*item_b) & RPDFS_BTREE_ITEM_OFF_PACK_MASK);
}

static int cmp_item_key(const void *A, const void *B)
{
	const __le64 *item_a = A;
	const __le64 *item_b = B;

	return rpdfs_compare(le64_to_cpu(*item_a), le64_to_cpu(*item_b));
}

/*
 * Return the offset of a value of the given size at the end of the
 * currently available free space at the center of the block.
 */
static u16 free_val_off(struct rpdfs_btree_block *bt, u16 val_size)
{
	return offsetof(struct rpdfs_btree_block, items[le16_to_cpu(bt->nr_items)]) +
		le16_to_cpu(bt->avail_free) - aligned_val_size(val_size);
}

static bool item_fits(__le16 free, u16 val_size)
{
	return full_item_size(val_size) <= le16_to_cpu(free);
}

/*
 * True if the caller must split the block before trying to insert an
 * item that wouldn't fit in the total free space in the block after
 * compaction.
 */
bool rpdfs_btree_must_split(struct rpdfs_btree_block *bt, u16 val_size)
{
	return !item_fits(bt->total_free, val_size);
}

/*
 * True if the block has enough free space that it's fallen under the
 * min item population and we should try and merge it with a sibling.
 */
bool rpdfs_btree_should_merge(struct rpdfs_btree_block *bt)
{
	return le16_to_cpu(bt->total_free) >= RPDFS_BTREE_MERGE_FREE_THRESH;
}

/*
 * Move the region of the item array from the index to the end of the
 * array in the given direction.  The index may fall outside the array
 * when inserting into an empty block or deleting the last sorted item
 * in the block.
 */
static inline void memmove_items(struct rpdfs_btree_block *bt, u16 ind, int dist)
{
	u16 nr = le16_to_cpu(bt->nr_items);

	if (ind < nr)
		memmove(&bt->items[ind + dist], &bt->items[ind],
			(nr - ind) * sizeof(bt->items[0]));
}

__always_unused
static bool check_item(struct rpdfs_btree_block *bt, u16 i, bool print)
{
	u64 key = item_key(bt, i);
	u16 off = item_off(bt, i);
	u16 size = item_size(bt, i);
	bool valid = true;
	size_t after_this = offsetof(struct rpdfs_btree_block, items[i + 1]);

	if (size > RPDFS_BTREE_MAX_VAL_SIZE) {
		if (print)
			rpdfs_err("item %u size %u is too large", i, size);
		valid = false;
	}

	if (!IS_ALIGNED(off, RPDFS_BTREE_VAL_ALIGN)) {
		if (print)
			rpdfs_err("item %u off %u not aligned", i, off);
		valid = false;
	}

	if (off < after_this || off >= RPDFS_BLOCK_SIZE) {
		if (print)
			rpdfs_err("item %u off %u outside values", i, off);
		valid = false;
	}

	if (off + size > RPDFS_BLOCK_SIZE) {
		if (print)
			rpdfs_err("item %u off+size %u+%u exceeds block", i, off, size);
		valid = false;
	}

	if (i > 0 && key <= item_key(bt, i - 1)) {
		if (print)
			rpdfs_err("item %u key %llx <= prev key %llx", i,
				  key, item_key(bt, i - 1));
		valid = false;
	}

	return valid;
}

__always_unused
static void check_block(struct rpdfs_btree_block *bt)
{
	bool valid = true;
	u16 total_val_size;
	u16 first_off;
	u16 first_free;
	u16 nr;
	int i;

	nr = le16_to_cpu(bt->nr_items);
	if (offsetof(struct rpdfs_btree_block, items[nr]) > RPDFS_BLOCK_SIZE) {
		/* guess a nr based on first sketchy item */
		nr = (RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_btree_block)) / sizeof(bt->items[0]);
		for (i = 0; i < nr; i++) {
			if (!check_item(bt, i, false)) {
				nr = i;
				break;
			}
		}
		rpdfs_err("nr %u is too large, guessing nr %u",
			  le16_to_cpu(bt->nr_items), nr);
		valid = false;
	}

	if (le16_to_cpu(bt->avail_free) > RPDFS_BLOCK_SIZE) {
		rpdfs_err("avail free %u is too large", le16_to_cpu(bt->avail_free));
		valid = false;
	}

	if (le16_to_cpu(bt->total_free) > RPDFS_BLOCK_SIZE) {
		rpdfs_err("total free %u is too large", le16_to_cpu(bt->total_free));
		valid = false;
	}

	if (le16_to_cpu(bt->avail_free) > le16_to_cpu(bt->total_free)) {
		rpdfs_err("avail free %u greater than total free %u",
			  le16_to_cpu(bt->avail_free), le16_to_cpu(bt->total_free));
		valid = false;
	}

	first_off = RPDFS_BLOCK_SIZE;
	total_val_size = 0;
	for (i = 0; i < nr; i++) {
		if (check_item(bt, i, true)) {
			first_off = min(item_off(bt, i), first_off);
			total_val_size += aligned_val_size(item_size(bt, i));
		} else {
			valid = false;
		}
	}

	first_free = offsetof(struct rpdfs_btree_block, items[nr]);

	/* avail_free isn't always updated on deletion, just can't cover a value */
	if (le16_to_cpu(bt->avail_free) > first_off - first_free) {
		rpdfs_err("avail free %u > first_off %u - first_free %u",
			  le16_to_cpu(bt->avail_free), first_off, first_free);
		valid = false;
	}

	if (le16_to_cpu(bt->total_free) != (RPDFS_BLOCK_SIZE - first_free - total_val_size)) {
		rpdfs_err("total free %u !~ free %u total %u",
			  le16_to_cpu(bt->total_free), first_free, total_val_size);
		valid = false;
	}

	if (!valid) {
		rpdfs_err("found invalid btree block:");
		rpdfs_err("  nr_items: %u", le16_to_cpu(bt->nr_items));
		rpdfs_err("  avail_free: %u", le16_to_cpu(bt->avail_free));
		rpdfs_err("  total_free: %u", le16_to_cpu(bt->total_free));
		rpdfs_err("  level: %u", bt->level);
		for (i = 0; i < nr; i++) {
			rpdfs_err("  [%u]: %llx (key %llx off %u size %u)",
				  i, le64_to_cpu(bt->items[i]),
				  item_key(bt, i), item_off(bt, i), item_size(bt, i));
		}

		print_hex_dump(KERN_DEBUG, "   ", DUMP_PREFIX_OFFSET, 16, 1,
			       bt, RPDFS_BLOCK_SIZE, true);
		BUG();
	}
}

/*
 * Defragment internal free space by moving all the values towards the
 * end of the block, gathering all free space in the center of the
 * block.  We sort the items by offset so we can move the items by
 * iterating in offset order.  Then we return the item headers to being
 * sorted by key.
 *
 * We could use per-cpu resources to have external offset sort index
 * but that could get obnoxious if the blocks got significantly larger
 * so we haven't bothered.  Compaction is rare so hopefully the doubled
 * sort cost isn't a problem.
 */
static void compact_items(struct rpdfs_btree_block *bt)
{
	void *src_val;
	s32 ind;
	u16 dst_off;
	u16 src_off;
	u16 bytes;
	u16 nr;

	if (bt->nr_items == 0 || bt->avail_free == bt->total_free)
		return;

	nr = le16_to_cpu(bt->nr_items);
	sort(bt->items, nr, sizeof(bt->items[0]), cmp_item_off, NULL);

	dst_off = RPDFS_BLOCK_SIZE;
	for (ind = nr - 1; ind >= 0; ind--) {
		src_off = item_off(bt, ind);
		bytes = aligned_val_size(item_size(bt, ind));
		dst_off -= bytes;

		if (src_off != dst_off) {
			src_val = item_val(bt, ind);
			set_item_off(bt, ind, dst_off);
			memmove(item_val(bt, ind), src_val, bytes);
		}
	}

	/* zero gathered free region before the now packed values */
	bytes = le16_to_cpu(bt->total_free) - le16_to_cpu(bt->avail_free);
	memzero_explicit((void *)bt + dst_off - bytes, bytes);

	bt->avail_free = bt->total_free;
	sort(bt->items, nr, sizeof(bt->items[0]), cmp_item_key, NULL);
}

/*
 * Insert a new item by extending the item array and adding the value to
 * the end of the available free space in the center of the block.  The
 * caller ensured that there's total free space available for the item
 * but we might have to compact to defragment it.
 *
 * Because we can compact we can move item values around.  Callers must
 * not hold pointers to values across any calls that can insert into
 * blocks.
 */
static void insert_item_kvec(struct rpdfs_btree_block *bt, u16 ind, u64 key,
			     struct kvec *kv, unsigned long nr_segs, u16 val_size)
{
	struct iov_iter iter;
	size_t copied;
	u16 bytes;
	u16 off;

	BUG_ON(ind > le16_to_cpu(bt->nr_items));
	BUG_ON(val_size > RPDFS_BTREE_MAX_VAL_SIZE);
	BUG_ON(!item_fits(bt->total_free, val_size));

	if (!item_fits(bt->avail_free, val_size))
		compact_items(bt);

	bytes = full_item_size(val_size);
	off = free_val_off(bt, val_size);

	memmove_items(bt, ind, 1);

	le16_add_cpu(&bt->avail_free, -bytes);
	le16_add_cpu(&bt->total_free, -bytes);
	le16_add_cpu(&bt->nr_items, 1);

	bt->items[ind] = pack_item(key, off, val_size);

	if (val_size) {
		iov_iter_kvec(&iter, ITER_SOURCE, kv, nr_segs, val_size);
		copied = copy_from_iter(item_val(bt, ind), val_size, &iter);
		BUG_ON(copied != val_size); /* no user */
	}
}

static void insert_item_val(struct rpdfs_btree_block *bt, u16 ind, u64 key,
			    void *val, u16 val_size)
{
	struct kvec kv = { .iov_base = val, .iov_len = val_size };

	insert_item_kvec(bt, ind, key, &kv, 1, val_size);
}

static void insert_item_item(struct rpdfs_btree_block *bt, u16 ind,
			     struct rpdfs_btree_block *src, u16 src_ind)
{
	return insert_item_val(bt, ind, item_key(src, src_ind), item_val(src, src_ind),
			       item_size(src, src_ind));
}

/*
 * Delete an item by removing its item header and zeroing its value
 * bytes.
 *
 * This almost certainly leaves behind fragmented free space in the
 * block that will later be reclaimed by compaction.
 */
static void delete_item(struct rpdfs_btree_block *bt, u16 ind)
{
	u16 bytes;
	u16 off;

	BUG_ON(ind >= le16_to_cpu(bt->nr_items));

	/* zero the value */
	bytes = item_size(bt, ind);
	if (bytes)
		memzero_explicit(item_val(bt, ind), bytes);

	/* update free tracking */
	off = item_off(bt, ind);
	bytes = full_item_size(item_size(bt, ind));
	if (off == free_val_off(bt, 0))
		le16_add_cpu(&bt->avail_free, bytes);
	else
		le16_add_cpu(&bt->avail_free, sizeof_field(struct rpdfs_btree_block, items[0]));
	le16_add_cpu(&bt->total_free, bytes);

	/* remove the item and update item count */
	memmove_items(bt, ind + 1, -1);
	le16_add_cpu(&bt->nr_items, -1);
	bt->items[le16_to_cpu(bt->nr_items)] = 0;
}

/*
 * The caller is moving items between blocks and wants to know if
 * collisions for the same base key are found in both blocks.  It will
 * keep moving until all the collisions are moved to the destination.
 */
static bool split_key_collisions(struct rpdfs_btree_block *dst, struct rpdfs_btree_block *src,
				 bool to_right)
{
	u16 s = to_right ? last_ind(src) : 0;
	u16 d = to_right ? 0 : last_ind(dst);

	return key_no_coll(item_key(src, s)) == key_no_coll(item_key(dst, d));
}

/*
 * Move items from the source block to the destination block.
 *
 * @to_right moves items in descending order from the end of the src
 * block to the front of the dst block.  When false it moves in the
 * opposite direction: in ascending order from the start of the src
 * block to the end of the dst block.
 *
 * @until_balanced stops moving once the consumed space in the two
 * blocks are roughly equal, rather than trying to move all items from
 * src to dst.  It checks after moving each item and might need to keep
 * moving so it doesn't split colliding keys across blocks.
 */
static void move_items(struct rpdfs_btree_block *dst, struct rpdfs_btree_block *src,
		       bool to_right, bool until_balanced)
{
	u16 src_ind;
	u16 dst_ind;

	if (src->nr_items == 0)
		return;

	if (to_right) {
		src_ind = le16_to_cpu(src->nr_items) - 1;
		dst_ind = 0;
	} else {
		src_ind = 0;
		dst_ind = le16_to_cpu(dst->nr_items);
	}

	while (src->nr_items != 0) {
		insert_item_item(dst, dst_ind, src, src_ind);
		delete_item(src, src_ind);

		if (until_balanced &&
		    (le16_to_cpu(dst->total_free) <= le16_to_cpu(src->total_free)) &&
		    !split_key_collisions(dst, src, to_right))
			break;

		if (to_right)
			src_ind--;
		else
			dst_ind++;
	}
}

static int copy_item_ref(struct rpdfs_btree_block *bt, u16 ind, struct rpdfs_block_ref *ref)
{
	const u16 sz = sizeof(struct rpdfs_block_ref);

	if (ind >= le16_to_cpu(bt->nr_items))
		return -EUCLEAN;

	if (item_size(bt, ind) != sz)
		return -EUCLEAN;

	memcpy(ref, item_val(bt, ind), sz);
	return 0;
}

/*
 * Given a parent btree block, set the caller's reference to the child
 * block that will contain the search key.
 */
int rpdfs_btree_find_child_ref(struct rpdfs_btree_block *bt, u64 key, u64 *ref_key,
			       struct rpdfs_block_ref *ref)
{
	u16 ind = find_key_ind(bt, key);

	*ref_key = item_key(bt, ind);
	return copy_item_ref(bt, ind, ref);
}

/*
 * Fill the references to the blocks that are on either side of the
 * block that contains the key.  When a block doesn't exist on either
 * side the corresponding reference is zeroed.
 */
int rpdfs_btree_find_sib_refs(struct rpdfs_btree_block *bt, u64 key, struct rpdfs_block_ref *left,
			      struct rpdfs_block_ref *right)
{
	u16 ind = find_key_ind(bt, key);
	int ret = 0;

	if (ind > 0)
		ret = copy_item_ref(bt, ind - 1, left);
	else
		*left = (struct rpdfs_block_ref) {0,};

	if ((ind + 1) < le16_to_cpu(bt->nr_items))
		ret = ret ?: copy_item_ref(bt, ind + 1, right);
	else
		*right = (struct rpdfs_block_ref) {0,};

	return ret;
}

/*
 * Parent keys must always include all the collision bits.
 */
static u64 parent_ref_key(struct rpdfs_btree_block *child)
{
	return rpdfs_btree_last_key(child) | RPDFS_BTREE_KEY_COLL_MASK;
}

/*
 * Split a block, moving items to a newly allocated block.  We move
 * items to balance the space they take up, not the number of items.
 * The new block is always empty so we can always move items.  We move
 * items to a new empty block to the left so that we only have to insert
 * a new parent item and don't have to modify the existing parent item's
 * key.
 */
void rpdfs_btree_split(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       struct rpdfs_block_ref *par_ref, struct rpdfs_btree_block *parent,
		       struct rpdfs_block_ref *sib_ref, struct rpdfs_btree_block *sib,
		       struct rpdfs_block_ref *ref, struct rpdfs_btree_block *bt)
{
	u64 key;

	/* link in allocated parent if we're splitting first block */
	if (root->ref.bnr == ref->bnr) {
		init_block(parent, bt->level + 1);
		insert_item_val(parent, 0, RPDFS_BTREE_ITEM_KEY_MASK,
				ref, sizeof(struct rpdfs_block_ref));
		root->ref = *par_ref;
		root->height++;
	}

	init_block(sib, bt->level);
	move_items(sib, bt, false, true);

	key = parent_ref_key(sib);
	insert_item_val(parent, find_key_ind(parent, key), key, sib_ref,
			sizeof(struct rpdfs_block_ref));
}

/*
 * Merge items from a sibling block into our block.  This is only called
 * if there is a parent block so there must be at least one sibling.
 *
 * Our block can be on either spine of the tree so we need to be able to
 * pull from a sibling on either side.  We have to update the key in the
 * parent reference item that separates the items in the two child
 * blocks, regardless.
 *
 * This can remove the sibling or parent from the tree.  If the sibling
 * loses its items it's removed and then if the parent only has one
 * remaining item it's also removed.  The caller can see blocks with no
 * items and free them in this case.
 */
void rpdfs_btree_merge(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
		       struct rpdfs_btree_block *parent, struct rpdfs_btree_block *sib,
		       struct rpdfs_btree_block *bt)
{
	bool until_balanced;
	bool to_right;
	u16 sib_ind;
	u16 ind;

	/* caller must have checked */
	BUG_ON(!rpdfs_btree_should_merge(bt));

	/* find our and sibling ref items */
	ind = find_key_ind(parent, rpdfs_btree_last_key(bt));
	if (rpdfs_btree_last_key(bt) < rpdfs_btree_last_key(sib))
		sib_ind = ind + 1;
	else
		sib_ind = ind - 1;
	to_right = sib_ind < ind;

	/* balance items between blocks if our sibling hasn't hit the merge threshold */
	until_balanced = !rpdfs_btree_should_merge(sib);

	move_items(bt, sib, to_right, until_balanced);

	/* update sib ref if it's still live and its last key changed */
	if (sib->nr_items != 0 && to_right)
		set_item_key(parent, sib_ind, parent_ref_key(sib));

	/* update our ref key if our last changed, using emptied sibs in case max */
	if (!to_right) {
		if (sib->nr_items == 0)
			set_item_key(parent, ind, item_key(parent, sib_ind));
		else
			set_item_key(parent, ind, parent_ref_key(bt));
	}

	/* delete ref to empty sibling, maybe also drop parent with single ref to us */
	if (sib->nr_items == 0) {
		delete_item(parent, sib_ind);
		if (le16_to_cpu(parent->nr_items) == 1) {
			copy_item_ref(parent, 0, &root->ref);
			root->height = bt->level + 1;
			delete_item(parent, 0);
		}
	}
}

static int call_item_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt, u16 ind,
			rpdfs_btree_item_cb_t cb, void *arg)
{
	return cb(rfi, item_key(bt, ind), item_val(bt, ind), item_size(bt, ind), arg);
}

#define for_each_item(bt_, key_, ind_) \
	for (ind_ = find_key_ind(bt_, key_); ind_ < le16_to_cpu(bt_->nr_items); ind_++)

#define for_each_key_collision_from(bt_, key_, ind_, from_) \
	for (ind_ = from_; \
	     (ind_ < le16_to_cpu(bt_->nr_items)) && \
		   (key_no_coll(key_) == key_no_coll(item_key(bt_, ind_))); \
	     ind_++)

#define for_each_key_collision(bt_, key_, ind_) \
	for_each_key_collision_from(bt_, key_, ind_, find_key_ind(bt_, key_no_coll(key_)))

static inline bool invalid_key(u64 key)
{
	return WARN_ON_ONCE(key & ~RPDFS_BTREE_ITEM_KEY_MASK) != 0;
}

/*
 * Call the callback on all the items whose keys collide with the
 * caller's.  If the caller has acquired a write handle on the block
 * then it can modify the item value in the callback but can't change
 * its size.
 *
 * If the callback only returns -ELOOP then -ENOENT is returned as this
 * is used to look for specific items.
 */
int rpdfs_btree_collisions_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt,
			      u64 key, rpdfs_btree_item_cb_t cb, void *arg)
{
	u16 ind;
	int ret;

	if (invalid_key(key)) {
		ret = -EINVAL;
		goto out;
	}

	for_each_key_collision(bt, key, ind) {
		ret = call_item_cb(rfi, bt, ind, cb, arg);
		if (ret != -ELOOP)
			goto out;
	}

	ret = -ENOENT;
out:
	return ret;
}

/*
 * Insert an item.  If the callback never returns an error then we'll
 * insert at the next available collision.  -EXFULL is returned if we
 * run out of collision bits.
 *
 * We can't insert if we haven't visited all the collisions.  It'd be
 * confusing to return >= 0 success without inserting so we warn if the
 * callback does so.
 */
int rpdfs_btree_insert_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt, u64 key,
			  rpdfs_btree_item_cb_t cb, void *arg, struct kvec *kv,
			  unsigned long nr_segs, u16 val_size)
{
	u64 coll;
	u16 base;
	u16 ind;
	int ret;

	if (invalid_key(key) || WARN_ON_ONCE(!item_fits(bt->total_free, val_size))) {
		ret = -EINVAL;
		goto out;
	}

	/* ignore caller's collision bits */
	coll = 0;
	base = find_key_ind(bt, key_no_coll(key));
	for_each_key_collision_from(bt, key, ind, base) {
		ret = call_item_cb(rfi, bt, ind, cb, arg);
		if (ret != -ELOOP) {
			WARN_ON_ONCE(ret >= 0);
			goto out;
		}

		if (coll == key_coll(item_key(bt, ind))) {
			if (++coll > RPDFS_BTREE_KEY_COLL_MASK) {
				ret = -EXFULL;
				goto out;
			}
		}
	}

	insert_item_kvec(bt, base + coll, key_no_coll(key) | coll, kv, nr_segs, val_size);
	ret = 0;
out:
	return ret;
}

/*
 * Delete an item when the callback returns success.  If the callback
 * only returns -ELOOP then we return -ENOENT.
 */
int rpdfs_btree_delete_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_root *root,
			  struct rpdfs_btree_block *bt, u64 key, rpdfs_btree_item_cb_t cb,
			  void *arg)
{
	u16 ind;
	int ret;

	if (invalid_key(key)) {
		ret = -EINVAL;
		goto out;
	}

	for_each_key_collision(bt, key, ind) {
		ret = call_item_cb(rfi, bt, ind, cb, arg);
		if (ret != -ELOOP) {
			if (ret >= 0) {
				delete_item(bt, ind);
				if (bt->nr_items == 0)
					memzero_explicit(root, sizeof(struct rpdfs_btree_root));
			}
			goto out;
		}
	}

	ret = -ENOENT;
out:
	return ret;
}

/*
 * Call the callback for each item in the block, sorted by key, starting
 * with the caller's key.  This works with full precision keys.
 *
 * This is used to iterate over all the items in the block so it
 * directly returns the last callback return.  The caller can see -ELOOP
 * to indicate if it should continue from the next leaf block of items.
 *
 * The caller's key can end in the leaf block after all keys so we won't
 * call the callback.  We default to returning -ELOOP to advance to the
 * next leaf.
 */
int rpdfs_btree_items_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_block *bt, u64 key,
			 rpdfs_btree_item_cb_t cb, void *arg)
{
	u16 ind;
	int ret;

	if (invalid_key(key)) {
		ret = -EINVAL;
		goto out;
	}

	ret = -ELOOP;
	for_each_item(bt, key, ind) {
		ret = call_item_cb(rfi, bt, ind, cb, arg);
		if (ret != -ELOOP)
			break;
	}
out:
	return ret;
}
