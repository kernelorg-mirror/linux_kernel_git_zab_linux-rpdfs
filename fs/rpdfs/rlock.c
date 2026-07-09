/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/seqlock.h>
#include <linux/xxhash.h>
#include "rlock.h"
#include "format-msg.h"
#include "ht.h"
#include "map.h"
#include "pr.h"
#include "rpdfs_trace.h"
#include "seqlock.h"

/*
 * rlocks -- "remote" locks (but also 'r' looks like "rpdfs" :))
 *
 * rlocks are used to synchronize between clients.  Typically holders
 * read and write blocks but rlocks can (XXX NYI) carry additional
 * state.
 */

/*
 * XXX:
 *  - add trace points?
 *  (later)
 *  - add shrinker
 */

struct rpdfs_rlock {
	struct rpdfs_ht_entry hte;
	struct rpdfs_rlock_key key;
	seqlock_t seqlock;
	wait_queue_head_t waitq;
	int holders[RPDFS_RLOCK_MODE__INVALID];
	u32 rv;
	u8 grant_mode;
	u8 request_mode;
	u8 revoke_mode;
};

static const struct rpdfs_rlock nil_rlock;

static inline const struct rpdfs_rlock *rlock_or_nil(struct rpdfs_rlock *rlock)
{
	return IS_ERR_OR_NULL(rlock) ? &nil_rlock : rlock;
}

#define __TR_RLOCK_ARGS(rlock) \
	(rlock)->grant_mode, (rlock)->request_mode, (rlock)->revoke_mode, \
	(rlock)->holders[RPDFS_RLOCK_MODE_SH_RD], (rlock)->holders[RPDFS_RLOCK_MODE_EX_WR]
#define TR_RLOCK(rlock) __TR_RLOCK_ARGS(rlock_or_nil(rlock))

#define KYF		"%llu.%llu"
#define KYA(key)	(key)->k[0], (key)->k[1]

#define RLF \
	KYF" gr %u rq %u rv %u rd %d wr %d"
#define RLA(rlock) \
	KYA(&(rlock)->key), (rlock)->grant_mode, (rlock)->request_mode, (rlock)->revoke_mode, \
	(rlock)->holders[RPDFS_RLOCK_MODE_SH_RD], (rlock)->holders[RPDFS_RLOCK_MODE_EX_WR]

struct rpdfs_rlock_info {
	struct rhashtable ht;
	bool have_ht;
};

static struct rpdfs_rlock_info *RPDFS_RLINF(struct rpdfs_fs_info *rfi)
{
	return rfi->rlock_info;
}

static void SET_RPDFS_RLINF(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_info *rlinf)
{
	rfi->rlock_info = rlinf;
}

static const struct rhashtable_params rlock_ht_params = {
	.key_len	= sizeof_field(struct rpdfs_rlock, key),
	.key_offset	= offsetof(struct rpdfs_rlock, key),
	.head_offset	= offsetof(struct rpdfs_rlock, hte.rhead),
};

static struct rpdfs_rlock *get_rlock(struct rpdfs_rlock_info *rlinf, struct rpdfs_rlock_key *key)
{
	struct rpdfs_ht_entry *hte;

	hte = rpdfs_ht_get(&rlinf->ht, key, rlock_ht_params);
	if (hte)
		return container_of(hte, struct rpdfs_rlock, hte);
	else
		return NULL;
}

static struct rpdfs_rlock *inc_rlock(struct rpdfs_rlock *rlock)
{
	rpdfs_ht_inc(&rlock->hte);
	return rlock;
}

/*
 * Return the most exclusive mode that's held.  We don't test the low
 * modes that have no holders and return NULL if we get that far.
 */
static u8 holders_mode(struct rpdfs_rlock *rlock)
{
	u8 mode;

	for (mode = RPDFS_RLOCK_MODE_EX_WR; mode >= RPDFS_RLOCK_MODE_SH_RD; mode--) {
		if (rlock->holders[mode])
			return mode;
	}

	return RPDFS_RLOCK_MODE_NULL;
}

static bool can_free_rlock(struct rpdfs_rlock *rlock)
{
	return rlock->grant_mode <= RPDFS_RLOCK_MODE_NONE &&
	       rlock->request_mode <= RPDFS_RLOCK_MODE_NONE &&
	       rlock->revoke_mode <= RPDFS_RLOCK_MODE_NONE &&
	       mem_is_zero(rlock->holders, sizeof(rlock->holders));
}

