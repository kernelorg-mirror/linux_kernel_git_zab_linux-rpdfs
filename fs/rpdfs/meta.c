/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include "aops.h"
#include "format-block.h"
#include "format-msg.h"
#include "inode.h"
#include "map.h"
#include "meta.h"
#include "pr.h"
#include "super.h"

/*
 * We collapse the per-type index namespaces into one index namespace
 * for the folios in the shadow inode address_space.
 */
enum {
	RPDFS_META_INODE = 0,
	RPDFS_META_XATTRS,
	RPDFS_META_DIRENTS,
};

/*
 * An in-memory vfs "shadow" inode is allocated for each real fs inode
 * to provide an address space for caching metadata blocks.
 *
 * Our large block keys that identify blocks over the network don't map
 * to a nice dense contiguous pgoff index space that'd work well with
 * the xa trees.  And even if they did, we wouldn't appreciate the
 * global contention of one address space.
 *
 * Callers map classes of metadata blocks down to low and relatively
 * contiguous indexes in each inode's shadow address space.  All inodes
 * have the inode block and xattr blocks.  Then dir and data blocks will
 * have mapping blocks.  Everything else (data, dirents, symlink
 * targets) go in i_mapping in the real inode.
 */

static struct address_space *shadow_mapping(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	return ri->shadow_inode->i_mapping;
}

/*
 * Give the caller a description of the location of the folio for the
 * key in the vfs.  The igd will describe the inode, and whether it's
 * the shadow meta inode, and the index will be set to the pgoff_t index
 * in the address space.
 */
int rpdfs_folio_loc_from_block_key(struct rpdfs_iget_data *igd, pgoff_t *index,
				   struct rpdfs_block_key *key)
{
	u8 type = le64_to_cpu(key->k[2]) >> RPDFS_BLOCK_KEY_TYPE__SHIFT;
	u64 ind = le64_to_cpu(key->k[2]) & RPDFS_BLOCK_KEY_INDEX__MASK;
	int ret;

	if (ind > (pgoff_t)U64_MAX) {
		ret = -EINVAL;
		goto out;
	}

	igd->is_shadow = true;

	switch (type) {
	case RPDFS_BLOCK_KEY_TYPE_INODE:
		*index = RPDFS_META_INODE;
		break;
	case RPDFS_BLOCK_KEY_TYPE_XATTR:
		*index = RPDFS_META_XATTRS;
		break;
	case RPDFS_BLOCK_KEY_TYPE_DIRENT:
		*index = RPDFS_META_DIRENTS;
		break;
	case RPDFS_BLOCK_KEY_TYPE_DATA:
		igd->is_shadow = false;
		*index = ind;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	igd->ino.i[0] = key->k[0];
	igd->ino.i[1] = key->k[1];
	ret = 0;
out:
	return ret;
}

/*
 * rpdfs_meta_get_folio() checked that indices were valid for the inode
 * as they were created.  We could store this in a folio->private but we
 * need a unique mapping to folio->index regardless so we might as well
 * spend a few cycles to save memory.
 */
void rpdfs_block_key_from_folio(struct rpdfs_block_key *key, struct folio *folio)
{
	struct rpdfs_inode_info *ri = RPDFS_FOLIO_I(folio);
	u64 t_index;
	u8 type;

	if (ri->is_shadow) {
		/* map meta indexes to the block key */
		switch (folio->index) {
		case RPDFS_META_INODE:
			type = RPDFS_BLOCK_KEY_TYPE_INODE;
			t_index = 0;
			break;
		case RPDFS_META_XATTRS:
			type = RPDFS_BLOCK_KEY_TYPE_XATTR;
			t_index = folio->index - RPDFS_META_XATTRS;
			break;
		case RPDFS_META_DIRENTS:
			type = RPDFS_BLOCK_KEY_TYPE_DIRENT;
			t_index = folio->index - RPDFS_META_DIRENTS;
			break;
		default:
			BUG();
		}
	} else {
		/* regular inode data folios use the index directly */
		type = RPDFS_BLOCK_KEY_TYPE_DATA;
		t_index = folio->index;
	}

	rpdfs_block_key_init(key, &ri->ino, type, t_index);
}

/*
 * This is a bit noisy but it lets us reuse the key->folio translation
 * that we have to do for incoming network messages.
 */
static int folio_index_from_type(struct inode *inode, u8 type, u64 t_index, pgoff_t *f_index)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_iget_data igd;
	struct rpdfs_block_key key;
	int ret;

