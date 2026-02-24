/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xxhash.h>

#include "balloc.h"
#include "btree_txn.h"
#include "dir.h"
#include "format-block.h"
#include "inode.h"

/*
 * A little container to describe a dirent.
 *
 * Sometimes they're allocated contiguously with the name following the
 * dirent so they can be copied into a dirent btree item.  Sometimes
 * they're on the stack and the name points to a name argument in the
 * caller.  So kd->{name,dent.name} should be used carefully.
 */
struct key_dent {
	struct rpdfs_btree_key key;
	const char *name;
	struct rpdfs_dirent dent;
};

/*
 * Return the name hash, masking off the low collision bit.
 */
static u64 dent_key_hash(struct rpdfs_btree_key *key)
{
	return le64_to_cpu(key->msq) & ~RPDFS_DIRENT_COLL_BIT;
}

static void init_dent_key(struct rpdfs_btree_key *key, u64 hash)
{
	key->msq = cpu_to_le64(hash);
	key->lsq = 0;
}

/*
 * Return unpadded bytes used by the dirent struct and its embedded name (no null term).
 */
static unsigned dent_size(unsigned name_len)
{
	return offsetof(struct rpdfs_dirent, name[name_len]);
}

static bool names_match(const char *a, unsigned a_len, const char *b, unsigned b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static bool item_name_matches(struct rpdfs_btree_item_args *bti, const char *name,
			      unsigned name_len)
{
	struct rpdfs_dirent *dent = (void *)bti->val;

	return names_match(dent->name, dent->name_len, name, name_len);
}

static bool item_hash_and_name_matches(struct rpdfs_btree_item_args *bti, struct key_dent *kd)
{
	return dent_key_hash(&bti->key) == dent_key_hash(&kd->key) &&
	       item_name_matches(bti, kd->name, kd->dent.name_len);
}

/*
 * The directory entries for . and .. are generated during lookup and
 * readdir and are not "real" directory entries stored as dirents. For
 * readdir to work properly, we need the position of each entry (its
 * hash value) to be stable. We also want to generate . and .. first
 * because it's easier than inserting them somewhere in the middle and
 * because applications like it that way.
 *
 * The solution is to reserve the hash values 0 for . and 1 for .. so
 * that we can return them first in readdir() and the positions returned
 * by readdir are strictly ascending.
 */
static u64 name_hash(const char *name, size_t name_len)
{
	u64 hash;

	if (is_dot_dotdot(name, name_len)) {
		if (name_len == 1)
			return RPDFS_DIRENT_DOT_HASH;
		else
			return RPDFS_DIRENT_DOT_DOT_HASH;
	}

	hash = xxh64(name, name_len, RPDFS_DIRENT_HASH_SEED) & RPDFS_DIRENT_HASH_MASK;

	if (hash < RPDFS_DIRENT_MIN_HASH)
		hash = RPDFS_DIRENT_MIN_HASH;

	return hash;
}

static void init_key_dent(struct key_dent *kd, const char *name, size_t name_len,
			  struct rpdfs_ino_gen *ig, bool copy_contig_name)
{
	BUG_ON(name_len == 0 || name_len > RPDFS_NAME_MAX);

	init_dent_key(&kd->key, name_hash(name, name_len));
	if (ig)
		kd->dent.ig = *ig;
	else
		kd->dent.ig = (struct rpdfs_ino_gen) { 0, };
	kd->dent.name_len = name_len;
	if (copy_contig_name) {
		memcpy(&kd->dent.name[0], name, name_len);
		kd->name = (char *)kd->dent.name;
	} else {
		kd->name = name;
	}
}

/*
 * We're looking up the key with the hash of the name without the
 * collision bit.  If the dirent exists then it can only be in the next
 * two items if they match the hash, without and with the collision bit.
 */
static int lookup_item_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_item_args *a,
			  struct rpdfs_btree_item_args *b, struct rpdfs_btree_item_args *c,
			  void *arg)
{
	struct key_dent *kd = arg;
	struct rpdfs_btree_item_args *match = NULL;
	struct rpdfs_dirent *dent;
	int ret;

	if (a && item_hash_and_name_matches(a, kd))
		match = a;
	else if (b && item_hash_and_name_matches(b, kd))
		match = b;

	if (match) {
		dent = (void *)match->val;
		kd->dent.ig = dent->ig;
		ret = 0;
	} else {
		ret = -ENOENT;
	}

	return ret;
}

