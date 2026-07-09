/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/timekeeping.h>

#include "format-block.h"
#include "meta.h"
#include "inode.h"
#include "mkfs.h"
#include "super.h"

/*
 * This is a hack while we're bringing the system up.
 */

int rpdfs_mkfs_root_inode(struct inode *inode)
{
	struct rpdfs_inode *rinode;
	struct folio *folio;
	int ret;

	folio = rpdfs_meta_get_folio(inode, MGF_NEW, RPDFS_BLOCK_KEY_TYPE_INODE, 0);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	folio_zero_segment(folio, 0, RPDFS_BLOCK_SIZE);

	rinode = folio_address(folio);
	rinode->ino = (struct rpdfs_inode_nr) RPDFS_INIT_ROOT_INODE_NR;
	rinode->size = cpu_to_le64(RPDFS_EMPTY_DIR_LEN);
	rinode->version = cpu_to_le64(1);
	rinode->nlink = cpu_to_le32(2);
	rinode->mode = cpu_to_le32(S_IFDIR | 0755);
	rinode->atime_nsec = cpu_to_le64(ktime_get_real_ns());
	rinode->ctime_nsec = rinode->atime_nsec;
	rinode->mtime_nsec = rinode->atime_nsec;
	rinode->crtime_nsec = rinode->atime_nsec;

	folio_mark_uptodate(folio);
	rpdfs_meta_dirty_folio(folio);
	folio_unlock(folio);
	folio_put(folio);
out:
	return ret;
}
