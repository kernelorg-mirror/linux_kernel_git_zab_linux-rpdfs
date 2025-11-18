/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/rhashtable.h>

#include "block.h"
#include "compare.h"
#include "format-block.h"
#include "format-msg.h"
#include "lists.h"
#include "map.h"
#include "net.h"
#include "pr.h"
#include "rht.h"

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
 * It's only set by the server as we receive either read_result or
 * grant_mode messages.  It's decreased after receiving a revocation,
 * users drain, and we send a confirmation.
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

enum {
	RPDFS_BLOCK_BIT_ACCESSED = 0,
};

#define bitfield_char(f, c) ((f) ? c : '-')
#define RBF \
	"bnr %llu wc %llu refc %llx rd %lu %c%c%c%c rm %u gm %u cm %u wcm %u"
#define RBA(bk) \
	(bk)->hnd.bnr, (bk)->hnd.wcount, atomic64_read(&(bk)->refcount), (bk)->readers, \
	bitfield_char(bk->dirty, 'd'), bitfield_char(bk->upd_meta,  'M'), \
	bitfield_char(bk->upd_data, 'D'), bitfield_char(bk->writer,  'w'), \
	(bk)->request_mode, (bk)->grant_mode, (bk)->confirm_mode, (bk)->write_confirm_mode

/*
 * We trigger a flush once a dirty list exceeds this block limit.  The
 * resulting distributed write can have this many blocks from each dirty
 * list, and a bit more for the size of the transaction that pushed the
 * count over the limit.
 */
#define DIRTY_BLOCK_LIMIT	128

static const struct rhashtable_params block_ht_params = {
	.key_len	= sizeof_field(struct rpdfs_block, hnd.bnr),
	.key_offset	= offsetof(struct rpdfs_block, hnd.bnr),
	.head_offset	= offsetof(struct rpdfs_block, rhead),
};

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
 * Execute the for() loop statement at least once, and continue
 * executing it as long as read_seqretry() says we have to.  The caller
 * can safely break out of the statement but they can't trust any output
 * of the statement if they do so.
 */
#define while_read_seqretry(seql) \
	for (unsigned seq__, retry__ = 1; \
	     retry__ && ({ seq__ = read_seqbegin(seql); true; }); \
	     retry__ = read_seqretry((seql), seq__))

static struct rpdfs_block *alloc_block(gfp_t gfp)
{
	struct rpdfs_block *bk;

	bk = kzalloc(sizeof(struct rpdfs_block), gfp);
	if (bk)
		bk->data_page = alloc_page(gfp);
	if (!bk || !bk->data_page) {
		kfree(bk);
		bk = NULL;
	} else {
		INIT_LIST_HEAD(&bk->lru_head);
		bk->hnd.data = page_address(bk->data_page);
		atomic64_set(&bk->refcount, 0);
		seqlock_init(&bk->seqlock);
		init_waitqueue_head(&bk->waitq);
		INIT_LIST_HEAD(&bk->dirty_head);
	}

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
	put_page(bk->data_page);
	kfree_rcu(bk, rcu);
}

/*
 * While blocks are in the hash table and lru they have a huge value
 * added to the refcount.  Lookup returns new references to blocks as
 * long as this value is here.  The exclusive right to remove from the
 * cache is only allowed by atomically removing this value and adding a
 * normal refcount.
 */
#define REMOVAL_REFCOUNT (S64_MAX ^ (S64_MAX >> 1))

