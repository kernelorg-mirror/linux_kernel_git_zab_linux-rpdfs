/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/rhashtable.h>
#include <linux/list_sort.h>

#include "balloc.h"
#include "block.h"
#include "compare.h"
#include "format-block.h"
#include "format-msg.h"
#include "ht.h"
#include "lists.h"
#include "map.h"
#include "net.h"
#include "place.h"
#include "pr.h"
#include "rht.h"
#include "rpdfs_trace.h"
#include "seqlock.h"

/*
 * This block cache sits between callers who use cached block contents
 * and the network protocol that provides cache coherency and performs
 * remote block IO.
 *
 * Each cached block has a cache mode that is granted by the network
 * protocol.  For example, a block is initially populated by requesting
 * a cached block with read mode.  Once granted local users can read the
 * contents of the block.  If a user needs to write the block, we need
 * to request that additional mode from the server.  Once it responds
 * local writers can modify blocks.
 *
 * Local shared read or exclusive write handles to blocks are mutually
 * exclusive.  Handle acquisition attempts will block until incompatible
 * handles are released.  In this way, handles behave like locks.  But
 * they layer on the additional requirement that an acquisition has to
 * be allowed by the mode granted by the server.
 *
 * The granted cache mode remains while there are no active handles.
 * From the network protocol's perspective, the cache mode is a level of
 * access that is granted to the client.  Local read handles are allowed
 * while the client is granted write access.  Only received revocations
 * can decrease the granted mode, and we won't send a confirmation to
 * the revocation until local handles that were relying on the mode have
 * been released.
 *
 * Dirty blocks are maintained in multiple dirty lists.  Blocks are
 * assigned an increasing sequence in each list as they're dirtied and
 * added to its tail.  Each block stores a boundary which records the
 * farthest position in all the dirty lists which must be written
 * atomically with the block.
 *
 * This design intends to provide just enough isolation between
 * independent dirtying work such that each stream can be flushed
 * independently.  Perhaps more critically revoked blocks only need to
 * flush the blocks from their independent work.  Contention on dirty
 * blocks won't necessarily flush all dirty data.
 */

struct rpdfs_block_info {
	struct rpdfs_fs_info *rfi;
	struct rhashtable block_ht;
	struct shrinker *shrinker;
	struct list_lru lru;
	struct workqueue_struct *workq;
	struct rpdfs_flusher *flshr;
	struct rpdfs_dirty_list *dirty_lists;

	atomic64_t regions_started;
	atomic64_t regions_done;
	struct rhashtable rbld_ht;
};

static struct rpdfs_block_info *RPDFS_BINF(struct rpdfs_fs_info *rfi)
{
	return rfi->block_info;
}

static void SET_RPDFS_BINF(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf)
{
	rfi->block_info = binf;
}

#define NR_DIRTY_LISTS	8
#define NR_DIST_WRITES	16

struct rpdfs_dirty_list {
	seqlock_t seqlock;
	struct list_head list;
	unsigned long count;
	u64 dirty_seq;
} ____cacheline_aligned_in_smp;

/*
 * A boundary records a position in each dirty list.  As blocks are
 * modified in transactions they each have a boundary that is extended
 * to cover all the blocks in the transaction.  When we flush a block we
 * also flush the blocks in the lists up to its boundary.
 */
struct rpdfs_dirty_boundary {
	u64 seq[NR_DIRTY_LISTS];
};

/*
 * The flusher finds the blocks in the dirty lists that are within a
 * boundary to flush and sends them off as distributed writes.
 */
struct rpdfs_flusher {
	struct rpdfs_block_info *binf;
	wait_queue_head_t waitq;
	struct work_struct work;
	seqlock_t seqlock;
	struct list_head idle_writes;
	struct list_head busy_writes;
	struct list_head dirty_list[NR_DIRTY_LISTS];
	struct rpdfs_dirty_boundary requested_bnd;
	struct rpdfs_dirty_boundary writing_bnd;
	struct rpdfs_dirty_boundary complete_bnd;
};

struct rpdfs_dist_write {
	struct rpdfs_block_info *binf;
	struct list_head head;
	struct work_struct work;
	atomic_t sent_writes;
	unsigned short nr_blocks;
	unsigned short completing:1;
	struct list_head dirty_list;
	struct rpdfs_dirty_boundary bnd;
};

/*
 * @request_mode is the mode that we've sent a request for, either with
 * a read or request_mode message.
 *
 * @grant_mode is the mode that new references must be compatible with.
 * It's set by the server as we receive read_result, grant_mode, or
 * free_stripe_grant messages.  It's decreased after receiving a
 * revocation, users drain, and we send a confirmation.
 *
 * @confirm_mode is the record of a confirm that we must send once
 * current users are compatible with the confirm mode.  It is set as we
 * receive a revoke_mode from the server, and cleared as incompatible
 * use finishes: releasing references or write completion.
 *
 * @write_confirm_mode is the record that a sent write message contains
 * a confirm_mode.  We don't know if it's been processed until we get a
 * write_result.
 */
struct rpdfs_block {
	struct rcu_head rcu;
	struct rhash_head rhead;
	struct list_head lru_head;
	struct rpdfs_block_handle hnd;
	atomic64_t refcount;
	seqlock_t seqlock;
	wait_queue_head_t waitq;
	struct page *data_page;
	u64 dirty_seq;
	struct list_head dirty_head;
	struct rpdfs_dirty_boundary dirty_bnd;
	struct rpdfs_dist_write *wri;
	unsigned long bits;
	unsigned long readers;
	unsigned long dirty:1,
		      upd_meta:1,
		      upd_data:1,
		      writer:1;
	unsigned int dirty_list_nr;
	short error;
	/* cache modes tracking protocol exchanges with server */
	u8 request_mode;
	u8 grant_mode;
	u8 confirm_mode;
	u8 write_confirm_mode;
};

/*
 * While blocks are in the hash table and lru they have a huge value
 * added to the refcount.  Lookup returns new references to blocks as
 * long as this value is here.  The exclusive right to remove from the
 * cache is only allowed by atomically removing this value and adding a
 * normal refcount.
 */
#define REMOVAL_REFCOUNT (S64_MAX ^ (S64_MAX >> 1))

enum {
	RPDFS_BLOCK_BIT_ACCESSED = 0,
};

static void fill_params(struct rpdfs_trace_block_params *p, struct rpdfs_block *bk)
{
	p->place = bk->hnd.place;
	p->bnr = bk->hnd.bnr;
	p->alloc_ctr = bk->hnd.alloc_ctr;
	p->write_ctr = bk->hnd.wcount;
	p->refcount = atomic64_read(&(bk)->refcount) & ~REMOVAL_REFCOUNT;
	p->dirty_seq = bk->dirty_seq;
	p->flags = ((atomic64_read(&(bk)->refcount) & REMOVAL_REFCOUNT) ?
			RPDFS_BLOCK_TRACE_FLAG_REMOVAL : 0) |
		   (test_bit(RPDFS_BLOCK_BIT_ACCESSED, &bk->bits) ?
			RPDFS_BLOCK_TRACE_FLAG_ACCESSED:0) |
		   (bk->dirty ? RPDFS_BLOCK_TRACE_FLAG_DIRTY : 0) |
		   (bk->upd_meta ? RPDFS_BLOCK_TRACE_FLAG_UPD_META : 0) |
		   (bk->upd_data ? RPDFS_BLOCK_TRACE_FLAG_UPD_DATA : 0) |
		   (bk->writer ? RPDFS_BLOCK_TRACE_FLAG_WRITER : 0);
	p->dirty_list_nr = bk->dirty_list_nr;
	p->request = bk->request_mode;
	p->grant = bk->grant_mode;
	p->confirm = bk->confirm_mode;
	p->write_confirm = bk->write_confirm_mode;
}

/* call the tracepoint with the given args after translating bk fields into params */
#define _TBP(tp, rfi, bk) \
do { \
	struct rpdfs_trace_block_params p__; \
	tp((rfi), ({ fill_params(&p__, (bk)); &p__; })); \
} while (0)

#define bitfield_char(f, c) ((f) ? c : '-')
#define RBF \
	"bnr %llu ac %llu wc %llu plc "RPF" refc %llx rd %lu %c%c%c%c " \
	"rm %u gm %u cm %u wcm %u"
#define RBA(bk) \
	(bk)->hnd.bnr, (bk)->hnd.alloc_ctr, (bk)->hnd.wcount, RPA((bk)->hnd.place), \
	atomic64_read(&(bk)->refcount), (bk)->readers, bitfield_char(bk->dirty, 'd'), \
	bitfield_char(bk->upd_meta,  'M'), bitfield_char(bk->upd_data, 'D'), \
	bitfield_char(bk->writer,  'w'), (bk)->request_mode, (bk)->grant_mode, \
	(bk)->confirm_mode, (bk)->write_confirm_mode

static const struct rhashtable_params block_ht_params = {
	.key_len	= sizeof_field(struct rpdfs_block, hnd.bnr),
	.key_offset	= offsetof(struct rpdfs_block, hnd.bnr),
	.head_offset	= offsetof(struct rpdfs_block, rhead),
};

/*
 * These record the write grants that we've received for free stripes
 * that only contain block details, not contents.  We translate these
 * bits and details into full cached blocks as individual blocks are
 * used locally.
 */
