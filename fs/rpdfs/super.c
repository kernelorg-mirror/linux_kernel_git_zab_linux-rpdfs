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
#include "rpdfs_trace.h"
#include "xattr.h"

struct rpdfs_fs_context {
	/* XXX arbitrary limit on number of addrs */
	struct rpdfs_net_transport_addr devd_addrs[16];
	size_t nr_addrs;
	bool mkfs;
};

static int rpdfs_sync_fs(struct super_block *sb, int wait)
{
	struct rpdfs_fs_info *rfi = RPDFS_SB_FS(sb);

	rpdfs_prd("wait %d", wait);

	rpdfs_block_sync(rfi, wait);

	return 0;
}

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
	.sync_fs	= rpdfs_sync_fs,
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
	rpdfs_preq_destroy(rfi);
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
	      rpdfs_balloc_setup(rfi) ?:
	      rpdfs_preq_setup(rfi);
	if (ret < 0)
		rpdfs_destroy(rfi);

	return ret;
}

static int add_devd_addrs(struct rpdfs_fs_info *rfi, struct rpdfs_fs_context *rfc)
{
	size_t i;
	int ret;

	for (i = 0; i < rfc->nr_addrs; i++) {
		ret = rpdfs_map_add_addr(rfi, &rfc->devd_addrs[i]);
		if (ret < 0)
			break;
	}

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

	/* generate as early as possible for tracing */
	generate_random_uuid(rfi->client_uuid);

	fc->s_fs_info = rfi;
	sb = sget_fc(fc, NULL, rpdfs_set_super);
	fc->s_fs_info = NULL;
	if (IS_ERR(sb)) {
		ret = PTR_ERR(sb);
		goto out;
	}

	/* use the uuid as a little fingerprint for now, might prefer client qlist id */
	snprintf(sb->s_id, sizeof(sb->s_id), RFI_TRACE_TPF, RFI_TRACE_ID(rfi));

	/*
	 * XXX We hack together an analog of a map with the devd
	 * addresses instead of connecting to a quorumd to get devd
	 * addresses from the devd qlist.
	 */
	ret = rpdfs_setup(rfi) ?:
	      add_devd_addrs(rfi, rfc) ?:
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
	Opt_devd_addr,
	Opt_mkfs,
	Opt_source,
};

static const struct fs_parameter_spec rpdfs_mount_parameters[] = {
	fsparam_string	("devd_addr",			Opt_devd_addr),
	fsparam_flag	("mkfs",			Opt_mkfs),
	fsparam_string	("source",			Opt_source),
	{}
};

static int parse_devd_addr(struct fs_context *fc, struct rpdfs_fs_context *rfc, char *str)
{
	int ret;

	if (rfc->nr_addrs >= ARRAY_SIZE(rfc->devd_addrs))
		return invalfc(fc, "too many addresses given, %zu supported.",
			       ARRAY_SIZE(rfc->devd_addrs));

	ret = rpdfs_parse_ipv4(str, &rfc->devd_addrs[rfc->nr_addrs]._sin);
	if (ret < 0) {
		if (ret == -EINVAL)
			return invalfc(fc, "Invalid ipv4 addr:port");
		else
			return invalfc(fc, "Error parsing ipv4 addr:port: %d", ret);
	}

	rfc->nr_addrs++;
	return 0;
}

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
	case Opt_devd_addr:
		ret = parse_devd_addr(fc, rfc, param->string);
		break;
	case Opt_mkfs:
		rfc->mkfs = true;
		break;
	case Opt_source:
		ret = parse_devd_addr(fc, rfc, param->string);
		if (ret == 0) {
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
