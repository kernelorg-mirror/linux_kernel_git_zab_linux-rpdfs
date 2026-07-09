/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/crc64.h>
#include "aops.h"
#include "format-block.h"
#include "format-msg.h"
#include "inode.h"
#include "map.h"
#include "meta.h"
#include "pr.h"
#include "super.h"

void rpdfs_block_key_init(struct rpdfs_block_key *key, struct rpdfs_inode_nr *ino,
			  u8 type, u64 t_index)
{
	key->k[0] = ino->i[0];
	key->k[1] = ino->i[1];
	key->k[2] = cpu_to_le64((((u64)type) << RPDFS_BLOCK_KEY_TYPE__SHIFT) |
				    (t_index & RPDFS_BLOCK_KEY_INDEX__MASK));
}

/*
 * Find the folio that's pinned by the pending read/write so the caller
 * can complete the IO.
 *
 * We can get to the folio reasonably efficiently by only using rcu
 * lookups in the inode cache and the inode's address space's xas.
 * filemap_find_entry() isn't exported and we "know" that
 * __filemap_get_folio() only calls it with the right flags.  *fingers
 * crossed*.
 *
 * We don't want to perturn dropbehind so we pass in DONTCACHE.
 */
static struct folio *get_block_key_folio(struct rpdfs_fs_info *rfi, struct rpdfs_block_key *key)
{
	struct super_block *sb = rfi->sb;
	struct inode *mapping = NULL;
	struct rpdfs_iget_data igd;
	struct inode *inode;
	struct folio *folio;
	pgoff_t index;
	int ret;

	ret = rpdfs_folio_loc_from_block_key(&igd, &index, key);
	if (ret < 0) {
		folio = ERR_PTR(ret);
		goto out;
	}

	rcu_read_lock();
	inode = rpdfs_find_inode_rcu(sb, &igd);
	if (inode)
		folio = __filemap_get_folio(inode->i_mapping, index, FGP_DONTCACHE, 0);
	else
		folio = ERR_PTR(-ENOENT);
	rcu_read_unlock();

	rpdfs_prd("key "RBKF" mapping %p folio %p index %ld",
		  RBKA(key), mapping, folio, !IS_ERR(folio) ? folio->index : -1L);
out:
	return folio;
}

