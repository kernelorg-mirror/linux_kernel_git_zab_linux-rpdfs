/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>

#include "file.h"
#include "inode.h"
#include "pr.h"

static int rpdfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret;
	int err;

	ret = file_write_and_wait_range(file, start, end);

	if (!datasync) {
		err = file_check_and_advance_wb_err(file);
		if (err < 0)
			ret = err;
	}

	rpdfs_prd("start %llu end %llu ds %d ret %d", (u64)start, (u64)end, datasync, ret);

	return ret;
}

/*
 * We'll want to cut up large writes into multiple checkpoints each with
 * an update of the inode.  For now we just update at the end of the
 * whole write.
 */
static ssize_t rpdfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct rpdfs_fs_info *rfi = RPDFS_INODE_FS(inode);
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	rpdfs_inode_update(rfi, inode);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

const struct inode_operations rpdfs_file_iops = {
	.getattr	= rpdfs_getattr,
	.setattr	= rpdfs_setattr,
};

const struct file_operations rpdfs_file_fops = {
	.read_iter	= generic_file_read_iter,
	.write_iter     = rpdfs_file_write_iter,
	.fsync		= rpdfs_fsync,
};