/*
 * Search the dirents in the dir for the given name.  If it's found
 * return 0 and set the caller's ig to the found dent's ig.  Returns
 * -ENOENT if the name wasn't found.
 */
static int lookup_name(struct rpdfs_fs_info *rfi, struct inode *dir, const char *name,
		       size_t name_len, struct rpdfs_ino_gen *ig)
{

	DECLARE_RPDFS_TXN(txn);
	struct key_dent kd;
	int ret;

	init_key_dent(&kd, name, name_len, NULL, false);

	do {
		ret = rpdfs_inode_txn_prepare(rfi, &txn, dir, 0) ?:
		      rpdfs_btree_txn_prepare_lookup(rfi, &txn, &RPDFS_I(dir)->dirents,
						     &kd.key, lookup_item_cb, &kd);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));

	if (ret == 0)
		*ig = kd.dent.ig;

	rpdfs_txn_reset(rfi, &txn);
	return ret;
}

static struct dentry *rpdfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct rpdfs_ino_gen ig;
	struct inode *inode;
	int ret;

	if (dentry->d_name.len > RPDFS_NAME_MAX) {
		inode = ERR_PTR(-ENAMETOOLONG);
		goto out;
	}

	ret = lookup_name(rfi, dir, dentry->d_name.name, dentry->d_name.len, &ig);
	if (ret < 0 && ret != -ENOENT) {
		inode = ERR_PTR(ret);
		goto out;
	}

	if (ret == -ENOENT)
		inode = NULL;
	else
		inode = rpdfs_iget(sb, &ig);

out:
	/* d_splice_alias passes through ERR_PTR inodes */
	return d_splice_alias(inode, dentry);
}

/*
 * Examine existing items to decide if we can insert a new dirent.
 * Either both or b can be null.
 *
 * If either item has a matching hash and name then we return eexist.
 * If both items have matching hashes then we've run out of room for a
 * new item.
 *
 * XXX rename within a directory can be removing an existing item with a
 * colliding hash value.  We'd want to account for that to avoid an
 * (unlikely) spurious enospc.
 *
 * Returns error to abort or 0 with the insert args set to describe the
 * item to insert.
 */
static int add_entry_item_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_item_args *a,
			     struct rpdfs_btree_item_args *b, struct rpdfs_btree_item_args *ins,
			     void *arg)
{
	struct key_dent *kd = arg;

	ins->key = kd->key;

	if (a && dent_key_hash(&kd->key) == dent_key_hash(&a->key)) {
		if (item_name_matches(a, kd->name, kd->dent.name_len))
			return -EEXIST;

		if (b && dent_key_hash(&kd->key) == dent_key_hash(&b->key)) {
			if (item_name_matches(a, kd->name, kd->dent.name_len))
				return -EEXIST;
			return -ENOSPC;
		}

		ins->key.msq = a->key.msq ^ cpu_to_le64(RPDFS_DIRENT_COLL_BIT);
	}

	ins->val = &kd->dent;
	ins->val_size = dent_size(kd->dent.name_len);

	return 0;
}

/*
 * We use one has collision bit so we only have to test two items at
 * most.
 */
static int remove_entry_item_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_item_args *a,
				struct rpdfs_btree_item_args *b, struct rpdfs_btree_item_args *c,
				void *arg)
{
	struct key_dent *kd = arg;

	if (a && item_hash_and_name_matches(a, kd))
		return 0;
	else if (b && item_hash_and_name_matches(b, kd))
		return 1;
	else
		return -ENOENT;
}

/*
 * Modification is used to rewrite the target inode of the dent.  We
 * re-use the removal pair fn to discover which item to modify.
 */
