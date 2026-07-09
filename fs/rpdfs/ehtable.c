/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/pagemap.h>
#include <linux/bitfield.h>
#include <linux/unaligned.h>
#include <linux/sort.h>
#include "ehtable.h"
#include "format-block.h"
#include "meta.h"

/*
 * This implements "extendible hashing" with blocks as buckets.
 *
 * Items are stored at the hash of their large key.  The interface
 * manages collisions of hash values and assigns unique stored positions
 * in the table derived from the hash values.
 *
 * A map block with bits that operate as a buddy allocator tracks
 * expanding and contracting blocks in the table.  The number of blocks
 * in the table is limited by the number of bits in one mapping block.
 * (NYI, obv.)
 *
 * Each block contains a hash table that uses open addressing and robin
 * hood probing.  High bits of the hash are used to determine the
 * starting location in the block.  We store the probe length in each
 * entry (distance from stored entry to its starting location) and use
 * deletion which moves following entries back into the vacated deletion
 * rather than marking tombstones.
 *
 * This relies on even distribution of hashed key values.  If there's an
 * uneven concentration of hash values then a block can fill and return
 * out of space errors while the whole hash table stores very few items.
 */

static void set_key_val_sizes(struct rpdfs_ehtable_item *item, u16 key_size, u8 val_size)
{
	BUG_ON(key_size > RPDFS_EHTABLE_MAX_KEY_SIZE);
	BUG_ON(val_size > RPDFS_EHTABLE_MAX_VAL_SIZE);

	put_unaligned_le16(FIELD_PREP(RPDFS_EHTABLE_KEY_SIZE_FIELD, key_size) |
			   FIELD_PREP(RPDFS_EHTABLE_VAL_SIZE_FIELD, val_size), &item->key_val_sizes);
}

static u16 get_key_size(struct rpdfs_ehtable_item *item)
{
	return FIELD_GET(RPDFS_EHTABLE_KEY_SIZE_FIELD, get_unaligned_le16(&item->key_val_sizes));
}

static u8 get_val_size(struct rpdfs_ehtable_item *item)
{
	return FIELD_GET(RPDFS_EHTABLE_VAL_SIZE_FIELD, get_unaligned_le16(&item->key_val_sizes));
}

static u16 item_size_sizes(u16 key_size, u8 val_size)
{
	return sizeof(struct rpdfs_ehtable_item) + key_size + val_size;
}

static u16 item_size(struct rpdfs_ehtable_item *item)
{
	return item_size_sizes(get_key_size(item), get_val_size(item));
}

static u16 base_entry_index(struct rpdfs_ehtable_block *ehb, u32 hash)
{
	return (hash >> (32 - RPDFS_EHTABLE_ENTRIES_SHIFT - ehb->depth))
		& RPDFS_EHTABLE_ENTRIES_MASK;
}

static struct rpdfs_ehtable_entry *base_entry(struct rpdfs_ehtable_block *ehb, u32 hash)
{
	return &ehb->entries[base_entry_index(ehb, hash)];
}

static struct rpdfs_ehtable_item *offset_item(struct rpdfs_ehtable_block *ehb, u16 offset)
{
	BUG_ON(offset > (RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_ehtable_item)));

	return (void *)ehb + offset;
}

static u16 ptr_offset(struct rpdfs_ehtable_block *ehb, void *ptr)
{
	size_t off = ptr - (void *)ehb;

	BUG_ON(ptr < (void *)ehb || off >= RPDFS_BLOCK_SIZE);
	return off;
}

static u16 free_offset(struct rpdfs_ehtable_block *ehb)
{
	return RPDFS_BLOCK_SIZE - le16_to_cpu(ehb->tail_free);
}

static struct rpdfs_ehtable_item *ent_item(struct rpdfs_ehtable_block *ehb,
					   struct rpdfs_ehtable_entry *ent)
{
	return offset_item(ehb, le16_to_cpu(ent->offset));
}

static u16 ent_index(struct rpdfs_ehtable_block *ehb, struct rpdfs_ehtable_entry *ent)
{
	size_t ind = ent - ehb->entries;

	BUG_ON(ent < ehb->entries || ind >= RPDFS_EHTABLE_ENTRIES);
	return ind;
}

static bool ent_is_empty(struct rpdfs_ehtable_entry *ent)
{
	return ent->offset == 0;
}

/*
 * We clear the high bit of the pos to avoid negative long seekdir args.
 */