static void put_rlock_try_free(struct rpdfs_rlock_info *rlinf, struct rpdfs_rlock *rlock,
			       bool try_remove)
{
	if (IS_ERR_OR_NULL(rlock))
		return;

	if (rpdfs_ht_put(&rlinf->ht, &rlock->hte, rlock_ht_params, try_remove)) {
		rpdfs_prd("freeing "RLF, RLA(rlock));
		kfree_rcu(rlock, hte.rcu);
	}
}

static void put_rlock(struct rpdfs_rlock_info *rlinf, struct rpdfs_rlock *rlock)
{
	if (IS_ERR_OR_NULL(rlock))
		return;

	rcu_read_lock();
	write_seqlock(&rlock->seqlock);
	put_rlock_try_free(rlinf, rlock, can_free_rlock(rlock));
	write_sequnlock(&rlock->seqlock);
	rcu_read_unlock();
}

static struct rpdfs_rlock *get_or_alloc_rlock(struct rpdfs_rlock_info *rlinf,
					      struct rpdfs_rlock_key *key, gfp_t gfp)
{
	struct rpdfs_ht_entry *hte;
	struct rpdfs_rlock *rlock;

	rlock = get_rlock(rlinf, key);
	if (rlock)
		goto out;

	rlock = kzalloc(sizeof(struct rpdfs_rlock), gfp);
	if (!rlock) {
		rlock = ERR_PTR(-ENOMEM);
		goto out;
	}

	rlock->key = *key;
	seqlock_init(&rlock->seqlock);
	init_waitqueue_head(&rlock->waitq);

	hte = rpdfs_ht_insert(&rlinf->ht, &rlock->hte, rlock_ht_params);
	if (hte != &rlock->hte) {
		put_rlock(rlinf, rlock);
		rlock = container_of(hte, struct rpdfs_rlock, hte);
	}
out:
	return rlock;
}

/*
 * Returns true if a local lock mode is compatible with the granted
 * mode.  This is different than the exclusion that exists between
 * clients.  Here a local READ is compatible with WRITE.
 */
static bool hold_compatible(u8 local, u8 remote)
{
	return remote >= local;
}

/*
 * Local holders can lock a mode when it is compatible with the mode
 * we've been granted and, if there's a revoke pending, the mode that
 * remains after revocation.
 */
static bool can_lock_mode(struct rpdfs_rlock *rlock, u8 mode)
{
	return hold_compatible(mode, rlock->grant_mode) &&
	       (!rlock->revoke_mode || hold_compatible(mode, rlock->revoke_mode));
}

static bool can_lock_mode_cond(struct rpdfs_rlock *rlock, u8 mode)
{
	bool can;

	while_read_seqretry(&rlock->seqlock)
		can = can_lock_mode(rlock, mode);

	return can;
}

static int send_rlock_message(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key,
			      u8 type, u8 mode, gfp_t gfp)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_msg_rlock rlm = {
		.key = *key,
		.mode = mode,
	};
	struct rpdfs_net_message_desc md = {
		.type = type,
		.ctl_buf = &rlm,
		.ctl_size = sizeof(rlm),
	};
	u64 mver;
	u32 rv;
	int ret;

	/* we could cache the rv in the rlock, or cache the mapping, etc */
	rv = xxh32(key, sizeof(struct rpdfs_rlock_key), 0);

	ret = rpdfs_map_rv_to_addr(rfi, rv, &addr, &mver);
	if (ret == 0)
		ret = rpdfs_net_send(rfi, &addr, &md, gfp);
	BUG_ON(ret < 0);

	return ret;
}

/*
 * Send a confirmation of a revoked mode once local holders are compatible
 * with the resulting mode.  The caller holds the seqlock and has
 * preloaded the send.
 */
static int try_send_confirm(struct rpdfs_fs_info *rfi, struct rpdfs_rlock *rlock)
{
	int ret = 0;

	if (rlock->revoke_mode && hold_compatible(holders_mode(rlock), rlock->revoke_mode)) {
		ret = send_rlock_message(rfi, &rlock->key, RPDFS_MSG_RLOCK_CONFIRM,
					  rlock->revoke_mode, GFP_NOWAIT);
		if (ret == 0) {
			rlock->grant_mode = rlock->revoke_mode;
			rlock->revoke_mode = RPDFS_RLOCK_MODE_NULL;
			rpdfs_prd("sent confirm "RLF, RLA(rlock));
		}
	}

	return ret;
}

/*
 * Grants are sent in response to request.  We don't free rlocks while
 * requests are in flight.  We can send back to back requests for
 * increasing modes.  We'll get grants for each requested mode.
 */
