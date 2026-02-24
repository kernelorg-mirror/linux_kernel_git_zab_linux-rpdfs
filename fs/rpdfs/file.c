/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>

#include "file.h"
#include "inode.h"

const struct inode_operations rpdfs_file_iops = {
	.getattr	= rpdfs_getattr,
	.setattr	= rpdfs_setattr,
};

const struct file_operations rpdfs_file_fops = {
};