static u32 pos_base(u32 hash)
{
	return (hash >> 1) & ~RPDFS_EHTABLE_POS_MASK;
}

static u32 least_pos_hash(u32 pos)
{
	return (pos & ~RPDFS_EHTABLE_POS_MASK) << 1;
}

static u32 ent_pos(const struct rpdfs_ehtable_entry *ent)
{
	return pos_base(le32_to_cpu(ent->hash)) | ent->pos;
}

static void *item_key(struct rpdfs_ehtable_item *item)
{
	return (void *)(item + 1);
}

static void *item_val(struct rpdfs_ehtable_item *item)
{
	return item_key(item) + get_key_size(item);
}

#define __for_each_entry(ehb, INIT, ent, pl) \
	for (ent = INIT, pl = 0; \
	     (ent = &ehb->entries[(ent - ehb->entries) & RPDFS_EHTABLE_ENTRIES_MASK]); \
	     ent++, pl++)

#define for_each_entry(ehb, hash, ent, pl) \
	__for_each_entry(ehb, base_entry(ehb, hash), ent, pl)

#define for_each_entry_continue(ehb, cont, ent, pl) \
	__for_each_entry(ehb, (cont + 1), ent, pl)

static bool keys_match(u32 a_hash, const void *a_key, u16 a_size,
		       u32 b_hash, const void *b_key, u16 b_size)
{
	return a_hash == b_hash && a_size == b_size && memcmp(a_key, b_key, a_size) == 0;
}

static void init_zeroed_ehb(struct rpdfs_ehtable_block *ehb, u8 depth)
{
	ehb->tail_free = cpu_to_le16(RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_ehtable_block));
	ehb->total_free =  ehb->tail_free;
	ehb->depth = depth;
}

/*
 * Look for an entry matching the caller's item's key.
 */
static struct rpdfs_ehtable_entry *lookup_entry(struct rpdfs_ehtable_block *ehb,
						struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_entry *ent;
	struct rpdfs_ehtable_item *item;
	u16 pl;

	for_each_entry(ehb, iargs->hash, ent, pl) {
		if (ent_is_empty(ent) || pl > ent->probe_len)
			break;

		item = ent_item(ehb, ent);
		if (keys_match(le32_to_cpu(ent->hash), item_key(item), get_key_size(item),
			       iargs->hash, iargs->key, iargs->key_size))
			return ent;
	}

	return NULL;
}

/*
 * It's the callers responsibility to make sure the population is low
 * enough to ensure that insertion will find an empty entry.
 */
static int insert_entry(struct rpdfs_ehtable_block *ehb, struct rpdfs_ehtable_item_args *iargs,
			u16 offset)
{
	struct rpdfs_ehtable_entry ins = {
		.hash = cpu_to_le32(iargs->hash),
		.offset = cpu_to_le16(offset),
	};
	struct rpdfs_ehtable_item *item;
	struct rpdfs_ehtable_entry *ent;
	unsigned long pos_bits = 0;
	u8 pl;
	int ret;

	/* search for existing key in collisions and record used pos bits */
	for_each_entry(ehb, iargs->hash, ent, pl) {
		if (ent_is_empty(ent) || pl > ent->probe_len)
			break;

		item = ent_item(ehb, ent);
		if (keys_match(le32_to_cpu(ent->hash), item_key(item), get_key_size(item),
			       iargs->hash, iargs->key, iargs->key_size)) {
			ret = -EEXIST;
			goto out;
		}

		if (pos_base(le32_to_cpu(ent->hash)) == pos_base(iargs->hash))
			set_bit(ent->pos, &pos_bits);
	}

	ins.pos = find_first_zero_bit(&pos_bits, BITS_PER_LONG);
	if (ins.pos > RPDFS_EHTABLE_POS_MASK) {
		ret = -ENOSPC;
		goto out;
	}

	/* now actually insert at the pos */
	for_each_entry(ehb, iargs->hash, ent, ins.probe_len) {
		if (ent->offset == 0) {
			*ent = ins;
			break;
		}

		if (ins.probe_len < ent->probe_len)
			swap(ins, *ent);
	}

	ret = 0;
out:
	return ret;
}

/*
 * Find and delete an entry that matches the key.  Returns errors if no
 * key was found or gives the caller the offset of the item whose entry
 * was removed.
 */