struct rpdfs_block_region_builder {
	struct rpdfs_ht_entry hte;
	struct rpdfs_fs_info *rfi;
	u64 base_bnr;
	atomic_t in_flight;
	spinlock_t lock;
	struct rpdfs_balloc_region *reg;
};

static const struct rhashtable_params rbld_ht_params = {
	.key_len	= sizeof_field(struct rpdfs_block_region_builder, base_bnr),
	.key_offset	= offsetof(struct rpdfs_block_region_builder, base_bnr),
	.head_offset	= offsetof(struct rpdfs_block_region_builder, hte.rhead),
};

/*
 * We trigger a flush once a dirty list exceeds this block limit.  The
 * resulting distributed write can have this many blocks from each dirty
 * list, and a bit more for the size of the transaction that pushed the
 * count over the limit.
 */
#define DIRTY_BLOCK_LIMIT	128

static u8 mode_from_rbaf(rbaf_t rbaf)
{
	return (rbaf & RBAF_WRITE) ? RPDFS_CACHE_MODE_WRITE : RPDFS_CACHE_MODE_READ;
}

/*
 * return the mode that reflects the current reference users.
 */
static u8 readers_writer_mode(struct rpdfs_block *bk)
{
	if (bk->writer)
		return RPDFS_CACHE_MODE_WRITE;
	else if (bk->readers)
		return RPDFS_CACHE_MODE_READ;
	else
		return RPDFS_CACHE_MODE_NONE;
}

/*
 * Return the least compatible mode needed to be compatible with current
 * use of the cached block.  Includes held references and dirty modified
 * contents.
 */
static bool cache_using_mode(struct rpdfs_block *bk)
{
	if (bk->dirty)
		return RPDFS_CACHE_MODE_WRITE;
	else
		return readers_writer_mode(bk);
}

static bool block_is_none(struct rpdfs_block *bk)
{
	return bk->request_mode <= RPDFS_CACHE_MODE_NONE &&
	       bk->grant_mode <= RPDFS_CACHE_MODE_NONE &&
	       bk->confirm_mode <= RPDFS_CACHE_MODE_NONE;
}

static bool alloc_ctr_is_free(u64 alloc_ctr)
{
	return (alloc_ctr & 1) == 0;
}

/*
 * Returns true if references of the two modes are compatible.  NULL and
 * NONE are compatible with everything.  The only incompatible
 * combinations are a write paired with a read or write.
 */
static bool modes_compatible(u8 low, u8 high)
{
	if (low > high)
		swap(low, high);

	return high < RPDFS_CACHE_MODE_WRITE || low < RPDFS_CACHE_MODE_READ;
}

/*
 * Make sure that the block has a data page assigned.  If it doesn't,
 * allocate one.
 */
static int alloc_data_page(struct rpdfs_block *bk, gfp_t gfp)
{
	struct page *page;

	while_read_seqretry(&bk->seqlock)
		page = bk->data_page;
	if (page)
		return 0;

	page = alloc_page(gfp);
	if (!page)
		return -ENOMEM;

	write_seqlock(&bk->seqlock);
	if (!bk->data_page) {
		bk->data_page = page;
		bk->hnd.data = page_address(bk->data_page);
		page = NULL;
	}
	write_sequnlock(&bk->seqlock);

	if (page)
		put_page(page);

	return 0;
}

static struct rpdfs_block *alloc_block(bool with_page, gfp_t gfp)
{
	struct rpdfs_block *bk;
	int ret;

	bk = kzalloc(sizeof(struct rpdfs_block), gfp);
	if (!bk) {
		bk = ERR_PTR(-ENOMEM);
		goto out;
	}

	INIT_LIST_HEAD(&bk->lru_head);
	atomic64_set(&bk->refcount, 0);
	seqlock_init(&bk->seqlock);
	init_waitqueue_head(&bk->waitq);
	INIT_LIST_HEAD(&bk->dirty_head);

	if (with_page) {
		ret = alloc_data_page(bk, gfp);
		if (ret < 0) {
			kfree(bk);
			bk = ERR_PTR(ret);
			goto out;
		}
	}
out:
	return bk;
}

static void free_block(struct rpdfs_block *bk)
{
	BUG_ON(!list_empty(&bk->lru_head));
	BUG_ON(atomic64_read(&bk->refcount) != 0);
	BUG_ON(waitqueue_active(&bk->waitq));
	BUG_ON(!list_empty(&bk->dirty_head));
	BUG_ON(bk->readers || bk->writer);

	/* rcu protects bk, not page.. that's only referenced with refcount */
	if (bk->data_page)
		put_page(bk->data_page);
	kfree_rcu(bk, rcu);
}


static void put_block(struct rpdfs_fs_info *rfi, struct rpdfs_block *bk)
{
	s64 now;

	if (!IS_ERR_OR_NULL(bk) && (now = atomic64_dec_return(&bk->refcount)) == 0) {
		_TBP(trace_rpdfs_block_put_freed, rfi, bk);
		free_block(bk);
	}

	WARN_ON_ONCE(now < 0);
	WARN_ON_ONCE(now == REMOVAL_REFCOUNT - 1);
}

/*
 * Increment the refcount while already holding one.
 */
static void get_block(struct rpdfs_block *bk)
{
	s64 now = atomic64_inc_return(&bk->refcount);

	WARN_ON_ONCE(now <= 1);
	WARN_ON_ONCE(now == REMOVAL_REFCOUNT);
}

/*
 * It feels silly to have a bunch of boilerplate with different
 * operators testing conditions..  this seems better?
 */
#define atomic64_add_if(v_, op_rhs, a_) \
({ \
	__typeof__(v_) v__ = (v_); \
	__typeof__(a_) a__ = (a_); \
	bool added__; \
	s64 c__ = raw_atomic64_read(v__); \
	for (;;) { \
		if (!(c__ op_rhs)) { \
			added__ = false; \
			break; \
		} \
		if (raw_atomic64_try_cmpxchg(v__, &c__, c__ + a__)) { \
			added__ = true; \
			break; \
		} \
	} \
	added__; \
})

/*
 * Get a refcount on a block unless it's going to be freed after this
 * rcu read section.  Returns true if the refcount was incremented.
 */
static bool get_block_not_removed(struct rpdfs_block *bk)
{
	return atomic64_add_if(&bk->refcount, >= REMOVAL_REFCOUNT, 1);
}

/*
 * Give the caller the removal refcount, only if there are exactly the
 * number of additional existing refs as they expect.
 */
static bool get_block_removal(struct rpdfs_block *bk, u64 refs)
{
	return atomic64_add_if(&bk->refcount, == (REMOVAL_REFCOUNT + refs), 1 - REMOVAL_REFCOUNT);
}

/*
 * Called by normal refcount holders with the block locked, having just
 * set modes such that now all its modes night be none/null.  If the
 * block doesn't have any protocol messaging in flight, and we're the
 * only refcount, then get the removal ref and remove the block from the
 * hash and lru.
 */
static void try_remove_none(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
			    struct rpdfs_block *bk)
{
	if (block_is_none(bk) && get_block_removal(bk, 1)) {
		rhashtable_remove_fast(&binf->block_ht, &bk->rhead, block_ht_params);
		list_lru_del_obj(&binf->lru, &bk->lru_head);
		put_block(rfi, bk);
	}
}

static bool block_has_writer(struct rpdfs_block *bk)
{
	bool writer;

	while_read_seqretry(&bk->seqlock)
		writer = !!bk->writer;

	return writer;
}

/*
 * Returns the block with a refcount if found, only NULL otherwise.
 */
static struct rpdfs_block *lookup_block(struct rpdfs_block_info *binf, u64 bnr)
{
	struct rpdfs_block *bk;

	rcu_read_lock();
	bk = rhashtable_lookup_fast(&binf->block_ht, &bnr, block_ht_params);
	if (bk && !get_block_not_removed(bk))
		bk = NULL;
	rcu_read_unlock();
	if (bk)
		set_bit(RPDFS_BLOCK_BIT_ACCESSED, &bk->bits);

	return bk;
}

static void put_rbld(struct rpdfs_block_info *binf, struct rpdfs_block_region_builder *rbld)
{
	if (IS_ERR_OR_NULL(rbld))
		return;

	rcu_read_lock();
	if (rpdfs_ht_put(&binf->rbld_ht, &rbld->hte, rbld_ht_params,
			 atomic_read(&rbld->in_flight) == 0)) {
		atomic64_inc(&binf->regions_done);
		kfree_rcu(rbld, hte.rcu);
	}
	rcu_read_unlock();
}

static struct rpdfs_block_region_builder *get_rbld(struct rpdfs_fs_info *rfi,
						   struct rpdfs_block_info *binf,
						   u64 base_bnr, unsigned long stripes)
{
	struct rpdfs_block_region_builder *ins;
	struct rpdfs_ht_entry *hte;

	hte = rpdfs_ht_get(&binf->rbld_ht, &base_bnr, rbld_ht_params);
	if (hte)
		goto out;

	ins = kzalloc(sizeof(struct rpdfs_block_region_builder), GFP_NOFS);
	if (ins)
		ins->reg = rpdfs_balloc_alloc_region(base_bnr,
						     RPDFS_MSG_BLOCKS_PER_FREE_STRIPE * stripes);
	if (!ins || !ins->reg) {
		kfree(ins);
		hte = ERR_PTR(-ENOMEM);
		goto out;
	}

	ins->rfi = rfi;
	ins->base_bnr = base_bnr;
	atomic_set(&ins->in_flight, stripes);
	spin_lock_init(&ins->lock);

	hte = rpdfs_ht_insert(&binf->rbld_ht, &ins->hte, rbld_ht_params);
	if (hte != &ins->hte)
		put_rbld(binf, ins);

out:
	if (IS_ERR_OR_NULL(hte))
		return ERR_CAST(hte);
	else
		return container_of(hte, struct rpdfs_block_region_builder, hte);
}