static void put_block(struct rpdfs_block *bk)
{
	s64 now;

	if (!IS_ERR_OR_NULL(bk) && (now = atomic64_dec_return(&bk->refcount)) == 0)
		free_block(bk);

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
static void try_remove_none(struct rpdfs_block_info *binf, struct rpdfs_block *bk)
{
	if (block_is_none(bk) && get_block_removal(bk, 1)) {
		rhashtable_remove_fast(&binf->block_ht, &bk->rhead, block_ht_params);
		list_lru_del_obj(&binf->lru, &bk->lru_head);
		put_block(bk);
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

/*
 * Returns the block with a refcount if found or allocated, or
 * ERR_PTR(-errno) on error.
 */
static struct rpdfs_block *lookup_or_alloc_block(struct rpdfs_block_info *binf, u64 bnr, gfp_t gfp)
{
	struct rpdfs_block *found;
	struct rpdfs_block *bk;
	bool retry;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		bk = alloc_block(gfp);
		if (!bk) {
			bk = ERR_PTR(-ENOMEM);
			goto out;
		}

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

static int send_block_write(struct rpdfs_fs_info *rfi, u64 bnr, u64 wcount, u8 mode,
			    struct page *data_page, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_block_write wr;
	struct rpdfs_net_message_desc md = {
		.type = RPDFS_MSG_BLOCK_WRITE,
		.ctl_buf = &wr,
		.ctl_size = sizeof(wr),
		.data_page = data_page,
		.data_size = data_page ? RPDFS_BLOCK_SIZE : 0,
	};
	u64 mver;
	int ret;

	wr.bnr = cpu_to_le64(bnr);
	wr.wcount = cpu_to_le64(wcount);
	wr.confirm_mode = mode;
	memzero_explicit(&wr._pad, sizeof(wr._pad));

	ret = rpdfs_map_bnr_to_addr(rfi, bnr, &addr, &mver);
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
 * See if we can send an explicit confirmation of a previously received
 * revocation.  We can't have active users that conflict with the mode.
 * If we sent a confirm mode with a flushing write then we have to wait
 * for its result to know if the server applied it and if we need to
 * resend its mode if it failed, or possibly send a further lesser
 * confirmation.
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
			bk->grant_mode = bk->confirm_mode;
			if (bk->grant_mode == RPDFS_CACHE_MODE_NONE) {
				bk->hnd.wcount = 0;
				bk->upd_meta = 0;
				bk->upd_data = 0;
			}
			bk->confirm_mode = RPDFS_CACHE_MODE_NULL;
			put_block(bk);
			try_remove_none(binf, bk);
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
 * The caller is an exclusive writer of the block so only we can dirty
 * it, but we use the bk seqlock to consistently serialize with retrying
 * readers.
 *
 * Returns a non-zero count of the size of the dirty list when it
 * dirties.
 */
static unsigned long make_dirty(struct rpdfs_block_info *binf, struct rpdfs_block *bk)
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

		write_sequnlock(&bk->seqlock);
	}

	return count;
}

/*
 * The block's dirty_head is on the caller's private dist_write at this
 * point.
 */
static void make_clean(struct rpdfs_block_info *binf, struct rpdfs_block *bk)
{
	struct rpdfs_dirty_list *dlist;

	BUG_ON(!bk->dirty);
	BUG_ON(list_empty(&bk->dirty_head));

	dlist = get_dlist(binf, bk->dirty_list_nr);
	write_seqlock(&dlist->seqlock);
	dlist->count--;
	write_sequnlock(&dlist->seqlock);

	write_seqlock(&bk->seqlock);
	bk->dirty = 0;
	bk->dirty_seq = 0;
	bk->dirty_list_nr = 0;
	list_del_init(&bk->dirty_head);
	write_sequnlock(&bk->seqlock);

	wake_up_all(&bk->waitq);
	put_block(bk);
}

/*
 * The caller got a write result for a block in a distributed write.  If
 * the write finished then walk the busy writes and complete them in
 * order as they complete.
 */
static void try_complete_write(struct rpdfs_block_info *binf, struct rpdfs_dist_write *wri)
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
			make_clean(binf, bk);
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
		write_sequnlock(&bk->seqlock);

		ret = send_block_write(rfi, bk->hnd.bnr, bk->hnd.wcount, mode,
				       bk->data_page, GFP_NOFS);
		BUG_ON(ret < 0);
	}

	/* drop our temp elevation, potentially completing the write */
	try_complete_write(binf, wri);
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
 * The flush work is responsible for gathering blocks in the dirty lists
 * into distributed writes.  This is racing with writers acquiring
 * handles on the blocks.  It advances the requested boundary as it
 * works, which prevents writers from acquiring dirty blocks within the
 * boundary.
 */
static void rpdfs_block_flush_work_fn(struct work_struct *work)
{
	struct rpdfs_flusher *flshr = container_of(work, struct rpdfs_flusher, work);
	struct rpdfs_block_info *binf = flshr->binf;
	struct rpdfs_dirty_list *dlist;
	struct rpdfs_dist_write *wri;
	struct rpdfs_block *bk;
	bool extended;
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

	/* gather dirty blocks up to the boundary from the dirty lists */
	extended = false;
	do {
		/* announce to writers that they should wait for dirty blocks within requested */
		if (extended) {
			write_seqlock(&flshr->seqlock);
			extend_boundary(&flshr->requested_bnd, &wri->bnd);
			write_sequnlock(&flshr->seqlock);
			extended = false;
		}

		for (i = 0; i < NR_DIRTY_LISTS; i++) {
			dlist = get_dlist(binf, i);

			/* gather blocks from dirty lists within the boundary */
			while_read_seqretry(&dlist->seqlock)
				bk = first_within_boundary(&dlist->list, &wri->bnd, i);
			if (bk) {
				write_seqlock(&dlist->seqlock);
				bk = first_within_boundary(&dlist->list, &wri->bnd, i);
				if (bk)
					list_splice_tail_init(&dlist->list, &flshr->dirty_list[i]);
				write_sequnlock(&dlist->seqlock);
			}

			/* add blocks to the write, possibly extending boundary */
			while ((bk = first_within_boundary(&flshr->dirty_list[i], &wri->bnd, i))) {

				/* wait for writers to drain and block on flush boundary */
				wait_event(bk->waitq, !block_has_writer(bk));

				write_seqlock(&bk->seqlock);
				bk->wri = wri;
				list_move_tail(&bk->dirty_head, &wri->dirty_list);
				write_sequnlock(&bk->seqlock);

				extended |= extend_boundary(&wri->bnd, &bk->dirty_bnd);
				wri->nr_blocks++;
			}
		}
	} while (extended);

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
 * Returns true if the block is dirty and its seq is within the
 * requested boundary that is being flushed.  This is the only place
 * that uses both the bk and flshr seqlock.
 */
static bool dirty_within_flush(struct rpdfs_block_info *binf, struct rpdfs_block *bk)
{
	struct rpdfs_flusher *flshr = binf->flshr;
	bool within = false;

	if (bk->dirty) {
		while_read_seqretry(&flshr->seqlock)
			within = bk->dirty_seq <= flshr->requested_bnd.seq[bk->dirty_list_nr];
	}

	return within;
}

/*
 * Returns true as the wait condition for the sleeping acquire caller if
 * it should retry the acquire.  This must match all the conditions in
 * the _acquire loop that do anything other than block.  There are some
 * cases (like returning when _NONBLOCK_) that don't need to be tested
 * here because they would have returned instead of blocking and calling
 * this.
 */
static bool should_try_acquire(struct rpdfs_block_info *binf, struct rpdfs_block *bk,
			       u8 mode, rbaf_t rbaf)
{
	bool try;

	while_read_seqretry(&bk->seqlock) {
		try = (bk->error && !(rbaf & RBAF_OVERWRITE)) ||
		      (((mode > bk->grant_mode) || (mode > bk->confirm_mode)) &&
		       (mode > bk->request_mode)) ||
		      ((mode <= bk->grant_mode) &&
		       (!bk->confirm_mode || mode <= bk->confirm_mode) &&
		       (!(rbaf & RBAF_WRITE) || !dirty_within_flush(binf, bk)) &&
		       modes_compatible(readers_writer_mode(bk), mode));
	}

	return try;
}

/*
 * Acquire a read or write handle to a block.  Returns 0 on success with
 * the handle pointing at the block.  _block_release() must be called on
 * the handle when the caller is done.
 *
 * With no flags, a readable handle is returned.  _WRITE acquires a
 * write handle which may be dirtied and will exclude all other handles.
 */
int rpdfs_block_acquire(struct rpdfs_fs_info *rfi, u64 bnr, struct rpdfs_block_handle **hnd_ret,
			rbaf_t rbaf)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_block *bk = NULL;
	bool need_data;
	u8 mode;
	int ret;

	if (WARN_ON_ONCE((rbaf & RBAF_OVERWRITE) && !(rbaf & RBAF_WRITE)) ||
	    WARN_ON_ONCE(*hnd_ret != NULL)) {
		ret = -EINVAL;
		goto out;
	}

	bk = lookup_or_alloc_block(binf, bnr, GFP_NOFS);
	if (IS_ERR(bk)) {
		ret = PTR_ERR(bk);
		goto out;
	}

	mode = mode_from_rbaf(rbaf);
	need_data = !(rbaf & RBAF_OVERWRITE);
	for (;;) {
		ret = rpdfs_net_preload(rfi, GFP_NOFS);
		if (ret < 0)
			goto out;

		write_seqlock(&bk->seqlock);

		ret = 0;
		if (bk->error && !(rbaf & RBAF_OVERWRITE)) {
			/* return a read error */
			ret = bk->error;

		} else if ((mode > bk->grant_mode) || (
			    bk->confirm_mode && mode > bk->confirm_mode)) {
			/* don't have sufficient mode */
			if (rbaf & RBAF_NONBLOCK_MODE) {
				ret = -EAGAIN;

			} else if (mode > bk->request_mode) {
				/* request elevated mode, either read or bare request */
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
			}

		} else if ((rbaf & RBAF_WRITE) && dirty_within_flush(binf, bk)) {
			/* requested flush of this block blocks writer */
			if (rbaf & RBAF_NONBLOCK_FLUSH)
				ret = -EAGAIN;

		} else if (modes_compatible(readers_writer_mode(bk), mode)) {
			/* rbaf mode compatible with other handles, acquire */
			if (rbaf & RBAF_WRITE)
				bk->writer = 1;
			else
				bk->readers++;

			if (rbaf & RBAF_OVERWRITE)
				bk->error = 0;

			*hnd_ret = &bk->hnd;
		}
		write_sequnlock(&bk->seqlock);

		rpdfs_net_preload_end(rfi);

		if (*hnd_ret != NULL || ret < 0)
			goto out;

		rpdfs_prd_rfi(rfi, "acquire mode %u rbaf %x wait "RBF,
			      mode, rbaf, RBA(bk));

		ret = wait_event_interruptible(bk->waitq, should_try_acquire(binf, bk, mode, rbaf));
		if (ret < 0)
			goto out;
	}
out:
	if (ret < 0)
		put_block(bk);
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
	int send_ret;

	if (*hnd == NULL)
		return;
	bk = container_of(*hnd, struct rpdfs_block, hnd);
	*hnd = NULL;

	send_ret = rpdfs_net_preload(rfi, GFP_NOFS);
	write_seqlock(&bk->seqlock);

	/* XXX not great that we're presuming the intent of the release :/ */
	if (bk->writer) {
		bk->writer = 0;
		if (bk->dirty && bk->grant_mode < RPDFS_CACHE_MODE_WRITE)
			flush = true;
	} else {
		bk->readers--;
	}

	if (send_ret == 0) {
		send_ret = try_send_confirm(rfi, bk);
		rpdfs_net_preload_end(rfi);
	}

	write_sequnlock(&bk->seqlock);

	if (flush)
		queue_block_flush(binf, bk);

	wake_up_all(&bk->waitq);
	put_block(bk);

	/*
	 * This failure is not a failure to release, it's a failure to
	 * participate in the cache coherency protocol.  We'd go
	 * read-only, abort, etc.
	 */
	BUG_ON(send_ret < 0);
}

/* iteration over blocks in caller's list, asking them for the block at each pos */
#define for_each_list_block(bk, pos, list, entry_handle_fn) \
        for (pos = (list)->next; \
             !list_is_head(pos, (list)) && \
		({ bk = container_of(entry_handle_fn(pos), struct rpdfs_block, hnd); true; }); \
             pos = pos->next)

/*
 * The caller has exclusive write refs on a set of blocks and is going
 * to modify them.  We make sure they're all marked dirty and that their
 * dirty boundaries cover each other so that they're written as one
 * distributed write.
 */
void rpdfs_block_make_dirty(struct rpdfs_fs_info *rfi, struct list_head *list,
			    rpdfs_block_entry_handle_fn_t entry_handle_fn)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_dirty_boundary bnd = {{0,}};
	struct rpdfs_block *bk;
	struct list_head *pos;
	bool flush = false;

	for_each_list_block(bk, pos, list, entry_handle_fn) {
		if (make_dirty(binf, bk) > DIRTY_BLOCK_LIMIT)
			flush = true;

		extend_boundary(&bnd, &bk->dirty_bnd);
	}

	for_each_list_block(bk, pos, list, entry_handle_fn)
		extend_boundary(&bk->dirty_bnd, &bnd);

	if (flush)
		queue_boundary_flush(binf, &bnd);
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
	put_block(bk);

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

	/* XXX not sure how to test that server didn't mess up req->set->active */
	write_seqlock(&bk->seqlock);
	if (rr->grant_mode && rr->grant_mode > bk->request_mode) {
		ret = -EPROTO;
	} else {
		bk->hnd.wcount = le64_to_cpu(rr->wcount);
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
			if (bk->request_mode == rr->grant_mode) {
				bk->request_mode = RPDFS_CACHE_MODE_NULL;
				put_block(bk);
			}
		}

		ret = 0;
	}
	write_sequnlock(&bk->seqlock);

	wake_up_all(&bk->waitq);
	put_block(bk);
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
			bk->hnd.wcount = 0;
			bk->upd_meta = 0;
			bk->upd_data = 0;
		}
		if (bk->write_confirm_mode == bk->confirm_mode) {
			bk->confirm_mode = RPDFS_CACHE_MODE_NULL;
			put_block(bk);
		}
		bk->write_confirm_mode = RPDFS_CACHE_MODE_NULL;

		/* can have to send second lesser confirm */
		ret = try_send_confirm(rfi, bk);
		try_remove_none(binf, bk);
	}
	write_sequnlock(&bk->seqlock);

	rpdfs_net_preload_end(rfi);

	try_complete_write(binf, bk->wri);