static int delete_entry(struct rpdfs_ehtable_block *ehb, struct rpdfs_ehtable_item_args *iargs,
			u16 *offset)
{
	struct rpdfs_ehtable_item *item;
	struct rpdfs_ehtable_entry *ent;
	struct rpdfs_ehtable_entry *del;
	u8 pl;
	int ret;

	/* search for the entry to delete, setting offset */
	for_each_entry(ehb, iargs->hash, ent, pl) {
		if (ent_is_empty(ent) || pl > ent->probe_len) {
			ret = -ENOENT;
			goto out;
		}

		item = ent_item(ehb, ent);
		if (keys_match(le32_to_cpu(ent->hash), item_key(item), get_key_size(item),
			       iargs->hash, iargs->key, iargs->key_size)) {
			*offset = le16_to_cpu(ent->offset);
			del = ent;
			break;
		}
	}

	/* move later entries with nonzero probe length back to fill the gap, finally clearing */
	for_each_entry_continue(ehb, del, ent, pl) {
		if (ent_is_empty(ent) || ent->probe_len == 0) {
			memset(del, 0, sizeof(struct rpdfs_ehtable_entry));
			break;
		}

		*del = *ent;
		del->probe_len--;
		del = ent;
	}

	ret = 0;
out:
	return ret;
}

/*
 * We find sort orders of entries in-place by sorting per-cpu arrays of
 * their indices.
 */
static DEFINE_PER_CPU(u16 [RPDFS_EHTABLE_ENTRIES], pcpu_indirect);

static struct rpdfs_ehtable_entry *indirect_entry(struct rpdfs_ehtable_block *ehb,
						  u16 *indirect, u16 i)
{
	return &ehb->entries[indirect[i]];
}

static int cmp_indirect_ent_offset(const void *A, const void *B, const void *priv)
{
	const struct rpdfs_ehtable_block *ehb = priv;
	const struct rpdfs_ehtable_entry *a = &ehb->entries[*(u16 *)A];
	const struct rpdfs_ehtable_entry *b = &ehb->entries[*(u16 *)B];

	return (int)le16_to_cpu(a->offset) - (int)le16_to_cpu(b->offset);
}

static int cmp_indirect_ent_pos(const void *A, const void *B, const void *priv)
{
	const struct rpdfs_ehtable_block *ehb = priv;
	const struct rpdfs_ehtable_entry *a = &ehb->entries[*(u16 *)A];
	const struct rpdfs_ehtable_entry *b = &ehb->entries[*(u16 *)B];

	return (s64)ent_pos(a) - (s64)ent_pos(b);
}

static bool should_compact(struct rpdfs_ehtable_block *ehb, u16 size)
{
	return le16_to_cpu(ehb->tail_free) < size && le16_to_cpu(ehb->total_free) >= size;
}

/*
 * Compact the block by moving all the items to the front of the block,
 * gathering free space at the end of the block.
 *
 * This isn't the most efficient thing in the world.  We're using a
 * trivial loop that moves items and updates the offsets individually.
 * Moving larger runs of items and then updating all their offsets would
 * probably be quicker.
 */
static void compact_items(struct rpdfs_ehtable_block *ehb)
{
	struct rpdfs_ehtable_entry *ent;
	struct rpdfs_ehtable_item *item;
	u16 *indirect;
	u16 size;
	u16 off;
	u16 nr;
	u16 i;
	u8 pl;

	if (ehb->tail_free == ehb->total_free)
		return;

	indirect = get_cpu_var(pcpu_indirect);
	nr = 0;

	/* gather populated indices */
	for_each_entry(ehb, 0, ent, pl) {
		if (!ent_is_empty(ent)) {
			indirect[nr++] = ent_index(ehb, ent);
			if (nr == le16_to_cpu(ehb->nr_entries))
				break;
		}
	}

	/* sort indirect indices by offset */
	sort_r(indirect, nr, sizeof(indirect[0]), cmp_indirect_ent_offset, NULL, ehb);

	off = ptr_offset(ehb, &ehb->entries[RPDFS_EHTABLE_ENTRIES]);
	for (i = 0; i < nr; i++) {
		ent = indirect_entry(ehb, indirect, i);
		item = ent_item(ehb, ent);
		size = item_size(item);

		if (le16_to_cpu(ent->offset) != off) {
			memmove(offset_item(ehb, off), item, size);
			ent->offset = cpu_to_le16(off);
		}
		off += size;
	}

	put_cpu_var(indirect);

	/* zero the newly vacated free space at the end */
	memset(offset_item(ehb, off), 0, free_offset(ehb) - off);

	ehb->tail_free = ehb->total_free;
}