/*
 * Returns the block with a refcount if found or allocated, or
 * ERR_PTR(-errno) on error.
 */
static struct rpdfs_block *lookup_or_alloc_block(struct rpdfs_fs_info *rfi,
						 struct rpdfs_block_info *binf, u64 bnr,
						 bool with_page, gfp_t gfp)
{
	struct rpdfs_block *found;
	struct rpdfs_block *bk;
	bool retry;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		bk = alloc_block(with_page, gfp);
		if (IS_ERR(bk))
			goto out;

		bk->hnd.bnr = bnr;
		/* our refcount stops shrinker->scan->walk->isolate from removing */
		atomic64_set(&bk->refcount, REMOVAL_REFCOUNT + 1);
		list_lru_add_obj(&binf->lru, &bk->lru_head);
		/* not setting referenced on initial insertion as a form of quick demotion */

		/* try to insert, returning existing or error, retry if removing */
		do {
			rcu_read_lock();
			found = rhashtable_lookup_get_insert_fast(&binf->block_ht, &bk->rhead,
								  block_ht_params);
			retry = !IS_ERR_OR_NULL(found) && !get_block_not_removed(found);
			rcu_read_unlock();
			if (retry)
				cpu_relax();
		} while (retry);

		if (found) {
			list_lru_del_obj(&binf->lru, &bk->lru_head);
			atomic64_set(&bk->refcount, 0);
			free_block(bk);
			bk = found;
		} else {
			_TBP(trace_rpdfs_block_inserted, rfi, bk);
		}
	}
out:
	return bk;
}