static int recv_rlock_grant(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_rlock_info *rlinf = RPDFS_RLINF(rfi);
	struct rpdfs_msg_rlock *rlm = md->ctl_buf;
	struct rpdfs_rlock *rlock;
	int ret;

	rlock = get_rlock(rlinf, &rlm->key);
	if (!rlock) {
		/* we should have pinned, server sent unrequested grant? */
		ret = -EPROTO;
		goto out;
	}

	rpdfs_prd("mode %u "RLF, rlm->mode, RLA(rlock));

	/* grants must be elevating in response to requests */
	write_seqlock(&rlock->seqlock);
	if (!rlock->request_mode || rlm->mode > rlock->request_mode ||
	    rlm->mode < rlock->grant_mode) {
		ret = -EPROTO;
	} else {
		rlock->grant_mode = rlm->mode;
		if (rlock->request_mode == rlm->mode)
			rlock->request_mode = RPDFS_RLOCK_MODE_NULL;
		ret = 0;
	}
	write_sequnlock(&rlock->seqlock);
	wake_up_all(&rlock->waitq);
	put_rlock(rlinf, rlock);
out:
	return ret;
}

/*
 * Unsolicited revocations are sent for previously granted modes.  The
 * server strictly only sends one revoke at a time.  Typically we'll
 * have the rlock with the old granted mode.  We'll confirm the
 * revocation once incompatible holders have finished.
 *
 * But we might have released an idle rlock as the server was sending
 * the revocation.  In this case we no longer have the rlock and send an
 * immediate confirm.
 */
static int recv_rlock_revoke(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_rlock_info *rlinf = RPDFS_RLINF(rfi);
	struct rpdfs_msg_rlock *rlm = md->ctl_buf;
	struct rpdfs_rlock *rlock = NULL;
	bool preloaded;
	int ret;

	rlock = get_rlock(rlinf, &rlm->key);
	if (!rlock) {
		/* no rlock, we released, send immediate confirm */
		ret = send_rlock_message(rfi, &rlm->key, RPDFS_MSG_RLOCK_CONFIRM,
					  rlm->mode, GFP_NOFS);
		if (ret == 0)
			rpdfs_prd("no rlock key "KYF" mode %u", KYA(&rlm->key), rlm->mode);
		goto out;
	}

	rpdfs_prd("mode %u "RLF, rlm->mode, RLA(rlock));

	preloaded = rpdfs_net_preload(rfi, GFP_NOFS) == 0;

	write_seqlock(&rlock->seqlock);
	if (!rlock->revoke_mode) {
		rlock->revoke_mode = rlm->mode;
		ret = try_send_confirm(rfi, rlock);
	} else {
		ret = -EPROTO;
	}
	write_sequnlock(&rlock->seqlock);

	if (preloaded)
		rpdfs_net_preload_end(rfi);

	if (ret == 0)
		wake_up_all(&rlock->waitq);
out:
	put_rlock(rlinf, rlock);
	return ret;
}

/*
 * Local holders must be compatible with the granted mode, but not
 * necessarily with each other.  Callers can safely have concurrent read
 * and write hold of granted write rlock if, say, they have their own
 * locking of different structures covered by the key.
 */
static void mod_holders(struct rpdfs_rlock *rlock, u8 mode, long delta)
{
	BUG_ON(mode <= RPDFS_RLOCK_MODE_NONE); /* no holders of low modes */
	BUG_ON(mode >= ARRAY_SIZE(rlock->holders)); /* bad mode array deref */

	rlock->holders[mode] += delta;

	/* catch basic refcounting bugs */
	BUG_ON(rlock->holders[mode] < 0);
	BUG_ON(delta > 0 && rlock->holders[mode] == 0);
}

int rpdfs_rlock_lock(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key, u8 mode,
		     struct rpdfs_rlock_hold *hold)
{
	struct rpdfs_rlock_info *rlinf = RPDFS_RLINF(rfi);
	struct rpdfs_rlock *rlock = NULL;
	int ret;

	trace_rpdfs_rlock_lock_enter(rfi, key, mode);

	if (WARN_ON_ONCE(mode <= RPDFS_RLOCK_MODE_NONE) ||
	    WARN_ON_ONCE(hold->rlock || hold->mode)) {
		ret = -EINVAL;
		goto out;
	}

	rlock = get_or_alloc_rlock(rlinf, key, GFP_NOFS);
	if (IS_ERR(rlock)) {
		ret = PTR_ERR(rlock);
		goto out;
	}

