/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/module.h>
#include <linux/magic.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/statfs.h>

#include "balloc.h"
#include "block.h"
#include "inode.h"
#include "map.h"
#include "mkfs.h"
#include "net.h"
#include "net_tcp.h"
#include "parse.h"
#include "pr.h"
#include "xattr.h"

struct rpdfs_fs_context {
	struct rpdfs_net_transport_addr source_addr;
	bool mkfs;
};

static int rpdfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	buf->f_type = RPDFS_SUPER_MAGIC;
	buf->f_bsize = RPDFS_BLOCK_SIZE;
	buf->f_namelen = RPDFS_NAME_MAX;

	return 0;
}

static const struct super_operations rpdfs_sop = {
	.alloc_inode	= rpdfs_alloc_inode,
	.free_inode	= rpdfs_free_inode,
	.statfs		= rpdfs_statfs,
	.write_inode	= rpdfs_write_inode,
};

static int rpdfs_set_super(struct super_block *sb, struct fs_context *fc)
{
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_xattr = rpdfs_xattr_handlers;
	sb->s_op = &rpdfs_sop;
	sb->s_time_gran = 1;
	sb->s_time_min = 0;
	sb->s_time_max = U64_MAX / NSEC_PER_SEC;
	sb->s_flags |= SB_NODIRATIME | SB_NOATIME;
	sb->s_magic = RPDFS_SUPER_MAGIC;

	return set_anon_super_fc(sb, fc);
}

static void rpdfs_destroy(struct rpdfs_fs_info *rfi)
{
	rpdfs_balloc_destroy(rfi);
	rpdfs_block_destroy(rfi);
	rpdfs_map_destroy(rfi);
	rpdfs_net_destroy(rfi);
}

static int rpdfs_setup(struct rpdfs_fs_info *rfi)
{
	int ret;

	ret = rpdfs_net_setup(rfi, &rpdfs_net_tcp_ops) ?:
	      rpdfs_map_setup(rfi) ?:
	      rpdfs_block_setup(rfi) ?:
	      rpdfs_balloc_setup(rfi);
	if (ret < 0)
		rpdfs_destroy(rfi);

	return ret;
}

static int rpdfs_get_tree(struct fs_context *fc)
{
	static struct rpdfs_ino_gen root_ig = {
		.ino = cpu_to_le64(RPDFS_ROOT_INO),
		.gen = cpu_to_le64(RPDFS_ROOT_GEN),
	};
	struct rpdfs_fs_context *rfc = fc->fs_private;
	struct rpdfs_fs_info *rfi = NULL;
	struct super_block *sb = NULL;
	struct inode *inode;
	int ret;

	if (!fc->source)
		return invalfc(fc, "No source");

	rfi = kmalloc(sizeof(struct rpdfs_fs_info), GFP_KERNEL);
	if (!rfi) {
		ret = -ENOMEM;
		goto out;
	}

	fc->s_fs_info = rfi;
	sb = sget_fc(fc, NULL, rpdfs_set_super);
	fc->s_fs_info = NULL;
	if (IS_ERR(sb)) {
		ret = PTR_ERR(sb);
		goto out;
	}

	/*
	 * XXX We hack together an analog of a map with a single devd
	 * address instead of connecting to mapd to get the real maps.
	 */
	ret = rpdfs_setup(rfi) ?:
	      rpdfs_map_add_addr(rfi, &rfc->source_addr) ?:
	      rpdfs_map_connect(rfi);
	if (ret < 0)
		goto out;

	if (rfc->mkfs) {
		ret = rpdfs_mkfs(rfi);
		if (ret < 0)
			goto out;
	}

	inode = rpdfs_iget(sb, &root_ig);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto out;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out;
	}

	sb->s_flags |= SB_ACTIVE;
        fc->root = dget(sb->s_root);
	ret = 0;