static int send_block_read(struct rpdfs_fs_info *rfi, u64 bnr, u8 mode, bool with_data, gfp_t gfp)
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

	rd.bnr = cpu_to_le64(bnr);
	rd.request_mode = mode;
	rd.flags = with_data ? cpu_to_le64(RPDFS_MSG_BLOCK_READ_FLAG_DATA) : 0;
	memzero_explicit(&rd._pad, sizeof(rd._pad));

	ret = rpdfs_map_bnr_to_addr(rfi, bnr, &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

static int send_block_write(struct rpdfs_fs_info *rfi, struct rpdfs_block *bk, u8 mode, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_block_write wr;
	struct rpdfs_net_message_desc md = {
		.type = RPDFS_MSG_BLOCK_WRITE,
		.ctl_buf = &wr,
		.ctl_size = sizeof(wr),
		.data_page = bk->data_page,
		.data_size = bk->data_page ? RPDFS_BLOCK_SIZE : 0,
	};
	u64 mver;
	int ret;

	wr.bnr = cpu_to_le64(bk->hnd.bnr);
	wr.det.alloc_ctr = cpu_to_le64(bk->hnd.alloc_ctr);
	wr.det.wcount = cpu_to_le64(bk->hnd.wcount);
	rpdfs_place_split_le(&wr.det.place_lo, &wr.det.place_hi, bk->hnd.place);
	wr.confirm_mode = mode;
	memzero_explicit(&wr._pad, sizeof(wr._pad));

	ret = rpdfs_map_bnr_to_addr(rfi, bk->hnd.bnr, &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

static int send_block_cache_mode(struct rpdfs_fs_info *rfi, u64 bnr, u8 type, u8 mode, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_cache_mode cm;
	struct rpdfs_net_message_desc md = {
		.type = type,
		.ctl_buf = &cm,
		.ctl_size = sizeof(cm),
	};
	u64 mver;
	int ret;

	cm.bnr = cpu_to_le64(bnr);
	cm.mode = mode;
	memzero_explicit(&cm._pad, sizeof(cm._pad));

	ret = rpdfs_map_bnr_to_addr(rfi, bnr, &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

/*
 * For _SEARCH requests the bnr is only used locally to map to a devd.
 */
static int send_free_stripe_request(struct rpdfs_fs_info *rfi, u64 bnr, u64 flags, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_free_stripe_request fsr;
	struct rpdfs_net_message_desc md = {
		.type = RPDFS_MSG_FREE_STRIPE_REQUEST,
		.ctl_buf = &fsr,
		.ctl_size = sizeof(fsr),
	};
	u64 mver;
	int ret;

	if (flags & RPDFS_MSG_FREE_STRIPE_REQUEST_FLAG_SEARCH)
		fsr.bnr = 0;
	else
		fsr.bnr = cpu_to_le64(bnr);
	fsr.flags = flags;
	memzero_explicit(&fsr._pad, sizeof(fsr._pad));

	ret = rpdfs_map_bnr_to_addr(rfi, bnr, &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

/*
 * See if we can send an explicit confirmation of a previously received
 * revocation.  We can't have active users that conflict with the mode.
 * If we sent a confirm mode with a flushing write then we have to wait
 * for its result to know if the server applied it and if we need to
 * resend its mode if it failed.
 */
static int try_send_confirm(struct rpdfs_fs_info *rfi, struct rpdfs_block *bk)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	int ret = 0;

	if (bk->confirm_mode && !bk->write_confirm_mode &&
	    modes_compatible(cache_using_mode(bk), bk->confirm_mode)) {
		ret = send_block_cache_mode(rfi, bk->hnd.bnr, RPDFS_MSG_BLOCK_CONFIRM_MODE,
					    bk->confirm_mode, GFP_NOWAIT);
		if (ret == 0) {
			_TBP(trace_rpdfs_block_send_confirm, rfi, bk);
			bk->grant_mode = bk->confirm_mode;
			if (bk->grant_mode == RPDFS_CACHE_MODE_NONE) {
				bk->hnd.alloc_ctr = 0;
				bk->hnd.wcount = 0;
				bk->upd_meta = 0;
				bk->upd_data = 0;
			}
			bk->confirm_mode = RPDFS_CACHE_MODE_NULL;
			put_block(rfi, bk);
			try_remove_none(rfi, binf, bk);
		}
	}

	return ret;
}

static unsigned int get_dlist_nr(unsigned int nr)
{
	return nr % NR_DIRTY_LISTS;
}

static struct rpdfs_dirty_list *get_dlist(struct rpdfs_block_info *binf, unsigned int nr)
{
	return &binf->dirty_lists[get_dlist_nr(nr)];
}

/*
 * Returns true if any of the seqs in a are < their counterparts in b.
 */
static bool boundary_any_less(struct rpdfs_dirty_boundary *a, struct rpdfs_dirty_boundary *b)
{
	int i;

	for (i = 0; i < NR_DIRTY_LISTS; i++) {
		if (a->seq[i] < b->seq[i])
			return true;
	}

	return false;
}

/*
 * Returns true if all of the seqs in a are <= their counterparts in b.
 */
static bool boundary_all_less_equal(struct rpdfs_dirty_boundary *a, struct rpdfs_dirty_boundary *b)
{
	int i;

	for (i = 0; i < NR_DIRTY_LISTS; i++) {
		if (a->seq[i] > b->seq[i])
			return false;
	}

	return true;
}

/* dst = max(dst, src) */
static bool extend_boundary(struct rpdfs_dirty_boundary *dst, struct rpdfs_dirty_boundary *src)
{
	bool extended = false;
	int i;

	for (i = 0; i < NR_DIRTY_LISTS; i++) {
		if (src->seq[i] > dst->seq[i]) {
			dst->seq[i] = src->seq[i];
			extended = true;
		}
	}

	return extended;
}

/*
 * Set the caller's boundary to cover the latest blocks dirtied in each
 * dlist at the time of the call.  This can miss later dirtying that
 * happens while this call is iterating over the lists.
 */
static void set_boundary_last_dirty(struct rpdfs_block_info *binf,
				    struct rpdfs_dirty_boundary *bnd)
{
	struct rpdfs_dirty_list *dlist;
	int i;

	for (i = 0; i < NR_DIRTY_LISTS; i++) {
		dlist = &binf->dirty_lists[i];
		while_read_seqretry(&dlist->seqlock)
			bnd->seq[i] = dlist->dirty_seq;
	}
}

/*
 * Make sure that block a's dirty boundary covers block b's.  This is
 * used as we dirty blocks in one transaction to to ensure that they're
 * written together.  This can be called quite a bit for blocks that
 * remain dirty across multiple operations on those blocks.  We spend
 * some effort to avoid taking locks when the block is already covered.
 */
static void dirty_boundary_covers(struct rpdfs_block *a, struct rpdfs_block *b)
{
	struct rpdfs_dirty_boundary bnd;
	bool less;

	while_read_seqretry(&b->seqlock)
		bnd = b->dirty_bnd;

	while_read_seqretry(&a->seqlock)
		less = boundary_any_less(&a->dirty_bnd, &bnd);
	if (less) {
		write_seqlock(&a->seqlock);
		extend_boundary(&a->dirty_bnd, &bnd);
		write_sequnlock(&a->seqlock);
	}
}

/*
 * The caller is an exclusive writer of the block so only we can dirty
 * it, but we use the bk seqlock to consistently serialize with retrying
 * readers.
 *
 * Returns a non-zero count of the size of the dirty list when it
 * dirties.
 */
static unsigned long make_dirty(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
				struct rpdfs_block *bk)
{
	unsigned int nr = get_dlist_nr(raw_smp_processor_id());
	struct rpdfs_dirty_list *dlist;
	unsigned long count = 0;

	if (!bk->dirty) {
		get_block(bk);

		dlist = get_dlist(binf, nr);

		write_seqlock(&bk->seqlock);

		spin_lock_nested(&dlist->seqlock.lock, SINGLE_DEPTH_NESTING);
		write_seqcount_begin_nested(&dlist->seqlock.seqcount, SINGLE_DEPTH_NESTING);

		bk->dirty = 1;
		bk->hnd.wcount++;
		bk->dirty_seq = ++dlist->dirty_seq;
		bk->dirty_list_nr = nr;
		list_add_tail(&bk->dirty_head, &dlist->list);
		count = ++dlist->count;

		/* new dirty seq is always most recent on dlist, > all previous */
		bk->dirty_bnd.seq[nr] = bk->dirty_seq;

		write_seqcount_end(&dlist->seqlock.seqcount);
		spin_unlock(&dlist->seqlock.lock);

		_TBP(trace_rpdfs_block_dirty, rfi, bk);
		write_sequnlock(&bk->seqlock);
	}

	return count;
}

/*
 * The block's dirty_head is on the caller's private dist_write at this
 * point.
 *
 * The block being dirty could have been the last thing preventing
 * sending a confirmation.  As we clean we need to try and send the
 * confirmation of the revoke mode.
 */
static void make_clean(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
		       struct rpdfs_block *bk)
{
	struct rpdfs_dirty_list *dlist;
	bool preloaded;
	int ret;

	BUG_ON(!bk->dirty);
	BUG_ON(list_empty(&bk->dirty_head));

	dlist = get_dlist(binf, bk->dirty_list_nr);
	write_seqlock(&dlist->seqlock);
	dlist->count--;
	write_sequnlock(&dlist->seqlock);

	preloaded = rpdfs_net_preload(rfi, GFP_NOFS) == 0;

	write_seqlock(&bk->seqlock);
	bk->dirty = 0;
	bk->dirty_seq = 0;
	bk->dirty_list_nr = 0;
	list_del_init(&bk->dirty_head);

	ret = try_send_confirm(rfi, bk);
	_TBP(trace_rpdfs_block_clean, rfi, bk);
	write_sequnlock(&bk->seqlock);

	if (preloaded)
		rpdfs_net_preload_end(rfi);

	wake_up_all(&bk->waitq);
	put_block(rfi, bk);

	/* like send failure in _block_release */
	BUG_ON(ret < 0);
}

/*
 * The caller got a write result for a block in a distributed write.  If
 * the write finished then walk the busy writes and complete them in
 * order as they complete.
 */
static void try_complete_write(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
			       struct rpdfs_dist_write *wri)
{
	struct rpdfs_flusher *flshr = binf->flshr;
	struct rpdfs_block *bk;
	struct rpdfs_block *bk__;

	if (atomic_dec_return(&wri->sent_writes) > 0)
		return;

	/* don't bother locking if the first busy write isn't complete */
	while_read_seqretry(&flshr->seqlock) {
		wri = list_first_entry_or_null(&flshr->busy_writes, struct rpdfs_dist_write, head);
		if (wri && atomic_read(&wri->sent_writes) > 0)
			wri = NULL;
	}
	if (!wri)
		return;

	write_seqlock(&flshr->seqlock);
	while ((wri = list_first_entry_or_null(&flshr->busy_writes,
					       struct rpdfs_dist_write, head))) {
		if (wri->completing || atomic_read(&wri->sent_writes) > 0)
			break;

		/* cleaning can preload/send for each block so we drop the flusher lock */
		wri->completing = 1;
		write_sequnlock(&flshr->seqlock);

		list_for_each_entry_safe(bk, bk__, &wri->dirty_list, dirty_head) {
			bk->wri = NULL;
			make_clean(rfi, binf, bk);
			bk = NULL; /* making clean put the refcount we were using */
		}

		write_seqlock(&flshr->seqlock);
		wri->nr_blocks = 0;
		wri->completing = 0;
		extend_boundary(&flshr->complete_bnd, &wri->bnd);
		list_move_tail(&wri->head, &flshr->idle_writes);
	}
	write_sequnlock(&flshr->seqlock);

	wake_up_all(&flshr->waitq);
	queue_work(binf->workq, &flshr->work);
}

/*
 * Each distributed write has its own work that prepares the blocks and
 * sends out each blocks' individual write message.
 */
static void rpdfs_block_write_work_fn(struct work_struct *work)
{
	struct rpdfs_dist_write *wri = container_of(work, struct rpdfs_dist_write, work);
	struct rpdfs_block_info *binf = wri->binf;
	struct rpdfs_fs_info *rfi = binf->rfi;
	struct rpdfs_block *bk;
	u8 mode;
	int ret;

	/* elevate sent writes so that the write can't complete until we're done */
	atomic_set(&wri->sent_writes, wri->nr_blocks + 1);

	list_for_each_entry(bk, &wri->dirty_list, dirty_head) {
		write_seqlock(&bk->seqlock);
		/* flushing excludes writers, compat ignores dirty that's cleared by write */
		if (bk->confirm_mode &&
		    modes_compatible(readers_writer_mode(bk), bk->confirm_mode)) {
			mode = bk->confirm_mode;
			bk->write_confirm_mode = mode;
		} else {
			mode = RPDFS_CACHE_MODE_NULL;
		}
		_TBP(trace_rpdfs_block_send_write, rfi, bk);
		write_sequnlock(&bk->seqlock);

		ret = send_block_write(rfi, bk, mode, GFP_NOFS);
		BUG_ON(ret < 0);
	}

	/* drop our temp elevation, potentially completing the write */
	try_complete_write(rfi, binf, wri);
}

static struct rpdfs_block *first_within_boundary(struct list_head *list,
						 struct rpdfs_dirty_boundary *bnd, unsigned i)
{
	struct rpdfs_block *bk = list_first_entry_or_null(list, struct rpdfs_block, dirty_head);

	if (bk && bk->dirty_seq <= bnd->seq[i])
		return bk;
	return NULL;
}

/*
 * Walk the clear list and set write on each block as long as they're
 * sorted by place.
 *
 * If we hit a block in the clear list that isn't sorted by place then
 * we unwind all the blocks in the set list greater than the out of
 * order block.  The caller will sort the clear list and try again.
 *
 * As we set write we also see if we need to extend the boundary to
 * cover more dependent blocks.  As the caller retries they also check
 * their boundary against the dirty list.
 */
static bool set_write_in_order(struct list_head *set_list, struct list_head *clear_list,
			       struct rpdfs_dist_write *wri, struct rpdfs_dirty_boundary *bnd)
{
	struct rpdfs_block *bk;
	struct rpdfs_block *_bk_;
	bool ordered = true;
	bool extended = false;
	bool writer;
	u128 place;

	if (!list_empty(set_list)) {
		bk = list_last_entry(set_list, struct rpdfs_block, dirty_head);
		place = bk->hnd.place;
	} else {
		place = 0;
	}

	list_for_each_entry_safe(bk, _bk_, clear_list, dirty_head) {
		do {
			write_seqlock(&bk->seqlock);
			/* would be.. corruption error? but we don't have io errors wired up */
			BUG_ON(bk->hnd.place == place);
			ordered = bk->hnd.place > place;
			writer = bk->writer;
			if (ordered && !writer) {
				bk->wri = wri;
				extended |= extend_boundary(bnd, &bk->dirty_bnd);
				list_move_tail(&bk->dirty_head, set_list);
			}
			/* save for unwinding or for continuing ordered test */
			if (!ordered || !writer)
				place = bk->hnd.place;
			write_sequnlock(&bk->seqlock);
			if (ordered && writer)
				wait_event(bk->waitq, !block_has_writer(bk));
		} while (ordered && writer);
		if (!ordered)
			break;
	}

	if (!ordered) {
		list_for_each_entry_safe_reverse(bk, _bk_, set_list, dirty_head) {
			if (bk->hnd.place < place)
				break;
			write_seqlock(&bk->seqlock);
			bk->wri = NULL;
			list_move(&bk->dirty_head, clear_list);
			write_sequnlock(&bk->seqlock);
			wake_up_all(&bk->waitq);
		}
	}

	return extended || !ordered;
}

static int cmp_dirty_head_place(void *priv, const struct list_head *A, const struct list_head *B)
{
	const struct rpdfs_block *a = container_of(A, struct rpdfs_block, dirty_head);
	const struct rpdfs_block *b = container_of(B, struct rpdfs_block, dirty_head);

	/* see list_sort comment for 0/1 cmp return */
	return a->hnd.place > b->hnd.place;
}

/*
 * The flush work is responsible for gathering blocks in the dirty lists
 * into distributed writes.  The distributed writes must cover all the
 * blocks that were modified together in transactions.
 *
 * A boundary is maintained which defines the blocks that will be
 * included from the dirty lists.  As we get each block we see if its
 * dirty boundary should extend the flushing boundary.
 *
 * As we try and set the write pointer in the block, we'll wait for any
 * write acquisitions to finish.  We're racing with writers who will
 * block waiting for the write pointer to clear.  We need to set the
 * write pointer in place order to avoid deadlocking.  We sort blocks by
 * their place before trying to set the write pointer.  The blocks may
 * be actively being dirtied and can have their place changed before we
 * get to them.  We might have to re-sort and retry.
 *
 * Dirty blocks have an elevated refcount.  It's only dropped once we
 * send the blocks to writers for IO and cleaning.  We can operate on
 * all dirty blocks using this reference.
 */
static void rpdfs_block_flush_work_fn(struct work_struct *work)
{
	struct rpdfs_flusher *flshr = container_of(work, struct rpdfs_flusher, work);
	struct rpdfs_block_info *binf = flshr->binf;
	struct rpdfs_dirty_list *dlist;
	struct rpdfs_dist_write *wri;
	struct rpdfs_block *bk;
	LIST_HEAD(clear_list);
	bool retry;
	bool flush;
	int i;

	/* stop fast when there's already max writes in flight */
	wri = list_first_entry_or_null(&flshr->idle_writes, struct rpdfs_dist_write, head);
	if (!wri)
		return;

	/* grab the boundary for the next flshr */
	while_read_seqretry(&flshr->seqlock) {
		flush = boundary_any_less(&flshr->writing_bnd, &flshr->requested_bnd);
		if (flush)
			wri->bnd = flshr->requested_bnd;
	}
	if (!flush)
		return;

	do {

		for (i = 0; i < NR_DIRTY_LISTS; i++) {
			dlist = get_dlist(binf, i);

			/* move blocks from global to private dirty list when within boundary */
			while_read_seqretry(&dlist->seqlock)
				bk = first_within_boundary(&dlist->list, &wri->bnd, i);
			if (bk) {
				write_seqlock(&dlist->seqlock);
				bk = first_within_boundary(&dlist->list, &wri->bnd, i);
				if (bk)
					list_splice_tail_init(&dlist->list, &flshr->dirty_list[i]);
				write_sequnlock(&dlist->seqlock);
			}

			/* get blocks within boundary for the write */
			while ((bk = first_within_boundary(&flshr->dirty_list[i], &wri->bnd, i))) {
				write_seqlock(&bk->seqlock);
				list_move_tail(&bk->dirty_head, &clear_list);
				write_sequnlock(&bk->seqlock);
				wri->nr_blocks++;
			}
		}

		/* sort the blocks we're about to set by place, might still change */
		list_sort(NULL, &clear_list, cmp_dirty_head_place);

		/* set write in place order, maybe retry to sort or get more blocks */
		retry = set_write_in_order(&wri->dirty_list, &clear_list, wri, &wri->bnd);
	} while (retry);

	write_seqlock(&flshr->seqlock);
	extend_boundary(&flshr->writing_bnd, &wri->bnd);
	list_move_tail(&wri->head, &flshr->busy_writes);
	write_sequnlock(&flshr->seqlock);
	queue_work(binf->workq, &wri->work);
}

/*
 * Update the request boundary and queue the flush work if the caller's
 * boundary is beyond what's already been requested.
 */
static void queue_boundary_flush(struct rpdfs_block_info *binf, struct rpdfs_dirty_boundary *bnd)
{
	struct rpdfs_flusher *flshr = binf->flshr;
	bool flush;

	while_read_seqretry(&flshr->seqlock)
		flush = boundary_any_less(&flshr->requested_bnd, bnd);
	if (flush) {
		write_seqlock(&flshr->seqlock);
		if (boundary_any_less(&flshr->requested_bnd, bnd)) {
			extend_boundary(&flshr->requested_bnd, bnd);
			queue_work(binf->workq, &flshr->work);
		}
		write_sequnlock(&flshr->seqlock);
	}
}

/*
 * Queue the flush work if the block is dirty and its dependent dirty
 * boundary is beyond what's been requested.
 */
static void queue_block_flush(struct rpdfs_block_info *binf, struct rpdfs_block *bk)
{
	struct rpdfs_dirty_boundary bnd;
	bool flush;

	while_read_seqretry(&bk->seqlock) {
		flush = bk->dirty;
		if (flush)
			bnd = bk->dirty_bnd;
	}

	if (flush)
		queue_boundary_flush(binf, &bnd);
}

/*
 * Mark the block dirty with the other blocks in the transaction.  If
 * the transaction has another dirty block then make sure the two block
 * boundaries cover each other.  Or record that we're the first dirty
 * block in the txn.
 */
static void dirty_in_txn(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
			 struct rpdfs_transaction *txn, struct rpdfs_block *bk)
{
	struct rpdfs_dirty_boundary bnd;
	struct rpdfs_block *other;
	bool flush = false;

	if (make_dirty(rfi, binf, bk) > DIRTY_BLOCK_LIMIT)
		flush = true;

	if (txn->dirty_bnr) {
		other = lookup_block(binf, txn->dirty_bnr);
		if (other) {
			dirty_boundary_covers(bk, other);
			dirty_boundary_covers(other, bk);
			put_block(rfi, other);
		}
	} else {
		txn->dirty_bnr = bk->hnd.bnr;
	}

	if (flush) {
		while_read_seqretry(&bk->seqlock)
			bnd = bk->dirty_bnd;
		queue_boundary_flush(binf, &bnd);
	}
}

/*
 * See if the caller can acquire the block with their mode and flags.
 * This is primarily called under a write seqlock when the caller will
 * act on the return.  It's also called during a read seqlock retry as a
 * wait condition to see if we should wake up and try again under the
 * write seqlock.
 *
 * Returns:
 *          < 0: hard error that should be returned without acquiring
 *            0: keep waiting as long as this returns zero
 *     CAN_SEND: send a request for the mode and then wait
 *  CAN_ACQUIRE: success, increment counts and return handle
 */
#define CAN_SEND      1
#define CAN_ACQUIRE   2
static int can_acquire(struct rpdfs_block_info *binf, struct rpdfs_block *bk, u8 mode, rbaf_t rbaf)
{
	int ret;

	/* return a read error */
	if (bk->error && !(rbaf & RBAF_OVERWRITE)) {
		ret = bk->error;
		goto out;
	}

	/* only satisfy allocs from idle write mode free blocks */
	if ((rbaf & RBAF_ALLOC) &&
	    !(bk->grant_mode == RPDFS_CACHE_MODE_WRITE &&
	      bk->confirm_mode == RPDFS_CACHE_MODE_NULL &&
	      readers_writer_mode(bk) == RPDFS_CACHE_MODE_NONE &&
	      alloc_ctr_is_free(bk->hnd.alloc_ctr))) {
		ret = -ENODATA;
		goto out;
	}

	if ((rbaf & RBAF_ALREADY_DIRTY) && !bk->dirty) {
		ret = -ENODATA;
		goto out;
	}

	/* don't have sufficient mode */
	if ((mode > bk->grant_mode) || (bk->confirm_mode && mode > bk->confirm_mode)) {
		if (rbaf & RBAF_NONBLOCK_MODE)
			ret = -EAGAIN;
		else if (mode > bk->request_mode)
			ret = CAN_SEND;
		else
			ret = 0;
		goto out;
	}

	/* write acquisition waits for writeback to finish */
	if ((rbaf & RBAF_WRITE) && bk->wri) {
		if (rbaf & RBAF_NONBLOCK_FLUSH)
			ret = -EAGAIN;
		else
			ret = 0;
		goto out;
	}

	/* finally acquire if we're compatible with others, or wait for incompat to finish */
	if (modes_compatible(readers_writer_mode(bk), mode))
		ret = CAN_ACQUIRE;
	else
		ret = 0;
out:
	BUG_ON(ret > CAN_ACQUIRE);
	return ret;
}

static bool retry_acquire(struct rpdfs_block_info *binf, struct rpdfs_block *bk, u8 mode,
			  rbaf_t rbaf)
{
	bool retry;

	while_read_seqretry(&bk->seqlock)
		retry = can_acquire(binf, bk, mode, rbaf) != 0;

	return retry;
}

/*
 * Acquire a read or write handle to a block.  Returns 0 on success with
 * the handle pointing at the block.  _block_release() must be called on
 * the handle when the caller is done.
 *
 * With no flags, a readable handle is returned.  _WRITE acquires a
 * write handle excludes all other handles.
 *
 * A transaction must be provided along with _WRITE to dirty blocks
 * together in the transaction.
 */
int rpdfs_block_acquire(struct rpdfs_fs_info *rfi, struct rpdfs_transaction *txn, u64 bnr,
			struct rpdfs_block_handle **hnd_ret, rbaf_t rbaf)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_block *bk = NULL;
	bool need_data;
	u8 mode;
	int ret;

	if (WARN_ON_ONCE((rbaf & RBAF_OVERWRITE) && !(rbaf & RBAF_WRITE)) ||
	    WARN_ON_ONCE((rbaf & RBAF_WRITE) && !txn) ||
	    WARN_ON_ONCE(*hnd_ret != NULL) ||
	    WARN_ON_ONCE(bnr == 0)) {
		ret = -EINVAL;
		goto out;
	}

	if ((rbaf & (RBAF_ALLOC | RBAF_ALREADY_DIRTY))) {
		bk = lookup_block(binf, bnr);
		if (!bk)
			bk = ERR_PTR(-ENODATA);
	} else {
		bk = lookup_or_alloc_block(rfi, binf, bnr, true, GFP_NOFS);
	}
	if (IS_ERR(bk)) {
		ret = PTR_ERR(bk);
		goto out;
	}

	ret = alloc_data_page(bk, GFP_NOFS);
	if (ret < 0)
		goto out;

	mode = mode_from_rbaf(rbaf);
	need_data = !(rbaf & RBAF_OVERWRITE);
	for (;;) {
		ret = rpdfs_net_preload(rfi, GFP_NOFS);
		if (ret < 0)
			goto out;

		write_seqlock(&bk->seqlock);

		ret = can_acquire(binf, bk, mode, rbaf);
		if (ret == CAN_SEND) {
			/* ret turns into err or 0 to wait after successful send */
			if (!bk->upd_meta || (!bk->upd_data && need_data))
				ret = send_block_read(rfi, bk->hnd.bnr, mode, need_data,
						      GFP_NOWAIT);
			else
				ret = send_block_cache_mode(rfi, bk->hnd.bnr,
							    RPDFS_MSG_BLOCK_REQUEST_MODE,
							    mode, GFP_NOWAIT);
			if (ret == 0) {
				if (!bk->request_mode)
					get_block(bk);
				bk->request_mode = mode;
			}
		} else if (ret == CAN_ACQUIRE) {
			if (rbaf & RBAF_WRITE)
				bk->writer = 1;
			else
				bk->readers++;
			if (rbaf & RBAF_OVERWRITE)
				bk->error = 0;
			if (rbaf & RBAF_ALLOC)
				bk->hnd.alloc_ctr++;
			*hnd_ret = &bk->hnd;
		}

		write_sequnlock(&bk->seqlock);

		rpdfs_net_preload_end(rfi);

		if (ret == CAN_ACQUIRE) {
			if (rbaf & RBAF_WRITE)
				dirty_in_txn(rfi, binf, txn, bk);
			ret = 0;
			break;
		}

		if (ret == 0) {
			rpdfs_prd_rfi(rfi, "waiting mode %u rbaf %x "RBF, mode, rbaf, RBA(bk));
			ret = wait_event_interruptible(bk->waitq,
						       retry_acquire(binf, bk, mode, rbaf));
		}
		if (ret < 0)
			break;
	}
out:
	if (ret < 0)
		put_block(rfi, bk);
	rpdfs_prd_rfi(rfi, "bnr %llu rbaf %x ret %d", bnr, rbaf, ret);
	return ret;
}

/*
 * Release a handle on a block.  If the caller had a handle it's
 * released and we set the caller's handle to NULL.  It's a nop if the
 * handle was already null.
 *
 * A clean block with a writer can receive a revocation before it is
 * dirty, so the revocation didn't have a chance to use its dirty
 * boundary to request a flush.  We detect that here and queue a flush
 * as we release.
 */
void rpdfs_block_release(struct rpdfs_fs_info *rfi, struct rpdfs_block_handle **hnd)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_block *bk;
	bool flush = false;
	bool preloaded;
	int ret;

	if (*hnd == NULL)
		return;
	bk = container_of(*hnd, struct rpdfs_block, hnd);
	*hnd = NULL;

	preloaded = rpdfs_net_preload(rfi, GFP_NOFS) == 0;

	write_seqlock(&bk->seqlock);
	/* XXX not great that we're presuming the intent of the release :/ */
	if (bk->writer) {
		bk->writer = 0;
		if (bk->dirty && bk->grant_mode < RPDFS_CACHE_MODE_WRITE)
			flush = true;
	} else {
		bk->readers--;
	}
	ret = try_send_confirm(rfi, bk);
	write_sequnlock(&bk->seqlock);

	if (preloaded)
		rpdfs_net_preload_end(rfi);
	if (flush)
		queue_block_flush(binf, bk);

	wake_up_all(&bk->waitq);
	put_block(rfi, bk);

	/*
	 * This failure is not a failure to release, it's a failure to
	 * participate in the cache coherency protocol.  We'd go
	 * read-only, abort, etc.
	 */
	BUG_ON(ret < 0);
}

/*
 * Return true if the block is cached and dirty at the moment.  This is
 * inherently racy and the caller must be prepared for the block to no
 * longer be dirty even before this returns.
 */
bool rpdfs_block_is_dirty(struct rpdfs_fs_info *rfi, u64 bnr)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_block *bk;
	bool dirty;

	bk = lookup_block(binf, bnr);
	if (bk) {
		while_read_seqretry(&bk->seqlock)
			dirty = bk->dirty;
		put_block(rfi, bk);
	} else {
		dirty = false;
	}

	return dirty;
}

static bool flushed_within(struct rpdfs_block_info *binf, struct rpdfs_dirty_boundary *bnd)
{
	struct rpdfs_flusher *flshr = binf->flshr;
	bool flushed;

	while_read_seqretry(&flshr->seqlock)
		flushed = boundary_all_less_equal(bnd, &flshr->complete_bnd);

	return flushed;
}

/*
 * XXX Errors aren't plumbed through write results and distributed write
 * completion to here.  I think we could have a simple list of pending
 * flush waiters.  If we see an io error for a write within their
 * boundary we can set it in their waiting entry on a list and wake
 * them.  Both waiters and errors are both rare slow paths.
 */
static int flush_and_wait(struct rpdfs_block_info *binf, struct rpdfs_dirty_boundary *bnd,
			  bool wait)
{
	struct rpdfs_flusher *flshr = binf->flshr;
	int ret;

	queue_boundary_flush(binf, bnd);

	if (wait)
		ret = wait_event_interruptible(flshr->waitq, flushed_within(binf, bnd));
	else
		ret = 0;

	return ret;
}

/*
 * If the caller's block is dirty then we start flushing it, possibly
 * waiting to return an error if the flush attempt failed.  The block
 * may not be currently dirty or even cached.
 */
int rpdfs_block_flush(struct rpdfs_fs_info *rfi, u64 bnr, bool wait)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_dirty_boundary bnd;
	struct rpdfs_block *bk;
	bool dirty;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		ret = 0;
		goto out;
	}

	while_read_seqretry(&bk->seqlock) {
		dirty = bk->dirty;
		if (dirty)
			bnd = bk->dirty_bnd;
	}
	put_block(rfi, bk);

	if (dirty)
		ret = flush_and_wait(binf, &bnd, wait);
	else
		ret = 0;
out:
	return ret;
}

/*
 * Queue flushing of any dirty blocks that were dirtied at the time of
 * the call, possibly waiting for all the blocks to be flushed before
 * returning.  This will not flush or wait for blocks that are dirtied
 * after it samples the initial boundary of blocks on the dirty lists.
 */
int rpdfs_block_sync(struct rpdfs_fs_info *rfi, bool wait)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_dirty_boundary bnd;

	set_boundary_last_dirty(binf, &bnd);

	return flush_and_wait(binf, &bnd, wait);
}

