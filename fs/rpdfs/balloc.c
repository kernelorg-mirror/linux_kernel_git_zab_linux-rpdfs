/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/bitmap.h>

#include "format-block.h"
#include "balloc.h"
#include "block.h"
#include "pr.h"
#include "rpdfs_trace.h"
#include "seqlock.h"
#include "super.h"
#include "txn.h"

#define REGF		"reg bnr %llu nr_set %lu"
#define REGA(reg)	(reg)->base_bnr, (reg)->nr_set

struct rpdfs_balloc_info {
	struct rpdfs_balloc_region * __percpu *pcpu_region;
	struct wait_queue_head waitq;
	seqlock_t seqlock;
	struct list_head region_list;
	atomic_t enospc_count;
};

static inline struct rpdfs_balloc_info *RPDFS_BALINF(struct rpdfs_fs_info *rfi)
{
	return rfi->balloc_info;
}

static inline void SET_RPDFS_BALINF(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_info *balinf)
{
	rfi->balloc_info = balinf;
}

struct rpdfs_balloc_region *rpdfs_balloc_alloc_region(u64 base_bnr, unsigned long nr_blocks)
{
	struct rpdfs_balloc_region *reg;

	reg = kvmalloc(offsetof(struct rpdfs_balloc_region,
				bits[DIV_ROUND_UP(nr_blocks, BITS_PER_LONG)]), GFP_NOFS);
	if (!reg) {
		reg = ERR_PTR(-ENOMEM);
		goto out;
	}

	INIT_LIST_HEAD(&reg->head);
	reg->base_bnr = base_bnr;
	reg->size = nr_blocks;
	reg->first_set = reg->size;
	reg->nr_set = 0;
	bitmap_zero(reg->bits, reg->size);
	rpdfs_prd(REGF, REGA(reg));
out:
	return reg;
}

void rpdfs_balloc_free_region(struct rpdfs_balloc_region *reg)
{
	if (!IS_ERR_OR_NULL(reg)) {
		rpdfs_prd(REGF, REGA(reg));
		kvfree(reg);
	}
}

void rpdfs_balloc_set_stripe_bits(struct rpdfs_balloc_region *reg, unsigned long this_stripe,
				  unsigned long stripes, __le64 *bmap, unsigned long size)
{
	unsigned long b;
	unsigned long r;

	for (b = 0; (b = find_next_bit_le(bmap, size, b)) < size; b++) {
		r = this_stripe + (b * stripes);
		set_bit(r, reg->bits);
		if (r < reg->first_set)
			reg->first_set = r;
		reg->nr_set++;
	}
}

int rpdfs_balloc_alloc_bnr(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg,
			   u64 *bnr_ret)
{
	unsigned long b;
	int ret;

	b = find_next_bit(reg->bits, reg->size, reg->first_set);
	if (b >= reg->size) {
		ret = -ENOSPC;
	} else {
		clear_bit(b, reg->bits);
		reg->first_set = b + 1;
		reg->nr_set--;

		*bnr_ret = reg->base_bnr + b;
		ret = 0;
	}

	trace_rpdfs_alloc_bnr(rfi, reg, *bnr_ret, ret);
	return ret;
}

/*
 * The block cache has completed a wave of requests for free stripes in
 * a region.  We either put a populated region on the list or record the
 * arrival of an empty region as an instance of enospc to be consumed.
 */
void rpdfs_balloc_publish_region(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg)
{
	struct rpdfs_balloc_info *balinf = RPDFS_BALINF(rfi);

	rpdfs_prd(REGF, REGA(reg));

	if (reg->nr_set == 0) {
		rpdfs_balloc_free_region(reg);
		atomic_inc(&balinf->enospc_count);
	} else {
		write_seqlock(&balinf->seqlock);
		list_add_tail(&reg->head, &balinf->region_list);
		write_sequnlock(&balinf->seqlock);
	}
	wake_up(&balinf->waitq);
}

static bool ready_or_resend(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_info *balinf, u64 until)
{
	bool empty;

	while_read_seqretry(&balinf->seqlock)
		empty = list_empty(&balinf->region_list);
	if (!empty || atomic_read(&balinf->enospc_count) > 0)
		return true;

	return rpdfs_block_should_request_free(rfi, until);
}