static int modify_entry_item_cb(struct rpdfs_fs_info *rfi, struct rpdfs_btree_item_args *a,
				struct rpdfs_btree_item_args *b, struct rpdfs_btree_item_args *c,
				void *arg)
{
	struct key_dent *kd = arg;
	struct rpdfs_btree_item_args *mod;
	struct rpdfs_dirent *dent;
	int ret;

	ret = remove_entry_item_cb(rfi, a, b, c, arg);
	if (ret < 0)
		return ret;

	mod = ret == 0 ? a : b;
	dent = mod->val;
	dent->ig = kd->dent.ig;
	dent->pers_dtype = kd->dent.pers_dtype;

	return 0;
}

static int prepare_add_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_prepare_insert(rfi, txn, &ri->dirents, &kd->key,
					      dent_size(kd->dent.name_len),
					      add_entry_item_cb, kd);
}

static int apply_add_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			   struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_apply_insert(rfi, txn, &ri->dirents, &kd->key,
					    dent_size(kd->dent.name_len),
					    add_entry_item_cb, kd);
}

static int prepare_remove_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_prepare_delete(rfi, txn, &ri->dirents, &kd->key,
					      dent_size(kd->dent.name_len),
					      remove_entry_item_cb, kd);
}

static int apply_remove_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			      struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_apply_delete(rfi, txn, &ri->dirents, &kd->key,
					    dent_size(kd->dent.name_len),
					    remove_entry_item_cb, kd);
}

/*
 * We use the remove pair fn to return ENOENT without trying to modify the entries while
 * preparing.
 */
static int prepare_modify_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_prepare_modify(rfi, txn, &ri->dirents, &kd->key,
					      remove_entry_item_cb, kd);
}

static int apply_modify_entry(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
			      struct inode *dir, struct key_dent *kd)
{
	struct rpdfs_inode_info *ri = RPDFS_I(dir);

	return rpdfs_btree_txn_apply_modify(rfi, txn, &ri->dirents, &kd->key,
					    modify_entry_item_cb, kd);
}

static struct key_dent *alloc_key_dent(struct dentry *dentry, struct inode *inode)
{
	const char *name = dentry->d_name.name;
	const unsigned name_len = dentry->d_name.len;
	struct key_dent *kd;

	kd = kmalloc(offsetof(struct key_dent, dent.name[name_len]), GFP_NOFS);
	if (kd)
		init_key_dent(kd, name, name_len, inode ? rpdfs_inode_ig(inode) : NULL, true);

	return kd;
}

/*
 * Allocate an inode in a block transaction and return an allocated vfs
 * inode at its ino/gen position.  Like _iget, this inserts an I_NEW
 * inode in the cache to reserve the ino/gen before the transaction.
 * This avoids having to insert the inode in the non-blocking apply
 * phase of the transaction, and avoids having to unwind the transaction
 * if insertion fails.
 *
 * It means we have to peek at what the allocation will be, and that can
 * change if the transaction retries and gets different allocations.
 * Like iget, we'll remove and unhash the ino/gen that wasn't used.
 *
 * Is it a bit too cheeky that we're using iget_failed() to remove
 * inodes on errors?  It does all we want (unhash, mark bad, unlock_new,
 * iput) but we're not in iget.
 */
static struct inode *create_new_inode(struct mnt_idmap *idmap, struct inode *dir,
				      struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(dir);
	struct rpdfs_ino_gen ig = {0,};
	struct key_dent *kd = NULL;
	struct inode *inode = NULL;
	DECLARE_RPDFS_TXN(txn);
	u64 bnr;
	int ret;

	if (dentry) {
		kd = alloc_key_dent(dentry, NULL);
		if (!kd) {
			ret = -ENOMEM;
			goto out;
		}
	}

	do {
		ret = rpdfs_balloc_prepare_alloc_peek(rfi, &txn, &bnr);
		if (ret == 0) {
			if (le64_to_cpu(ig.ino) != bnr) {
				ig.ino = cpu_to_le64(bnr);
				ig.gen = cpu_to_le64(1);
				if (inode)
					iget_failed(inode);
				inode = rpdfs_new_inode(sb, &ig);
				if (IS_ERR(inode)) {
					ret = PTR_ERR(inode);
					goto out;
				}
				rpdfs_txn_force_retry(&txn);
				continue;
			}
			ret = rpdfs_inode_txn_prepare(rfi, &txn, dir, RBAF_WRITE);
		}
		if (ret == 0 && kd)
			ret = prepare_add_entry(rfi, &txn, dir, kd);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));
	if (ret < 0)
		goto out;

	/* allocation must have matched preparation */
	rpdfs_balloc_apply_alloc(rfi, &txn, &bnr);
	BUG_ON(le64_to_cpu(ig.ino) != bnr);

	/* update vfs inodes */
	inode_init_owner(idmap, inode, dir, mode);
	if (S_ISDIR(mode))
		set_nlink(inode, 2);
	else
		set_nlink(inode, 1);
	rpdfs_inode_init_ops(inode);

	if (S_ISDIR(mode))
		inc_nlink(dir);

	/* apply changes to referenced blocks */
	if (kd) {
		kd->dent.ig = ig;
		apply_add_entry(rfi, &txn, dir, kd);
	}

	/* sync changes to vfs inodes with inode blocks */
	rpdfs_inode_txn_update(rfi, &txn, dir);
	rpdfs_inode_txn_update(rfi, &txn, inode);

	ret = 0;