/*
 * Returns true when the caller could send a free stripe request to keep
 * one in flight.  &until is initialized to 0 and then set by each send.
 */
bool rpdfs_block_should_request_free(struct rpdfs_fs_info *rfi, u64 until)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);

	return atomic64_read(&binf->regions_done) >= until;
}

/*
 * Send a request, and update the caller's until so that
 * _should_request_free will return .. roughly once our request has been
 * processed.
 *
 * We round-robin requests amongst the devds by having an increasing
 * _started counter that acts as a fake block number that is mapped to
 * the devds.
 */
int rpdfs_block_request_free(struct rpdfs_fs_info *rfi, u64 *until)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	u64 bnr;
	int ret;

	bnr = atomic64_inc_return(&binf->regions_started);
	ret = send_free_stripe_request(rfi, bnr, RPDFS_MSG_FREE_STRIPE_REQUEST_FLAG_SEARCH,
				       GFP_NOFS);
	if (ret < 0)
		atomic64_inc(&binf->regions_done);
	else
		*until = bnr;

	return ret;
}

/*
 * Receive a read result for our previously sent read.  We must have
 * pinned the block while the request was in flight.  If we send a mode
 * request while the read is in flight then the read response might
 * contain a grant mode that covers the requested mode, depending on
 * server processing sequencing.
 */