put:
	put_block(bk);
out:
	return ret;
}

/*
 * We pin a block when we send a request.  The server may take a while
 * to process the request, but it eventually will.  We're guaranteed a
 * grant response.  We can send back to back requests for increasing
 * modes.  We may get back to back grants, or we may get a grant for the
 * highest mode by the time the server processes the request.
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

	/* can't get grant we haven't requested, and grants should only elevate */
	write_seqlock(&bk->seqlock);
	if (cm->mode > bk->request_mode || cm->mode < bk->grant_mode) {
		ret = -EPROTO;
	} else {
		bk->grant_mode = cm->mode;
		if (bk->request_mode == cm->mode) {
			bk->request_mode = RPDFS_CACHE_MODE_NULL;
			put_block(bk);
		}
		ret = 0;
	}
	write_sequnlock(&bk->seqlock);
	wake_up_all(&bk->waitq);
	put_block(bk);
out:
	return ret;
}

/*
 * Revocations are only received for previously granted modes.  They
 * should only decrease our granted mode.  We can receive back to back
 * revocations of decreasing modes (write->read, read->none).
 *
 * However, we can free clean blocks under memory pressure.  Then we can
 * allocate a new block that a caller's trying to acquire.  We can
 * receive revocations for the old block number that was freed at any
 * point in that life cycle.  We need to be forgiving of the state of
 * blocks when we receive a revocation.
 *
 * We must send a confirmation for every revoke we receive, and we have
 * to wait until users of the revoked mode (including dirty blocks) have
 * finished.  We record that we need to confirm the mode, and this
 * pending confirmation stops future incompatible users.
 */