out:
	rpdfs_txn_reset(rfi, &txn);
	kfree(kd);

	if (ret < 0) {
		if (!IS_ERR_OR_NULL(inode))
			iget_failed(inode);
		inode = ERR_PTR(ret);
	}

	return inode;
}

static int create_and_instantiate_new(struct mnt_idmap *idmap, struct inode *dir,
				      struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = create_new_inode(idmap, dir, dentry, mode);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
	} else {
		d_instantiate_new(dentry, inode);
		ret = 0;
	}

	return ret;
}

static int rpdfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
			umode_t mode, bool excl)
{
	return create_and_instantiate_new(idmap, dir, dentry, S_IFREG | mode);
}

static struct dentry *rpdfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	return ERR_PTR(create_and_instantiate_new(idmap, dir, dentry, S_IFDIR | mode));
}

/*
 * The vfs has verified the cached directories.  Our inode refresh will
 * fail if the inodes have been reused, so we don't have to test if
 * they're still directories.
 *
 * We update the vfs structures here, rather than down in the dirent
 * structure helpers, so that we can perform one modification for the
 * whole of the transaction.  For example, not modifying the target
 * inode's nlink while the callbacks might want to dec and inc it as
 * entries are removed and added.
 */
static int rpdfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(old_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct key_dent *old_kd = NULL;
	struct key_dent *new_kd = NULL;
	DECLARE_RPDFS_TXN(txn);
	struct timespec64 now;
	int ret;

	if (flags & ~RENAME_NOREPLACE) {
		ret = -EINVAL;
		goto out;
	}

	if (old_dentry->d_name.len > RPDFS_NAME_MAX ||
	    new_dentry->d_name.len > RPDFS_NAME_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	old_kd = alloc_key_dent(old_dentry, old_inode);
	new_kd = alloc_key_dent(new_dentry, new_inode);
	if (!old_kd || !new_kd) {
		ret = -ENOMEM;
		goto out;
	}

	/* prepare all the blocks for in the txn */
	do {
		ret = rpdfs_inode_txn_prepare(rfi, &txn, old_dir, RBAF_WRITE) ?:
		      rpdfs_inode_txn_prepare(rfi, &txn, old_inode, RBAF_WRITE) ?:
		      prepare_remove_entry(rfi, &txn, old_dir, old_kd);
		if (ret == 0 && new_dir != old_dir)
			ret = rpdfs_inode_txn_prepare(rfi, &txn, new_dir, RBAF_WRITE);
		if (ret == 0) {
			if (new_inode) {
				ret = rpdfs_inode_txn_prepare(rfi, &txn, new_inode, RBAF_WRITE) ?:
				      prepare_modify_entry(rfi, &txn, new_dir, new_kd);
			} else {
				ret = prepare_add_entry(rfi, &txn, new_dir, new_kd);
			}
		}
	} while (rpdfs_txn_retry(rfi, &txn, &ret));
	if (ret < 0)
		goto out;

	/* apply changes to block structures */
	apply_remove_entry(rfi, &txn, old_dir, old_kd);
	if (new_inode)
		apply_modify_entry(rfi, &txn, new_dir, new_kd);
	else
		apply_add_entry(rfi, &txn, new_dir, new_kd);

	/* update vfs inodes: first dir sizes .. */
	i_size_write(old_dir, i_size_read(old_dir) - old_dentry->d_name.len);
	if (!new_inode)
               i_size_write(new_dir, i_size_read(new_dir) + new_dentry->d_name.len);
	/* .. then link counts .. */
	if (new_inode) {
		drop_nlink(new_inode);
		if (S_ISDIR(new_inode->i_mode)) {
			drop_nlink(new_dir);
			drop_nlink(new_inode);
		}
	}
	if (S_ISDIR(old_inode->i_mode) && (old_dir != new_dir)) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}
	/* .. and finally times */
	now = inode_set_ctime_current(old_dir);
	inode_set_mtime_to_ts(old_dir, now);
	if (new_dir != old_dir) {
		inode_set_ctime_to_ts(new_dir, now);
		inode_set_mtime_to_ts(new_dir, now);
	}
	inode_set_ctime_to_ts(old_inode, now);
	if (new_inode)
		inode_set_ctime_to_ts(new_inode, now);

	/* update block storage of vfs inodes */
	rpdfs_inode_txn_update(rfi, &txn, old_dir);
	rpdfs_inode_txn_update(rfi, &txn, old_inode);
	if (new_dir != old_dir)
		rpdfs_inode_txn_update(rfi, &txn, new_dir);
	if (new_inode)
		rpdfs_inode_txn_update(rfi, &txn, new_inode);

	ret = 0;
out:
	rpdfs_txn_reset(rfi, &txn);
	kfree(old_kd);
	kfree(new_kd);

	return ret;
}