static int recv_block_read_result(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_block_read_result *rr = md->ctl_buf;
	u64 bnr = le64_to_cpu(rr->bnr);
	struct rpdfs_block *bk;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		ret = -EPROTO;
		goto out;
	}

	write_seqlock(&bk->seqlock);
	if (rr->grant_mode && rr->grant_mode < bk->grant_mode) {
		ret = -EPROTO;
	} else {
		bk->hnd.place = rpdfs_place_combine_le(rr->det.place_lo, rr->det.place_hi);
		bk->hnd.alloc_ctr = le64_to_cpu(rr->det.alloc_ctr);
		bk->hnd.wcount = le64_to_cpu(rr->det.wcount);
		bk->upd_meta = 1;

		if (md->data_page) {
			if (bk->data_page)
				put_page(bk->data_page);
			bk->data_page = md->data_page;
			get_page(bk->data_page);
			bk->hnd.data = page_address(bk->data_page);

			bk->upd_data = 1;
		}

		/* can immediately elevate mode, server serializes with revoke */
		if (rr->grant_mode) {
			bk->grant_mode = rr->grant_mode;
			if (bk->request_mode <= rr->grant_mode) {
				bk->request_mode = RPDFS_CACHE_MODE_NULL;
				put_block(rfi, bk);
			}
		}

		ret = 0;
	}
	write_sequnlock(&bk->seqlock);

	wake_up_all(&bk->waitq);
	put_block(rfi, bk);