/*
 * Copy the items in the block referenced by the indirect index array
 * into the buffer.  We fill an array of iargs structs from the start of
 * the buffer and copy keys and values from the end of the buffer.
 */
static int fill_item_args(struct rpdfs_ehtable_block *ehb, u16 *indirect, u16 nr,
			  void *buf, size_t size)
{
	struct rpdfs_ehtable_item_args *iargs;
	struct rpdfs_ehtable_item *item;
	struct rpdfs_ehtable_entry *ent;
	u16 copied = 0;
	u16 len;
	u16 i;

	for (i = 0; i < nr; i++) {
		ent = indirect_entry(ehb, indirect, i);
		item = ent_item(ehb, ent);
		len = sizeof(struct rpdfs_ehtable_item_args) + get_key_size(item) +
		      get_val_size(item);

		if (size < len)
			break;

		iargs = buf;
		iargs->hash = le32_to_cpu(ent->hash);
		iargs->pos = ent_pos(ent);
		iargs->key_size = get_key_size(item);
		iargs->val_size = get_val_size(item);
		iargs->val = buf + size - iargs->val_size;
		iargs->key = iargs->val - iargs->key_size;
		memcpy((void *)iargs->key, item_key(item), iargs->key_size);
		memcpy((void *)iargs->val, item_val(item), iargs->val_size);

		copied++;
		buf = iargs + 1;
		size -= len;
	}

	return copied;
}

/*
 * Copy items from the block to the caller's buffer.  We fill an array
 * of iargs structs at the start of the buffer that reference their keys
 * and values in the buffer.
 *
 * Items are copied in pos sorted order, starting with the caller's pos.
 * The number of items read and copied into the buffer is returned.
 *
 * We work in groups of populated entries that start with a base index.
 * In each group, we only collect the entries with a greater pos and
 * which are in our base group so that we skip wrapped entries when we
 * encounter them in the front of the block.  Once we hit an empty entry
 * we know we've hit a gap in the sort order by hash, so we must have
 * hit a gap in the sort order by pos.  We sort the entries by pos and
 * fill the buffer.
 */
static u16 read_items(struct rpdfs_ehtable_block *ehb, u32 pos, void *buf, size_t size)
{
	struct rpdfs_ehtable_item_args *iargs;
	struct rpdfs_ehtable_entry *ent;
	bool stop_at_empty = false;
	u16 *indirect;
	u16 copied = 0;
	u16 filled;
	u16 start;
	u16 nr = 0;
	u8 _unused;
	u8 pl = 0;

	/* entries aren't sorted by pos, be sure to check all pos at hash */
	start = least_pos_hash(pos);

	indirect = get_cpu_var(pcpu_indirect);

	for_each_entry(ehb, start, ent, _unused) {
		if (ent == &ehb->entries[RPDFS_EHTABLE_ENTRIES - 1])
			stop_at_empty = true;

		if (ent_is_empty(ent)) {
			if (nr > 0) {
				sort_r(indirect, nr, sizeof(indirect[0]),
				       cmp_indirect_ent_pos, NULL, ehb);
				filled = fill_item_args(ehb, indirect, nr, buf, size);
				if (filled == 0)
					break;

				iargs = buf;
				iargs += filled - 1;
				buf = iargs + 1;
				size = (void *)iargs->key - buf;
				copied += filled;

				if (filled < nr || copied == le16_to_cpu(ehb->nr_entries))
					break;

				nr = 0;
			}

			pl = 0;

			if (stop_at_empty)
				break;
		} else {
			if (ent->probe_len <= pl && ent_pos(ent) >= pos)
				indirect[nr++] = ent_index(ehb, ent);
			pl++;
		}
	}

	put_cpu_var(indirect);

	return copied;
}

