/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/timekeeping.h>

#include "block.h"
#include "format-block.h"
#include "inode.h"
#include "mkfs.h"
#include "super.h"

/*
 * This is a hack while we're bringing the system up.
 */

int rpdfs_mkfs(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_block_handle *hnd = NULL;
	struct rpdfs_inode *rinode;
	u64 bnr;
	int ret;

	bnr = rpdfs_ino_bnr(RPDFS_ROOT_INO);

	ret = rpdfs_block_acquire(rfi, bnr, &hnd, RBAF_WRITE | RBAF_OVERWRITE);
	if (ret < 0)
		goto out;

	memset(hnd->data, 0, RPDFS_BLOCK_SIZE);
	rinode = hnd->data;

	rinode->ig.ino = cpu_to_le64(RPDFS_ROOT_INO);
	rinode->ig.gen = cpu_to_le64(RPDFS_ROOT_GEN);
	rinode->size = cpu_to_le64(RPDFS_EMPTY_DIR_LEN);
	rinode->version = cpu_to_le64(1);
	rinode->nlink = cpu_to_le32(2);
	rinode->mode = cpu_to_le32(S_IFDIR | 0755);
	rinode->atime_nsec = cpu_to_le64(ktime_get_real_ns());
	rinode->ctime_nsec = rinode->atime_nsec;
	rinode->mtime_nsec = rinode->atime_nsec;
	rinode->crtime_nsec = rinode->atime_nsec;

	rpdfs_block_dirty(rfi, 0, hnd);
	rpdfs_block_release(rfi, &hnd);
	ret = rpdfs_block_flush(rfi, bnr, true);
out:
	return ret;
}