out:
	return ret;
}

static int recv_block_write_result(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_block_write_result *wr = md->ctl_buf;
	u64 bnr = le64_to_cpu(wr->bnr);
	struct rpdfs_block *bk;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		ret = -EPROTO;
		goto out;
	}

	ret = rpdfs_net_preload(rfi, GFP_NOFS);
	if (ret < 0)
		goto put;

	/* resolve pending confirm_mode sent with write */
	write_seqlock(&bk->seqlock);
	if (bk->write_confirm_mode) {
		bk->grant_mode = bk->write_confirm_mode;
		if (bk->grant_mode == RPDFS_CACHE_MODE_NONE) {
			bk->hnd.alloc_ctr = 0;
			bk->hnd.wcount = 0;
			bk->upd_meta = 0;
			bk->upd_data = 0;
		}
		if (bk->write_confirm_mode == bk->confirm_mode) {
			bk->confirm_mode = RPDFS_CACHE_MODE_NULL;
			put_block(rfi, bk);
		}
		bk->write_confirm_mode = RPDFS_CACHE_MODE_NULL;

		try_remove_none(rfi, binf, bk);
	}
	write_sequnlock(&bk->seqlock);

	rpdfs_net_preload_end(rfi);

	try_complete_write(rfi, binf, bk->wri);
put:
	put_block(rfi, bk);
out:
	return ret;
}

/*
 * We pin a block when we send a request.  The server may take a while
 * to process the request, but it eventually will.  We're guaranteed a
 * grant response.  We can send back to back requests for increasing
 * modes.  We may get back to back grants, or we may get a grant for the
 * highest mode by the time the server processes the request.
 *
 * Our request can cross with a free_stripe_grant that gives us a higher
 * mode than we requested.  Then the server will grant us the current mode, which
 * can be higher than the requested mode.
 */
static int recv_block_grant_mode(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_cache_mode *cm = md->ctl_buf;
	u64 bnr = le64_to_cpu(cm->bnr);
	struct rpdfs_block *bk;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		/* we should have pinned, server sent unrequested grant? */
		ret = -EPROTO;
		goto out;
	}

	rpdfs_prd("mode %u "RBF, cm->mode, RBA(bk));

	/* grants should only elevate */
	write_seqlock(&bk->seqlock);
	if (cm->mode < bk->grant_mode) {
		ret = -EPROTO;
	} else {
		bk->grant_mode = cm->mode;
		if (bk->request_mode <= cm->mode) {
			bk->request_mode = RPDFS_CACHE_MODE_NULL;
			put_block(rfi, bk);
		}
		ret = 0;
	}
	write_sequnlock(&bk->seqlock);
	wake_up_all(&bk->waitq);
	put_block(rfi, bk);
out:
	return ret;
}

/*
 * Revocations are only received for previously granted modes.  They
 * should only decrease our granted mode.  We will only receive one
 * revoke mode at a time.
 *
 * However, we can free clean blocks under memory pressure.  Then we can
 * allocate a new block that is being acquired.  We can receive a revoke
 * for the old block number that was freed at any point in that life
 * cycle.  We need to be forgiving of the state of blocks when we
 * receive a revocation.
 *
 * We must send a confirmation for every revoke we receive, and we have
 * to wait until users of the revoked mode (including dirty blocks) have
 * finished.  We record that we need to confirm the mode and this
 * pending confirmation stops future incompatible users.
 */
static int recv_block_revoke_mode(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_cache_mode *cm = md->ctl_buf;
	u64 bnr = le64_to_cpu(cm->bnr);
	struct rpdfs_block *bk;
	bool preloaded;
	bool flush;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		/* no block, we shrank, send immediate confirmation */
		ret = send_block_cache_mode(rfi, bnr, RPDFS_MSG_BLOCK_CONFIRM_MODE,
					    cm->mode, GFP_NOFS);
		if (ret == 0)
			trace_rpdfs_block_send_uncached_confirm(rfi, bnr, cm->mode);
		goto out;
	}

	preloaded = rpdfs_net_preload(rfi, GFP_NOFS) == 0;

	write_seqlock(&bk->seqlock);
	if (!bk->confirm_mode) {
		get_block(bk);
		bk->confirm_mode = cm->mode;
		flush = bk->dirty;
		ret = try_send_confirm(rfi, bk);
	} else {
		ret = -EPROTO;
	}
	write_sequnlock(&bk->seqlock);

	if (preloaded)
		rpdfs_net_preload_end(rfi);
	if (flush)
		queue_block_flush(binf, bk);

	if (ret == 0)
		wake_up_all(&bk->waitq);
	put_block(rfi, bk);
out:
	return ret;
}

/*
 * Receive a free stripe grant from the server.  The message contains
 * details for any blocks in the devd's stripe that were free and that
 * we now have been granted write mode.
 *
 * This incoming stripe could be the first from a region that was the
 * result of a search.  In this case we send requests to the rest of the
 * devds that own the rest of the stripes.
 *
 * For each block, the server granted write mode when no one had the
 * block cached.  The only way we could have a local cached block
 * allocated for these free blocks is if we sent a request while the
 * incoming free stripe grant was in flight.  Because the search returns
 * an unknown block, we don't have blocks that recorded that we had a
 * free stripe request in flight.  In effect, this can be an unsolicited
 * write grant that crosses the wire with other requests.  We must
 * update the mode to match the server, but we don't have block data so
 * the request will still see a read result.
 */
static int recv_free_stripe_grant(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_free_stripe_grant *fsg = md->ctl_buf;
	struct rpdfs_msg_free_stripe_detail *fsd = NULL;
	const u64 grant_bnr = le64_to_cpu(fsg->bnr);
	struct rpdfs_block_region_builder *rbld;
	unsigned long this_stripe;
	unsigned long stripes;
	struct rpdfs_block *bk;
	int nr_blocks;
	int in_flight;
	u64 mver;
	u64 bnr;
	int b;
	int i;
	int ret;

	/* we're handing a le bitmap to bitmap_weight, can't deal with partial words */
	BUILD_BUG_ON((sizeof(fsg->bmap) % sizeof(long)) != 0);

	nr_blocks = bitmap_weight((unsigned long *)fsg->bmap, RPDFS_MSG_BLOCKS_PER_FREE_STRIPE);
	if (md->data_size != nr_blocks * sizeof(fsd[0])) {
		ret = -EPROTO;
		goto out;
	}

	if (nr_blocks)
		fsd = page_address(md->data_page);

	/* XXX we're ignoring the mver output here */
	ret = rpdfs_map_alloc_stripe_geom(rfi, grant_bnr, &this_stripe, &stripes, &mver);
	if (ret < 0)
		goto out;

	for (b = 0, i = 0;
	     (b = find_next_bit_le(&fsg->bmap, RPDFS_MSG_BLOCKS_PER_FREE_STRIPE, b))
			< RPDFS_MSG_BLOCKS_PER_FREE_STRIPE;
	     b++, i++) {
		bnr = grant_bnr + (b * stripes);

		bk = lookup_or_alloc_block(rfi, binf, bnr, false, GFP_NOFS);
		if (IS_ERR(bk)) {
			ret = PTR_ERR(bk);
			goto out;
		}

		write_seqlock(&bk->seqlock);
		bk->hnd.alloc_ctr = le64_to_cpu(fsd[i].alloc_ctr);
		bk->hnd.wcount = le64_to_cpu(fsd[i].wcount);
		rpdfs_block_set_place(&bk->hnd, RPDFS_PLACE_FREE, 0, 0, bk->hnd.bnr);
		bk->upd_meta = 1;
		bk->grant_mode = RPDFS_CACHE_MODE_WRITE;
		write_sequnlock(&bk->seqlock);

		/* most will be newly allocated and idle, but might have request waiters */
		wake_up_all(&bk->waitq);
		put_block(rfi, bk);
	}

	bnr = grant_bnr - this_stripe;
	rbld = get_rbld(rfi, binf, bnr, stripes);
	if (IS_ERR(rbld)) {
		ret = PTR_ERR(rbld);
		goto out;
	}

	spin_lock(&rbld->lock);
	rpdfs_balloc_set_stripe_bits(rbld->reg, this_stripe, stripes, fsg->bmap,
				     RPDFS_MSG_BLOCKS_PER_FREE_STRIPE);
	spin_unlock(&rbld->lock);

	/* only the first search result for a given rbld will send requests to others */
	if (fsg->flags & RPDFS_MSG_FREE_STRIPE_GRANT_FLAG_SEARCH) {
		in_flight = atomic_cmpxchg(&rbld->in_flight, stripes, stripes - 1) - 1;
		if (in_flight == stripes - 1) {
			for (i = 0; i < stripes; i++) {
				if (i == this_stripe)
					continue;
				ret = send_free_stripe_request(rfi, bnr + i, 0, GFP_NOFS);
				if (ret < 0)
					in_flight = atomic_dec_return(&rbld->in_flight);
			}
		}
	} else {
		in_flight = atomic_dec_return(&rbld->in_flight);
	}

	/* publish the region if all requests are answered (or failed to send) */
	if (in_flight == 0)
		rpdfs_balloc_publish_region(rbld->rfi, rbld->reg);

	put_rbld(binf, rbld);

	ret = 0;
out:
	return ret;
}