static int insert_item(struct rpdfs_ehtable_desc *desc, struct rpdfs_ehtable_block *ehb,
		       struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_item *item;
	u16 size;
	u16 off;
	int ret;

	size = item_size_sizes(iargs->key_size, iargs->val_size);
	if (le16_to_cpu(ehb->nr_entries) >= RPDFS_EHTABLE_FULL_ENTRIES ||
	    le16_to_cpu(ehb->total_free) < size) {
		ret = -ENOSPC;
		goto out;
	}

	if (should_compact(ehb, size))
		compact_items(ehb);

	off = free_offset(ehb);
	ret = insert_entry(ehb, iargs, off);
	if (ret < 0)
		goto out;

	item = offset_item(ehb, off);
	set_key_val_sizes(item, iargs->key_size, iargs->val_size);
	memcpy(item_key(item), iargs->key, iargs->key_size);
	memcpy(item_val(item), iargs->val, iargs->val_size);

	le16_add_cpu(&ehb->nr_entries, 1);
	le16_add_cpu(&ehb->tail_free, -size);
	le16_add_cpu(&ehb->total_free, -size);

	le32_add_cpu(&desc->nr_keys, 1);
	le32_add_cpu(&desc->total_key_size, iargs->key_size);

	ret = 0;
out:
	return ret;
}

static int delete_item(struct rpdfs_ehtable_desc *desc, struct rpdfs_ehtable_block *ehb,
		       struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_item *item;
	u16 offset;
	u16 size;
	int ret;

	ret = delete_entry(ehb, iargs, &offset);
	if (ret < 0)
		goto out;

	item = offset_item(ehb, offset);
	size = item_size(item);

	le16_add_cpu(&ehb->nr_entries, -1);
	if (ptr_offset(ehb, item) == free_offset(ehb))
		le16_add_cpu(&ehb->tail_free, size);
	le16_add_cpu(&ehb->total_free, size);

	le32_add_cpu(&desc->nr_keys, -1);
	le32_add_cpu(&desc->total_key_size, -iargs->key_size);

	memset(item, 0, size);
	ret = 0;
out:
	return ret;
}

/*
 * XXX NYI: This would use the map block to find the block index for the
 * hash.  It'd split and merge as blocks filled and pairs drained.
 *
 * Callers that can add items to an empty table pass in MGF_NEW.  It's
 * translated to _WRITE if the table has entries.  This lets us return
 * -ENOENT for operations that don't have items.
 */
static struct folio *get_ehb_folio(struct inode *inode, struct rpdfs_ehtable_desc *desc,
				   mgf_t mgf, u8 type, u32 hash)
{
	struct rpdfs_ehtable_block *ehb;
	struct folio *folio;

	if (!(mgf & MGF_NEW) && desc->nr_keys == 0) {
		folio = ERR_PTR(-ENOENT);
		goto out;
	} else if ((mgf & MGF_NEW) && desc->nr_keys != 0) {
		mgf = MGF_WRITE;
	}

	folio = rpdfs_meta_get_folio(inode, mgf, type, 0);
	if (!IS_ERR(folio) && (mgf & MGF_NEW)) {
		ehb = folio_address(folio);
		folio_zero_segment(folio, 0, RPDFS_BLOCK_SIZE);
		init_zeroed_ehb(ehb, 0);
		folio_mark_uptodate(folio);
	}

out:
	return folio;
}

/*
 * Lookup an item in the hash table.  The iargs hash and key is set to
 * the key to search for.  A value buffer can be provided to get a copy
 * of the found item's value.
 *
 * On success, the pos and val_size in the iargs are set to the found
 * item.  This lets the caller know the value size regardless of their
 * provided buffer.  If a value buffer was provided in the iargs then
 * the min of the input val_size and the item's value is copied to the
 * buffer.
 *
 * The size of the copied buffer is returned, 0 if there was no buffer.
 */
int rpdfs_ehtable_lookup(struct inode *inode, struct rpdfs_ehtable_desc *desc,
			 u8 type, struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_block *ehb;
	struct rpdfs_ehtable_entry *ent;
	struct rpdfs_ehtable_item *item;
	struct folio *folio;
	int ret;

	folio = get_ehb_folio(inode, desc, MGF_READ, type, iargs->hash);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	ehb = folio_address(folio);
	ent = lookup_entry(ehb, iargs);
	if (ent) {
		item = ent_item(ehb, ent);
		if (iargs->val)
			ret = min(iargs->val_size, get_val_size(item));
		else
			ret = 0;
		if (ret > 0)
			memcpy(iargs->val, item_val(item), ret);

		iargs->pos = ent_pos(ent);
		iargs->val_size = get_val_size(item);
	} else {
		ret = -ENOENT;
	}
	folio_put(folio);
out:
	return ret;
}

/*
 * When adding items to the table we need to make sure their sizes can
 * be described by the items.
 */