	for (;;) {
		ret = rpdfs_net_preload(rfi, GFP_NOFS);
		if (ret < 0)
			break;

		write_seqlock(&rlock->seqlock);
		if (!can_lock_mode(rlock, mode)) {
			if (!rlock->request_mode) {
				ret = send_rlock_message(rfi, key, RPDFS_MSG_RLOCK_REQUEST,
							  mode, GFP_NOWAIT);
				if (ret == 0)
					rlock->request_mode = mode;
			}
		} else {
			mod_holders(rlock, mode, 1);
			hold->rlock = inc_rlock(rlock);
			hold->mode = mode;
		}
		write_sequnlock(&rlock->seqlock);

		rpdfs_net_preload_end(rfi);

		if (hold->mode || ret < 0)
			break;

		trace_rpdfs_rlock_lock_wait(rfi, key, mode, TR_RLOCK(rlock));

		rpdfs_prd("waiting mode %u "RLF, mode, RLA(rlock));
		ret = wait_event_interruptible(rlock->waitq, can_lock_mode_cond(rlock, mode));
		if (ret < 0)
			break;
	}

out:
	if (!IS_ERR_OR_NULL(rlock))
		rpdfs_prd("mode %u ret %d "RLF, mode, ret, RLA(rlock));
	else
		rpdfs_prd("key "KYF" mode %u ret %d", KYA(key), mode, ret);

	trace_rpdfs_rlock_lock_exit(rfi, key, mode, ret, TR_RLOCK(rlock));

	put_rlock(rlinf, rlock);
	return ret;
}

void rpdfs_rlock_unlock(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_hold *hold)
{
	struct rpdfs_rlock_info *rlinf = RPDFS_RLINF(rfi);
	struct rpdfs_rlock *rlock;
	bool preloaded;
	int ret;

	if (IS_ERR_OR_NULL(hold) || !hold->rlock)
		return;

	rlock = hold->rlock;
	preloaded = rpdfs_net_preload(rfi, GFP_NOFS) == 0;

	write_seqlock(&rlock->seqlock);
	mod_holders(rlock, hold->mode, -1);
	ret = try_send_confirm(rfi, rlock);
	write_sequnlock(&rlock->seqlock);

	if (preloaded)
		rpdfs_net_preload_end(rfi);

	rpdfs_prd("mode %u "RLF, hold->mode, RLA(rlock));
	wake_up_all(&rlock->waitq);
	put_rlock(rlinf, rlock);

	rpdfs_rlock_init_hold(hold);

	/*
	 * This is a failure to participate in the rlock protocol.
	 * We'd go read-only, abort, etc.
	 */
	BUG_ON(ret < 0);
}

int rpdfs_rlock_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_rlock_info *rlinf;
	int ret;

	rlinf = kzalloc(sizeof(struct rpdfs_rlock_info), GFP_KERNEL);
	if (!rlinf) {
		ret = -ENOMEM;
		goto out;
	}

	ret = rhashtable_init(&rlinf->ht, &rlock_ht_params);
	if (ret < 0)
		goto out;
	rlinf->have_ht = true;

	ret = rpdfs_net_register_recv(rfi, RPDFS_MSG_RLOCK_GRANT, recv_rlock_grant) ?:
	      rpdfs_net_register_recv(rfi, RPDFS_MSG_RLOCK_REVOKE, recv_rlock_revoke);
	if (ret < 0)
		goto out;

	SET_RPDFS_RLINF(rfi, rlinf);
	ret = 0;
out:
	if (ret < 0)
		rpdfs_rlock_destroy(rfi);
	return ret;
}

/*
 * The ht destroying caller has removed from the ht, this just needs to
 * put the refcount.
 */
static void free_ht_rlock(void *ptr, void *arg)
{
	struct rpdfs_rlock_info *rlinf = arg;
	struct rpdfs_rlock *rlock = ptr;

	put_rlock_try_free(rlinf, rlock, false);
}

void rpdfs_rlock_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_rlock_info *rlinf = RPDFS_RLINF(rfi);

	if (rlinf) {
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_RLOCK_GRANT, recv_rlock_grant);
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_RLOCK_REVOKE, recv_rlock_revoke);

		if (rlinf->have_ht)
			rhashtable_free_and_destroy(&rlinf->ht, free_ht_rlock, rlinf);

		kfree(rlinf);

		SET_RPDFS_RLINF(rfi, NULL);
	}
}