/*
 * Firstly, we promote blocks that have been accessed since the last
 * scan.  Then we'll only remove blocks that don't have any other
 * refcount holders.  Many block states (request in flight, confirm
 * pending, dirty) hold refcounts specifically so they're excluded from
 * shrinking.
 */
static enum lru_status scoutfs_block_isolate(struct list_head *item, struct list_lru_one *list,
					     void *cb_arg)
{
	struct rpdfs_block *bk = container_of(item, struct rpdfs_block, lru_head);
	struct list_head *isolated = cb_arg;

	if (test_and_clear_bit(RPDFS_BLOCK_BIT_ACCESSED, &bk->bits))
		return LRU_ROTATE;

	if (!get_block_removal(bk, 0))
		return LRU_SKIP;

	list_lru_isolate_move(list, &bk->lru_head, isolated);
	return LRU_REMOVED;
}

static unsigned long rpdfs_block_count_objects(struct shrinker *shrinker,
					       struct shrink_control *sc)
{
	struct rpdfs_block_info *binf = shrinker->private_data;

	return list_lru_shrink_count(&binf->lru, sc);
}

/* The caller acquired the removal ref while moving off the lru. */
static void remove_isolated_list(struct rpdfs_fs_info *rfi, struct rpdfs_block_info *binf,
				 struct list_head *isolated)
{
	struct rpdfs_block *bk;
	struct rpdfs_block *bk__;

	list_for_each_entry_safe(bk, bk__, isolated, lru_head) {
		rhashtable_remove_fast(&binf->block_ht, &bk->rhead, block_ht_params);
		list_del_init(&bk->lru_head);
		put_block(rfi, bk);
	}
}

static unsigned long rpdfs_block_scan_objects(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct rpdfs_block_info *binf = shrinker->private_data;
	LIST_HEAD(isolated);
	unsigned long freed;

	freed = list_lru_shrink_walk(&binf->lru, sc, scoutfs_block_isolate, &isolated);
	remove_isolated_list(binf->rfi, binf, &isolated);
	return freed;
}

static void init_flusher(struct rpdfs_block_info *binf, struct rpdfs_flusher *flshr)
{
	int i;

	flshr->binf = binf;
	INIT_WORK(&flshr->work, rpdfs_block_flush_work_fn);
	init_waitqueue_head(&flshr->waitq);
	seqlock_init(&flshr->seqlock);
	INIT_LIST_HEAD(&flshr->idle_writes);
	INIT_LIST_HEAD(&flshr->busy_writes);

	for (i = 0; i < NR_DIRTY_LISTS; i++)
		INIT_LIST_HEAD(&flshr->dirty_list[i]);
}

static void init_dirty_list(struct rpdfs_dirty_list *dlist)
{
	seqlock_init(&dlist->seqlock);
	INIT_LIST_HEAD(&dlist->list);
}

static void init_dist_write(struct rpdfs_block_info *binf, struct rpdfs_dist_write *wri)
{
	wri->binf = binf;
	INIT_LIST_HEAD(&wri->head);
	INIT_WORK(&wri->work, rpdfs_block_write_work_fn);
	atomic_set(&wri->sent_writes, 0);
	INIT_LIST_HEAD(&wri->dirty_list);
}

static void free_dist_writes(struct list_head *a, struct list_head *b)
{
	struct rpdfs_dist_write *wri;
	struct rpdfs_dist_write *wri__;

	list_splice_init(a, b);
	list_for_each_entry_safe(wri, wri__, b, head) {
		list_del_init(&wri->head);
		kfree(wri);
	}
}

int rpdfs_block_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_block_info *binf;
	struct rpdfs_dist_write *wri;
	int ret;
	int i;

	/* using WQ_MEM_RECLAIM for group write work */
	binf = kzalloc(sizeof(struct rpdfs_block_info), GFP_KERNEL);
	if (binf) {
		binf->shrinker = shrinker_alloc(0, "rpdfs-block");
		binf->workq = alloc_workqueue("rpdfs-block", WQ_MEM_RECLAIM, 0);
		binf->flshr = kzalloc(sizeof(struct rpdfs_flusher), GFP_KERNEL);
		binf->dirty_lists = kzalloc(sizeof(struct rpdfs_flusher) * NR_DIRTY_LISTS,
					    GFP_KERNEL);
	}
	if (!binf || !binf->shrinker || !binf->workq || !binf->flshr || !binf->dirty_lists) {
		ret = -ENOMEM;
		goto out;
	}

	binf->rfi = rfi;
	init_flusher(binf, binf->flshr);
	atomic64_set(&binf->regions_started, 0);
	atomic64_set(&binf->regions_done, 0);

	binf->shrinker->scan_objects = rpdfs_block_scan_objects;
	binf->shrinker->count_objects = rpdfs_block_count_objects;
	binf->shrinker->private_data = binf;
	shrinker_register(binf->shrinker);

	for (i = 0; i < NR_DIRTY_LISTS; i++)
		init_dirty_list(&binf->dirty_lists[i]);

	for (i = 0; i < NR_DIST_WRITES; i++) {
		wri = kzalloc(sizeof(struct rpdfs_dist_write), GFP_KERNEL);
		if (!wri) {
			ret = -ENOMEM;
			goto out;
		}
		init_dist_write(binf, wri);
		list_add_tail(&wri->head, &binf->flshr->idle_writes);
	}

	ret = list_lru_init(&binf->lru);
	if (ret < 0)
		goto out;

	ret = rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_READ_RESULT, recv_block_read_result) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_WRITE_RESULT, recv_block_write_result) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_GRANT_MODE, recv_block_grant_mode) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_REVOKE_MODE, recv_block_revoke_mode) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_FREE_STRIPE_GRANT, recv_free_stripe_grant);
	if (ret < 0)
		goto out;

	ret = rhashtable_init(&binf->block_ht, &block_ht_params);
	if (ret < 0)
		goto out;

	ret = rhashtable_init(&binf->rbld_ht, &rbld_ht_params);
	if (ret < 0)
		goto out;

	SET_RPDFS_BINF(rfi, binf);
	ret = 0;
out:
	if (ret < 0 && binf) {
		if (binf->shrinker)
			shrinker_free(binf->shrinker);
		if (binf->workq)
			destroy_workqueue(binf->workq);
		list_lru_destroy(&binf->lru);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_READ_RESULT,
					  recv_block_read_result);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_WRITE_RESULT,
					  recv_block_write_result);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_GRANT_MODE,
					  recv_block_grant_mode);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_REVOKE_MODE,
					  recv_block_revoke_mode);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_FREE_STRIPE_GRANT,
					  recv_free_stripe_grant);
		if (binf->flshr) {
			free_dist_writes(&binf->flshr->idle_writes, &binf->flshr->busy_writes);
			kfree(binf->flshr);
		}
		kfree(binf->dirty_lists);
		kfree(binf);

	}
	return ret;
}

/*
 * The caller is calling with blocks as they're removed from the hash
 * table.  The rest of the system has been shutdown so the block state
 * will not change once we get here.  We could be aggressively tearing
 * down after having interrupted normal operations.
 */
static void free_and_destroy_block(void *ptr, void *arg)
{
	struct rpdfs_block *bk = ptr;
	struct rpdfs_block_info *binf = arg;
	struct rpdfs_fs_info *rfi = binf->rfi;

	if (bk->request_mode)
		put_block(rfi, bk);
	if (bk->confirm_mode)
		put_block(rfi, bk);

	/* this can pull off of dirty lists, the flusher, or distributed writes */
	if (bk->dirty) {
		bk->dirty = 0;
		if (!list_empty(&bk->dirty_head))
			list_del_init(&bk->dirty_head);
		put_block(rfi, bk);
	}

	if (get_block_removal(bk, 0)) {
		list_lru_del_obj(&binf->lru, &bk->lru_head);
		put_block(rfi, bk);
	}
}

/*
 * There should be no more users of the builders as we're destroying.
 * The caller has removed from the hash table and we just need to free.
 */
static void free_and_destroy_rbld(void *ptr, void *arg)
{
	struct rpdfs_block_region_builder *rbld = ptr;

	if (rbld->reg)
		rpdfs_balloc_free_region(rbld->reg);
	kfree(rbld);
}

void rpdfs_block_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_dist_write *wri;

	if (binf) {
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_READ_RESULT,
					  recv_block_read_result);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_WRITE_RESULT,
					  recv_block_write_result);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_GRANT_MODE,
					  recv_block_grant_mode);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_REVOKE_MODE,
					  recv_block_revoke_mode);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_FREE_STRIPE_GRANT,
					  recv_free_stripe_grant);
		shrinker_free(binf->shrinker);

		cancel_work_sync(&binf->flshr->work);

		list_splice_init(&binf->flshr->idle_writes, &binf->flshr->busy_writes);
		list_for_each_entry(wri, &binf->flshr->busy_writes, head)
			cancel_work_sync(&wri->work);

		destroy_workqueue(binf->workq);

		rhashtable_free_and_destroy(&binf->block_ht, free_and_destroy_block, binf);
		list_lru_destroy(&binf->lru);
		free_dist_writes(&binf->flshr->idle_writes, &binf->flshr->busy_writes);
		rhashtable_free_and_destroy(&binf->rbld_ht, free_and_destroy_rbld, NULL);
		kfree(binf->flshr);
		kfree(binf);

		SET_RPDFS_BINF(rfi, NULL);
	}
}
