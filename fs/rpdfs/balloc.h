/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BALLOC_H
#define RPDFS_BALLOC_H

#include <linux/types.h>

struct rpdfs_transaction;

#include "format-msg.h"
#include "txn.h"

/*
 * A record of blocks that were free and granted write mode when the
 * block cache received free stripe grants.
 *
 * By the time we try to allocate them they might have been allocated
 * elsewhere or revoked.
 */
struct rpdfs_balloc_region {
	struct list_head head;
	u64 base_bnr;
	unsigned long size;
	unsigned long first_set;
	unsigned long nr_set;
	unsigned long bits[];
};

struct rpdfs_balloc_region *rpdfs_balloc_alloc_region(u64 base_bnr, unsigned long nr_blocks);
void rpdfs_balloc_free_region(struct rpdfs_balloc_region *reg);
void rpdfs_balloc_set_stripe_bits(struct rpdfs_balloc_region *reg, unsigned long this_stripe,
				  unsigned long stripes, __le64 *bmap, unsigned long size);
void rpdfs_balloc_publish_region(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg);

int rpdfs_balloc_alloc_bnr(struct rpdfs_balloc_region *reg, u64 *bnr_ret);

struct rpdfs_balloc_region *rpdfs_balloc_take_region(struct rpdfs_fs_info *rfi);
void rpdfs_balloc_return_region(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg);

int rpdfs_balloc_free_meta(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr);

int rpdfs_balloc_setup(struct rpdfs_fs_info *rfi);
void rpdfs_balloc_destroy(struct rpdfs_fs_info *rfi);

#endif
