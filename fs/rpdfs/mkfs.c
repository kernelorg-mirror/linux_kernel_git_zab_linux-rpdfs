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
	DECLARE_RPDFS_TXN(txn);
	u64 bnr;
	int ret;

	bnr = rpdfs_ino_bnr(RPDFS_ROOT_INO);

	/*
	 * This works today because txn block prep doesn't verify block
	 * details, but once it checks read checksums this will fail and
	 * we'll need a richer txn interface for preparing to write to
	 * unused blocks.
	 */
	do {
		ret = rpdfs_txn_prepare_acquire(rfi, &txn, bnr, &hnd);
		if (ret == 0)
			rpdfs_txn_prepare_release(rfi, &txn, &hnd, RBAF_WRITE | RBAF_OVERWRITE);
	} while (rpdfs_txn_retry(rfi, &txn, &ret));
	if (ret < 0)
		goto out;

	ret = rpdfs_txn_use_prepared(rfi, &txn, bnr, &hnd, RBAF_WRITE | RBAF_OVERWRITE);
	BUG_ON(ret != 0);

	memset(hnd->data, 0, RPDFS_BLOCK_SIZE);
	rinode = hnd->data;

	rinode->ig.ino = cpu_to_le64(RPDFS_ROOT_INO);
	rinode->ig.gen = cpu_to_le64(RPDFS_ROOT_GEN);
	rinode->size = cpu_to_le64(5); /* name lens of . and .. with null term */
	rinode->version = cpu_to_le64(1);
	rinode->nlink = cpu_to_le32(2);
	rinode->mode = cpu_to_le32(S_IFDIR | 0755);
	rinode->atime_nsec = cpu_to_le64(ktime_get_real_ns());
	rinode->ctime_nsec = rinode->atime_nsec;
	rinode->mtime_nsec = rinode->atime_nsec;
	rinode->crtime_nsec = rinode->atime_nsec;
	ret = 0;

out:
	rpdfs_txn_reset(rfi, &txn);
	if (ret == 0)
		ret = rpdfs_block_flush(rfi, bnr, true);

	return ret;
}