static int send_block_read(struct rpdfs_fs_info *rfi, struct folio *folio, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_block_read rd;
	struct rpdfs_net_message_desc md = {
		.type = RPDFS_MSG_BLOCK_READ,
		.ctl_buf = &rd,
		.ctl_size = sizeof(rd),
	};
	u64 mver;
	int ret;

	rpdfs_block_key_from_folio(&rd.key, folio);

	ret = rpdfs_map_hash_rv_to_addr(rfi, &rd.key, sizeof(rd.key), &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

static int recv_block_read_result(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_msg_block_read_result *rr = md->ctl_buf;
	struct folio *folio;
	bool success = false;
	u64 crc;
	int ret;

	rpdfs_prd("bk "RBKF" err %u", RBKA(&rr->key), rr->err);

	BUILD_BUG_ON(RPDFS_BLOCK_SIZE != PAGE_SIZE);

	folio = get_block_key_folio(rfi, &rr->key);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	if (!rr->err && md->data_page) {
		crc = crc64_nvme(0, page_address(md->data_page), RPDFS_BLOCK_SIZE);
		if (crc != le64_to_cpu(rr->det.crc)) {
			rpdfs_err("crc failed, calculated %016llx != transmitted %016llx",
				  crc, le64_to_cpu(rr->det.crc));
		} else {
			memcpy_to_folio(folio, 0, page_address(md->data_page), RPDFS_BLOCK_SIZE);
			success = true;
		}
	}
	folio_end_read(folio, success);
	folio_put(folio);

	ret = 0;
out:
	return ret;
}

static int send_block_write(struct rpdfs_fs_info *rfi, struct folio *folio, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_block_write wr;
	struct rpdfs_net_message_desc md = {
		.type = RPDFS_MSG_BLOCK_WRITE,
		.ctl_buf = &wr,
		.ctl_size = sizeof(wr),
		.data_page = folio_page(folio, 0),
		.data_size = RPDFS_BLOCK_SIZE,
	};
	u64 mver;
	u64 crc;
	int ret;

	crc = crc64_nvme(0, folio_address(folio), RPDFS_BLOCK_SIZE);

	rpdfs_block_key_from_folio(&wr.key, folio);
	wr.det.crc = cpu_to_le64(crc);

	rpdfs_prd("key "RBKF" "RFF, RBKA(&wr.key), RFA(folio));

	ret = rpdfs_map_hash_rv_to_addr(rfi, &wr.key, sizeof(wr.key), &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

static int recv_block_write_result(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_msg_block_write_result *wr = md->ctl_buf;
	struct folio *folio;
	int ret;

	folio = get_block_key_folio(rfi, &wr->key);
	if (IS_ERR(folio)) {
		rpdfs_prd("key "RBKF" err %u ret %d", RBKA(&wr->key), wr->err, ret);
		ret = PTR_ERR(folio);
		goto out;
	}

	rpdfs_prd("key "RBKF" err %u "RFF, RBKA(&wr->key), wr->err, RFA(folio));

	BUG_ON(!folio_test_writeback(folio));

	folio_end_writeback(folio);
	folio_put(folio);

	ret = 0;
out:
	return ret;
}

static int rpdfs_read_folio(struct file *file, struct folio *folio)
{
	struct rpdfs_fs_info *rfi = RPDFS_FOLIO_FS(folio);
	int ret;

	ret = send_block_read(rfi, folio, GFP_NOFS);
	if (ret < 0)
		folio_end_read(folio, false);
	return ret;
}

/*
 * The read_pages() caller does exactly this so I'm not sure that we
 * need a .readahead method.
 */
static void rpdfs_readahead(struct readahead_control *rac)
{
	struct folio *folio;

	while ((folio = readahead_folio(rac)))
		rpdfs_read_folio(rac->file, folio);
}

static int write_one_folio(struct rpdfs_fs_info *rfi, struct address_space *mapping,
			   struct folio *folio)
{
	int ret;

	BUG_ON(!folio_test_uptodate(folio));
	BUG_ON(folio_test_writeback(folio));

	folio_start_writeback(folio);

	ret = send_block_write(rfi, folio, GFP_NOFS);
	if (ret < 0) {
		mapping_set_error(mapping, -EIO);
		folio_end_writeback(folio);
	}

	folio_unlock(folio);

	return ret;
}

static int rpdfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	struct rpdfs_fs_info *rfi = RPDFS_MAPPING_FS(mapping);
	struct folio *folio = NULL;
	int ret;

	while ((folio = writeback_iter(mapping, wbc, folio, &ret)))
		ret = write_one_folio(rfi, mapping, folio);

	return ret;
}

static int rpdfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos,
			     unsigned len, struct folio **foliop, void **fsdata)
{
	struct folio *folio;
	int ret;

	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_WRITEBEGIN,
				    mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out;
	}

	folio_mark_uptodate(folio);

	/* XXX need reads around partial folio writes here */

	*foliop = folio;
	ret = 0;
out:
	return ret;
}

static int rpdfs_write_end(struct file *file, struct address_space *mapping, loff_t pos,
			   unsigned len, unsigned copied, struct folio *folio, void *fsdata)
{
	struct inode *inode = folio_inode(folio);
	loff_t old_size = inode->i_size;

	rpdfs_prd("ino "RIF" sz %llu pos %llu len %u copied %u", RIA(inode), (u64)old_size,
		  (u64)pos, len, copied);

	if (unlikely(copied < len))
		folio_zero_segment(folio, pos + copied, pos + len);
	flush_dcache_folio(folio);

	folio_mark_uptodate(folio);
	filemap_dirty_folio(mapping, folio);

	if (pos + copied > inode->i_size)
		i_size_write(inode, pos + copied);

	folio_unlock(folio);
	folio_put(folio);

	if (old_size < pos)
		pagecache_isize_extended(inode, old_size, pos);

	return copied;
}

const struct address_space_operations rpdfs_aops = {
	.read_folio		= rpdfs_read_folio,
	.writepages		= rpdfs_writepages,
	.readahead		= rpdfs_readahead,
	.write_begin            = rpdfs_write_begin,
	.write_end              = rpdfs_write_end,
};

int rpdfs_aops_setup(struct rpdfs_fs_info *rfi)
{
	int ret;

	ret = rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_READ_RESULT, recv_block_read_result) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_WRITE_RESULT, recv_block_write_result);
	if (ret < 0)
		rpdfs_aops_destroy(rfi);
	return ret;
}

void rpdfs_aops_destroy(struct rpdfs_fs_info *rfi)
{
	rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_READ_RESULT, recv_block_read_result);
	rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_WRITE_RESULT, recv_block_write_result);
}