out:
	if (ret < 0) {
		if (!IS_ERR(sb))
			deactivate_locked_super(sb);
		kfree(rfi);
	}

	return ret;
}

enum {
	Opt_mkfs,
	Opt_source,
};

static const struct fs_parameter_spec rpdfs_mount_parameters[] = {
	fsparam_flag	("mkfs",			Opt_mkfs),
	fsparam_string	("source",			Opt_source),
	{}
};

static int rpdfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct rpdfs_fs_context *rfc = fc->fs_private;
	struct fs_parse_result result;
	int token;
	int ret;

	token = fs_parse(fc, rpdfs_mount_parameters, param, &result);
	if (token < 0)
		return token;

	switch (token) {
	case Opt_mkfs:
		rfc->mkfs = true;
		break;
	case Opt_source:
		if (rfc->source_addr.sa.sa_family != AF_UNSPEC)
			return invalfc(fc, "Multiple sources specified");
		ret = rpdfs_parse_ipv4(param->string, &rfc->source_addr._sin);
		if (ret == -EINVAL)
			return invalfc(fc, "Invalid ipv4 addr:port");
		else if (ret == 0) {
			fc->source = param->string;
			param->string = NULL;
		}
		break;
	default:
		return invalfc(fc, "Unknown option token '%d'", token);
	}

	return ret;
}

static void rpdfs_free_fc(struct fs_context *fc)
{
	struct rpdfs_fs_context *rfc = fc->fs_private;

	kfree(rfc);
}

static const struct fs_context_operations rpdfs_context_ops = {
	.parse_param	= rpdfs_parse_param,
	.get_tree	= rpdfs_get_tree,
	.free		= rpdfs_free_fc,
};

static int rpdfs_init_fs_context(struct fs_context *fc)
{
	struct rpdfs_fs_context *rfc;
	int ret;

	rfc = kzalloc(sizeof(struct rpdfs_fs_context), GFP_USER);
	if (!rfc) {
		ret = -ENOMEM;
		goto out;
	}

	rfc->source_addr.sa.sa_family = AF_UNSPEC;

	fc->fs_private = rfc;
	fc->ops = &rpdfs_context_ops;
	ret = 0;
out:
	return ret;
}

static void rpdfs_kill_sb(struct super_block *sb)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);

	if (!rfi)
		return;

	{
		struct inode *inode;
		spin_lock(&sb->s_inode_list_lock);
		list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
			rpdfs_prd("inode %p ri %p ino %llu i_count %d dent empt %u",
				  inode, RPDFS_I(inode), rpdfs_inode_ino(inode),
				  atomic_read(&inode->i_count), hlist_empty(&inode->i_dentry));

		}
		spin_unlock(&sb->s_inode_list_lock);
	}

	kill_anon_super(sb);
	rpdfs_destroy(rfi);
}

static struct file_system_type rpdfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "rpdfs",
	.init_fs_context	= rpdfs_init_fs_context,
	.kill_sb		= rpdfs_kill_sb,
};
MODULE_ALIAS_FS("rpdfs");

/*
 * All the submodule exits in reverse order, except not unregistering
 * the fs so that that's only done in the main module _exit and not when
 * tearing down inits on error.  Our _exits are always safe to call if
 * their _init wasn't called.
 */
static void rpdfs_exits(void)
{
	rpdfs_inode_exit();
}

static int __init rpdfs_init(void)
{
	int ret;

	ret = rpdfs_inode_init() ?:
	      register_filesystem(&rpdfs_fs_type);
	if (ret < 0)
		rpdfs_exits();

	return ret;
}

static void __exit rpdfs_exit(void)
{
	rpdfs_exits();
	unregister_filesystem(&rpdfs_fs_type);
}

MODULE_AUTHOR("Zach Brown");
MODULE_DESCRIPTION("RapidFS File System (rpdfs)");
MODULE_LICENSE("GPL");
module_init(rpdfs_init)
module_exit(rpdfs_exit)