static int prepare_copy_entries(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn,
				struct inode *inode, struct rpdfs_btree_key *key,
				void *buf, size_t size)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return rpdfs_btree_txn_prepare_copy_items(rfi, txn, &ri->dirents, key, buf, size);
}

/*
 * We don't want to fault emitting an entry while holding a block ref so
 * we bounce the entries we read through a buffer from which we then
 * emit.
 */
static int rpdfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	struct rpdfs_btree_item_args *bti;
	struct rpdfs_btree_key key;
	struct rpdfs_dirent *dent;
	struct page *page = NULL;
	DECLARE_RPDFS_TXN(txn);
	int ret;
	int i;

	if (!dir_emit_dots(file, ctx)) {
		ret = 0;
		goto out;
	}

	page = alloc_page(GFP_NOFS);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	for (;;) {
		init_dent_key(&key, ctx->pos);
		do {
			ret = rpdfs_inode_txn_prepare(rfi, &txn, inode, 0) ?:
			      prepare_copy_entries(rfi, &txn, inode, &key, page_address(page),
						   PAGE_SIZE);
		} while (rpdfs_txn_retry(rfi, &txn, &ret));
		if (ret <= 0)
			goto out;

		for (i = 0, bti = page_address(page); i < ret;
		     i++, bti = rpdfs_btree_next_copied_item(bti)) {
			dent = bti->val;

			ctx->pos = dent_key_hash(&bti->key);
			if (!dir_emit(ctx, dent->name, dent->name_len, le64_to_cpu(dent->ig.ino),
				      DT_UNKNOWN) || ctx->pos == U64_MAX) {
				ret = 0;
				goto out;
			}

			/* XXX worry more about eof, wrapping, and full precision hash */
			ctx->pos++;
		}

		rpdfs_txn_reset(rfi, &txn);
	}

	ret = 0;
out:
	rpdfs_txn_reset(rfi, &txn);
	if (page)
		__free_page(page);
	return ret;
}

const struct inode_operations rpdfs_dir_iops = {
	.create		= rpdfs_create,
	.getattr	= rpdfs_getattr,
	.lookup		= rpdfs_lookup,
	.mkdir		= rpdfs_mkdir,
	.rename		= rpdfs_rename,
	.setattr	= rpdfs_setattr,
};

const struct file_operations rpdfs_dir_fops = {
	.read		= generic_read_dir,
	.iterate_shared	= rpdfs_readdir,
};