static bool invalid_iargs(struct rpdfs_ehtable_item_args *iargs)
{
	return iargs->key_size > RPDFS_EHTABLE_MAX_KEY_SIZE ||
	       iargs->val_size > RPDFS_EHTABLE_MAX_VAL_SIZE;
}

int rpdfs_ehtable_insert(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			 struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_block *ehb;
	struct folio *folio;
	int ret;

	if (invalid_iargs(iargs)) {
		ret = -EINVAL;
		goto out;
	}

	folio = get_ehb_folio(inode, desc, MGF_NEW, type, iargs->hash);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	ehb = folio_address(folio);
	ret = insert_item(desc, ehb, iargs);
	if (ret == 0)
		rpdfs_meta_dirty_folio(folio);
	folio_unlock(folio);
	folio_put(folio);
out:
	return ret;
}

/*
 * Set an item in the hash table to the given value.  If an item with
 * the current key already exists then its value is updated.  The caller
 * can provide flags to return errors if the item must or must not
 * already exist.
 *
 * This is relatively rare so we don't mind the relative inefficiency of
 * implementing it in terms of full insertion and deletion.
 */
int rpdfs_ehtable_set(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
		      struct rpdfs_ehtable_item_args *iargs, int flags)
{
	struct rpdfs_ehtable_block *ehb;
	struct rpdfs_ehtable_entry *ent;
	struct rpdfs_ehtable_item *item;
	struct folio *folio;
	s16 delta;
	int ret;

	folio = get_ehb_folio(inode, desc, MGF_NEW, type, iargs->hash);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	ehb = folio_address(folio);

	ent = lookup_entry(ehb, iargs);
	if (ent && (flags & RPDFS_EHT_EEXIST)) {
		ret = -EEXIST;
		goto out;
	} else if (!ent && (flags & RPDFS_EHT_ENOENT)) {
		ret = -ENOENT;
		goto out;
	}

	if (ent) {
		item = ent_item(ehb, ent);
		delta = iargs->val_size - get_val_size(item);
	} else {
		delta = 0;
	}

	/* we'd retry getting the block and split to leave this much room */
	if (delta > le16_to_cpu(ehb->total_free)) {
		ret = -ENOSPC;
		goto out;
	}

	/* we've ensured that deletion must succeed */
	if (ent) {
		ret = delete_item(desc, ehb, iargs);
		if (ret) {
			ret = -EUCLEAN;
			goto out;
		}
	}

	/* this shouldn't fail if we deleted, we don't have to undo the deletion */
	ret = insert_item(desc, ehb, iargs);
	if (ret < 0)
		goto out;

	ret = 0;
	rpdfs_meta_dirty_folio(folio);
out:
	if (!IS_ERR(folio)) {
		folio_unlock(folio);
		folio_put(folio);
	}
	return ret;
}

/*
 * Delete an item from the table that matches the key.
 */
int rpdfs_ehtable_delete(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			 struct rpdfs_ehtable_item_args *iargs)
{
	struct rpdfs_ehtable_block *ehb;
	struct folio *folio;
	int ret;

	folio = get_ehb_folio(inode, desc, MGF_WRITE, type, iargs->hash);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	ehb = folio_address(folio);
	ret = delete_item(desc, ehb, iargs);
	if (ret == 0)
		rpdfs_meta_dirty_folio(folio);
	folio_unlock(folio);
	folio_put(folio);
out:
	return ret;
}

/*
 * Copy items from the table into the caller's buffer.  Entries are
 * copied in sort order starting with the provided pos.
 *
 * The buffer must be aligned to the alignment of the item_args.  On
 * return, the buffer will start with an array of item_args structs for
 * the number of items copied.  The pointers in each item_args struct
 * will point to the region of the buffer that contains the keys and
 * values that were copied.
 */
int rpdfs_ehtable_read_items(struct inode *inode, struct rpdfs_ehtable_desc *desc, u8 type,
			     u32 pos, void *buf, size_t size)
{
	struct rpdfs_ehtable_block *ehb;
	u32 hash = least_pos_hash(pos);
	struct folio *folio;
	int ret;

	if (WARN_ON_ONCE(!IS_ALIGNED((unsigned long)buf,
				     __alignof__(struct rpdfs_ehtable_item_args)))) {
		ret = -EINVAL;
		goto out;
	}

	folio = get_ehb_folio(inode, desc, MGF_READ, type, hash);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	ehb = folio_address(folio);
	ret = read_items(ehb, pos, buf, size);
	folio_put(folio);
out:
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}