	rpdfs_block_key_init(&key, &ri->ino, type, t_index);
	ret = rpdfs_folio_loc_from_block_key(&igd, f_index, &key);
	if (ret == 0 && !igd.is_shadow)
		ret = -EINVAL;
	return ret;
}

/*
 * Get a folio that contains the block of the described type and index.
 * Modeled loosely after __filemap_get_folio, except that we have much
 * less variation in callers.
 *
 * Generally folios are returned without the folio lock held.  Callers
 * are expected to have higher level serialization of safe access to the
 * block's contents.
 *
 * The waiting flags (_READ, _WRITE) are killable and only suitable for
 * task contexts.
 *
 * MGF_NEW: return a locked folio that is not uptodate and whose
 * contents are uninitialized.  The caller must unlock, and must either
 * mark the folio in error or initialize the contents and set it
 * uptodate.
 *
 * MGF_READ: return a folio that is uptodate.  The folio may be
 * undergoing writeback.
 *
 * MGF_WRITE: return a locked folio that is uptodate and not under
 * writeback.  The folio is returned locked so that writeback won't
 * start until the caller unlocks.
 */
struct folio *rpdfs_meta_get_folio(struct inode *inode, mgf_t mgf, u8 type, u64 t_index)
{
	struct address_space *mapping = shadow_mapping(inode);
	struct folio *folio;
	pgoff_t f_index;
	int ret;

	BUILD_BUG_ON(PAGE_SIZE != RPDFS_BLOCK_SIZE);

	if (WARN_ON_ONCE(mgf == 0 || (mgf & MGF__INVALID)) ||
	    WARN_ON_ONCE((mgf & MGF_NEW) && (mgf & ~MGF_NEW)) ||
	    WARN_ON_ONCE((mgf & MGF_READ) && (mgf & MGF_WRITE))) {
		ret = -EINVAL;
		goto out;
	}

	ret = folio_index_from_type(inode, type, t_index, &f_index);
	if (ret < 0) {
		folio = ERR_PTR(ret);
		goto out;
	}

	if (mgf & MGF_NEW) {
		folio = __filemap_get_folio(mapping, f_index, FGP_LOCK|FGP_CREAT, GFP_NOFS);
		goto out;
	}

	folio = mapping_read_folio_gfp(mapping, f_index, GFP_NOFS);
	if (IS_ERR(folio))
		goto out;

	if (mgf & MGF_WRITE) {
		ret = folio_lock_killable(folio);
		if (ret == 0) {
			ret = folio_wait_writeback_killable(folio);
			if (ret < 0)
				folio_unlock(folio);
		}
		if (ret < 0) {
			folio_put(folio);
			folio = ERR_PTR(ret);
			goto out;
		}
	}

out:
	if (!IS_ERR(folio))
		rpdfs_prd("mgf %x type %u index %llu "RFF, mgf, type, t_index, RFA(folio));
	else
		rpdfs_prd("mgf %x type %u index %llu ret %ld", mgf, type, t_index, PTR_ERR(folio));

	return folio;
}

void rpdfs_meta_dirty_folio(struct folio *folio)
{
	filemap_dirty_folio(folio->mapping, folio);
}

/*
 * Allocate a meta inode and attach it to the real inode.
 */
int rpdfs_meta_alloc_shadow(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct rpdfs_inode_info *ri = RPDFS_I(inode);
	struct rpdfs_iget_data igd = { ri->ino, true };
	struct inode *shadow;
	int ret;

	if (WARN_ON_ONCE(ri->shadow_inode)) {
		ret = -EINVAL;
		goto out;
	}

	shadow = rpdfs_new_inode(sb, &igd);
	if (IS_ERR(shadow)) {
		ret = PTR_ERR(shadow);
		goto out;
	}

	/* aops uses very view fields in the inode */
	shadow->i_mode = inode->i_mode;
	shadow->i_mapping->a_ops = &rpdfs_aops;
	/* make sure all index values are possible */
	i_size_write(shadow, OFFSET_MAX);

	/* inode ri holds the shadow iget until the inode is evicted */
	ri->shadow_inode = shadow;
	unlock_new_inode(shadow);
	ret = 0;
out:
	return ret;
}

/*
 * This is also called on the shadow inode itself but it's naturally a
 * nop because the shadow inode pointer will always be null.
 */
void rpdfs_meta_evict_shadow(struct inode *inode)
{
	struct rpdfs_inode_info *ri = RPDFS_I(inode);

	/* XXX calling shadow iput inside the inode's final_iput->evict makes me nervous! */
	if (ri->shadow_inode) {
		iput(ri->shadow_inode);
		ri->shadow_inode = NULL;
	}
}
