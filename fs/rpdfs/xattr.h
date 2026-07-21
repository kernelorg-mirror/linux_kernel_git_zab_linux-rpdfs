/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_XATTR_H
#define RPDFS_XATTR_H

ssize_t rpdfs_listxattr(struct dentry *dentry, char *buf, size_t size);

extern const struct xattr_handler * const rpdfs_xattr_handlers[];

#endif