/*
 * Get exclusive access to a region of probably free blocks.  Typically
 * a busy task will be handing a region to and from its per-cpu region.
 *
 * When we don't have any prepared we ask the block cache to assemble a
 * new free region by requesting free stripes from the devds.
 *
 * ENOSPC isn't handled particularly gracefully.  Each arrival of an
 * empty region causes one waiting task, who has already sent their
 * requests, to return enospc.
 *
 * The only attempt at fairness is in having exclusive waits for the
 * list to be populated.  The longest waiter is woken first.  We could
 * use wake functions to stop new arrivals from removing from the list
 * early.
 */
struct rpdfs_balloc_region *rpdfs_balloc_take_region(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_balloc_info *balinf = RPDFS_BALINF(rfi);
	struct rpdfs_balloc_region **preg;
	struct rpdfs_balloc_region *reg;
	u64 until = 0;
	bool empty;
	int ret;

	for (;;) {
		reg = NULL;
		preg = get_cpu_ptr(balinf->pcpu_region);
		if (*preg)
			swap(reg, *preg);
		put_cpu_ptr(balinf->pcpu_region);
		if (reg)
			break;

		while_read_seqretry(&balinf->seqlock)
			empty = list_empty(&balinf->region_list);
		if (!empty) {
			write_seqlock(&balinf->seqlock);
			reg = list_first_entry_or_null(&balinf->region_list,
						       struct rpdfs_balloc_region, head);
			if (reg)
				list_del_init(&reg->head);
			write_sequnlock(&balinf->seqlock);
		}
		if (reg)
			break;

		if (rpdfs_block_should_request_free(rfi, until)) {
			if (until > 0 && atomic_dec_if_positive(&balinf->enospc_count) >= 0) {
				reg = ERR_PTR(-ENOSPC);
				break;
			}

			ret = rpdfs_block_request_free(rfi, &until);
			if (ret < 0) {
				reg = ERR_PTR(ret);
				break;
			}
		}

		ret = wait_event_interruptible_exclusive(balinf->waitq,
							 ready_or_resend(rfi, balinf, until));
		if (ret < 0) {
			reg = ERR_PTR(ret);
			break;
		}
	}

	if (!IS_ERR_OR_NULL(reg))
		rpdfs_prd(REGF, REGA(reg));

	return reg;
}

void rpdfs_balloc_return_region(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg)
{
	struct rpdfs_balloc_info *balinf = RPDFS_BALINF(rfi);
	struct rpdfs_balloc_region **preg;

	if (IS_ERR_OR_NULL(reg))
		return;

	rpdfs_prd(REGF, REGA(reg));

	if (reg->nr_set == 0) {
		rpdfs_balloc_free_region(reg);
		return;
	}

	preg = get_cpu_ptr(balinf->pcpu_region);
	if (*preg == NULL) {
		*preg = reg;
		reg = NULL;
	}
	put_cpu_ptr(balinf->pcpu_region);

	if (reg) {
		write_seqlock(&balinf->seqlock);
		list_add(&reg->head, &balinf->region_list);
		write_sequnlock(&balinf->seqlock);
		wake_up(&balinf->waitq);
	}
}

int rpdfs_balloc_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_balloc_info *balinf;
	int ret;

	balinf = kzalloc(sizeof(struct rpdfs_balloc_info), GFP_KERNEL);
	if (balinf)
		balinf->pcpu_region = alloc_percpu(struct rpdfs_balloc_region *);
	if (!balinf || !balinf->pcpu_region) {
		kfree(balinf);
		ret = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&balinf->waitq);
	seqlock_init(&balinf->seqlock);
	INIT_LIST_HEAD(&balinf->region_list);
	atomic_set(&balinf->enospc_count, 0);

	SET_RPDFS_BALINF(rfi, balinf);
	ret = 0;
out:
	return ret;
}

void rpdfs_balloc_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_balloc_info *balinf = RPDFS_BALINF(rfi);
	struct rpdfs_balloc_region *reg;
	struct rpdfs_balloc_region *_reg_;
	int cpu;

	if (balinf) {
		for_each_possible_cpu(cpu) {
			reg = *per_cpu_ptr(balinf->pcpu_region, cpu);
			rpdfs_balloc_free_region(reg);
		}

		list_for_each_entry_safe(reg, _reg_, &balinf->region_list, head) {
			list_del_init(&reg->head);
			rpdfs_balloc_free_region(reg);
		}

		free_percpu(balinf->pcpu_region);
		kfree(balinf);
		SET_RPDFS_BALINF(rfi, NULL);
	}
}