static int recv_block_revoke_mode(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_block_info *binf = RPDFS_BINF(rfi);
	struct rpdfs_msg_cache_mode *cm = md->ctl_buf;
	u64 bnr = le64_to_cpu(cm->bnr);
	struct rpdfs_block *bk;
	bool flush;
	int ret;

	bk = lookup_block(binf, bnr);
	if (!bk) {
		/* no block, we shrank, send immediate confirmation */
		ret = send_block_cache_mode(rfi, bnr, RPDFS_MSG_BLOCK_CONFIRM_MODE,
					    RPDFS_CACHE_MODE_NONE, GFP_NOFS);
		goto out;
	}

	ret = rpdfs_net_preload(rfi, GFP_NOFS);
	if (ret < 0)
		goto put;

	write_seqlock(&bk->seqlock);
	if (!bk->confirm_mode)
		get_block(bk);
	bk->confirm_mode = cm->mode;
	flush = bk->dirty;
	ret = try_send_confirm(rfi, bk);
	write_sequnlock(&bk->seqlock);

	rpdfs_net_preload_end(rfi);

	if (flush)
		queue_block_flush(binf, bk);

	if (ret == 0)
		wake_up_all(&bk->waitq);
put:
	put_block(bk);
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
static void remove_isolated_list(struct rpdfs_block_info *binf, struct list_head *isolated)
{
	struct rpdfs_block *bk;
	struct rpdfs_block *bk__;

	list_for_each_entry_safe(bk, bk__, isolated, lru_head) {
		rhashtable_remove_fast(&binf->block_ht, &bk->rhead, block_ht_params);
		put_block(bk);
	}
}

static unsigned long rpdfs_block_scan_objects(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct rpdfs_block_info *binf = shrinker->private_data;
	LIST_HEAD(isolated);
	unsigned long freed;

	freed = list_lru_shrink_walk(&binf->lru, sc, scoutfs_block_isolate, &isolated);
	remove_isolated_list(binf, &isolated);
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
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_REVOKE_MODE, recv_block_revoke_mode);
	if (ret < 0)
		goto out;

	ret = rhashtable_init(&binf->block_ht, &block_ht_params);
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

	if (bk->request_mode)
		put_block(bk);
	if (bk->confirm_mode)
		put_block(bk);

	/* this can pull off of dirty lists, the flusher, or distributed writes */
	if (bk->dirty) {
		bk->dirty = 0;
		if (!list_empty(&bk->dirty_head))
			list_del_init(&bk->dirty_head);
		put_block(bk);
	}

	if (get_block_removal(bk, 0)) {
		list_lru_del_obj(&binf->lru, &bk->lru_head);
		put_block(bk);
	}
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
		shrinker_free(binf->shrinker);

		cancel_work_sync(&binf->flshr->work);

		list_splice_init(&binf->flshr->idle_writes, &binf->flshr->busy_writes);
		list_for_each_entry(wri, &binf->flshr->busy_writes, head)
			cancel_work_sync(&wri->work);

		destroy_workqueue(binf->workq);

		rhashtable_free_and_destroy(&binf->block_ht, free_and_destroy_block, binf);
		list_lru_destroy(&binf->lru);
		free_dist_writes(&binf->flshr->idle_writes, &binf->flshr->busy_writes);
		kfree(binf->flshr);
		kfree(binf);

		SET_RPDFS_BINF(rfi, NULL);
	}
}
