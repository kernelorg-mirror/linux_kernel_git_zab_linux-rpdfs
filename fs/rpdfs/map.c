/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/xxhash.h>

#include "map.h"
#include "net.h"
#include "seqlock.h"
#include "super.h"

struct addr_map {
	struct rcu_head rcu;
	unsigned long count;
	struct map_entry {
		struct rpdfs_net_transport_addr addr;
		u32 rv;
	} entries[];
};

struct rpdfs_map_info {
	seqlock_t seqlock;
	u64 mver;
	struct addr_map __rcu *amap;
};

static struct rpdfs_map_info *RPDFS_MINF(struct rpdfs_fs_info *rfi)
{
	return rfi->map_info;
}

static void SET_RPDFS_MINF(struct rpdfs_fs_info *rfi, struct rpdfs_map_info *minf)
{
	rfi->map_info = minf;
}

/*
 * This is incrementally building up the map so the gradual allocation
 * of the full map is a little clumsy.  This will be more coherent when
 * it's acting on complete received maps.
 */
int rpdfs_map_add_addr(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	struct addr_map *next;
	unsigned long count;
	unsigned int seq;
	u64 mver;
	int ret;

	do {
		rcu_read_lock();
		do {
			seq = read_seqbegin(&minf->seqlock);
			mver = minf->mver;
			amap = rcu_dereference(minf->amap);
			count = amap ? amap->count + 1 : 1;
		} while (read_seqretry(&minf->seqlock, seq));
		rcu_read_unlock();

		next = kvmalloc(offsetof(struct addr_map, entries[count]), GFP_NOFS);
		if (!next) {
			ret = -ENOMEM;
			goto out;
		}

		next->count = count;
		next->entries[count - 1].addr = *addr;
		next->entries[count - 1].rv = xxh32(addr, sizeof(struct rpdfs_net_transport_addr),
						    0);

		write_seqlock(&minf->seqlock);
		if (minf->mver == mver) {
			if (amap) {
				memcpy(next->entries, amap->entries,
				       amap->count * sizeof(struct map_entry));
				kvfree_rcu(amap, rcu);
			}
			rcu_assign_pointer(minf->amap, next);
			next = NULL;
			minf->mver++;
		}
		write_sequnlock(&minf->seqlock);

		if (next)
			kvfree(next);
	} while (next);

	ret = 0;
out:
	return ret;
}

int rpdfs_map_nr_devds(struct rpdfs_fs_info *rfi, u64 *mver)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	unsigned int seq;
	int ret;

	rcu_read_lock();
	do {
		seq = read_seqbegin(&minf->seqlock);
		amap = rcu_dereference(minf->amap);
		*mver = minf->mver;
		if (amap)
			ret = amap->count;
		else
			ret = -ENOENT;
	} while (read_seqretry(&minf->seqlock, seq));
	rcu_read_unlock();

	return ret;
}

int rpdfs_map_bnr_to_addr(struct rpdfs_fs_info *rfi, u64 bnr,
			  struct rpdfs_net_transport_addr *addr, u64 *mver)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	unsigned int seq;
	int ret;

	rcu_read_lock();
	do {
		seq = read_seqbegin(&minf->seqlock);
		amap = rcu_dereference(minf->amap);
		*mver = minf->mver;
		if (amap) {
			*addr = amap->entries[bnr % amap->count].addr;
			ret = 0;
		} else {
			ret = -ENOENT;
		}
	} while (read_seqretry(&minf->seqlock, seq));
	rcu_read_unlock();

	return ret;
}

/*
 * Rather than hash an object's identity with every destination, we
 * calculate strong hashes for each and then find the score by
 * multiplying the hash values.  It's still consistent, reasonably
 * distributed, and much faster (and could be vectored).
 */
static u32 rendezvous(struct addr_map *amap, u32 rv)
{
	unsigned long i;
	u32 greatest = 0;
	u32 score;
	u32 r = 0;

	for (i = 0; i < amap->count; i++) {
		score = rv * amap->entries[i].rv;
		if (score > greatest) {
			greatest = score;
			r = i;
		}
	}

	return r;
}

int rpdfs_map_rv_to_addr(struct rpdfs_fs_info *rfi, u32 rv,
			 struct rpdfs_net_transport_addr *addr, u64 *mver)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	unsigned int seq;
	unsigned long r;
	int ret;

	rcu_read_lock();
	do {
		seq = read_seqbegin(&minf->seqlock);
		amap = rcu_dereference(minf->amap);
		*mver = minf->mver;
		if (amap) {
			r = rendezvous(amap, rv);
			*addr = amap->entries[r].addr;
			ret = 0;
		} else {
			ret = -ENOENT;
		}
	} while (read_seqretry(&minf->seqlock, seq));
	rcu_read_unlock();

	return ret;
}

/*
 * Ideally callers can cache the rv calculation in their object that's
 * consistently being sent with the same identifier.  This is for when
 * they can't.
 */
int rpdfs_map_hash_rv_to_addr(struct rpdfs_fs_info *rfi, void *data, size_t len,
			      struct rpdfs_net_transport_addr *addr, u64 *mver)
{
	return rpdfs_map_rv_to_addr(rfi, xxh32(data, len, 0), addr, mver);
}

int rpdfs_map_nth_addr(struct rpdfs_fs_info *rfi, unsigned int n,
		       struct rpdfs_net_transport_addr *addr, u64 *mver)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	unsigned int seq;
	int ret;

	rcu_read_lock();
	do {
		seq = read_seqbegin(&minf->seqlock);
		amap = rcu_dereference(minf->amap);
		*mver = minf->mver;
		if (amap && n <= amap->count) {
			*addr = amap->entries[n].addr;
			ret = 0;
		} else {
			ret = -ENOENT;
		}
	} while (read_seqretry(&minf->seqlock, seq));
	rcu_read_unlock();

	return ret;
}

/*
 * bnr is the first block in the stripe.  Give the caller the stripe
 * number and the number of stripes.
 */
int rpdfs_map_alloc_stripe_geom(struct rpdfs_fs_info *rfi, u64 bnr, unsigned long *this_stripe,
				unsigned long *stripes, u64 *mver)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;
	u32 rem;
	int ret;

	rcu_read_lock();
	while_read_seqretry(&minf->seqlock) {
		amap = rcu_dereference(minf->amap);
		*mver = minf->mver;
		if (amap && amap->count > 0) {
			div_u64_rem(bnr, amap->count, &rem);
			*this_stripe = rem;
			*stripes = amap->count;
			ret = 0;
		} else {
			ret = -ENOENT;
		}
	}
	rcu_read_unlock();

	return ret;
}

/*
 * Just a quick iterator to get all the addresses in a version of the
 * address map, returning errors if the map changes.  This is serving
 * callers that don't store their state derived from the maps.  In a
 * more complete world they'd be operating on the difference between
 * their state and the maps, not just iterating over the map.
 */
static bool addr_iter(struct rpdfs_map_info *minf, u64 *mver, unsigned long *i,
		      struct rpdfs_net_transport_addr *addr, int *ret)
{
	struct addr_map *amap;
	unsigned seq;
	bool got;

	rcu_read_lock();
	do {
		*ret = 0;
		got = false;

		seq = read_seqbegin(&minf->seqlock);
		amap = rcu_dereference(minf->amap);
		if (!amap) {
			*ret = -ENOENT;
		} else {
			if (*mver > 0 && *mver != minf->mver) {
				*ret = -EREMCHG;
			} else {
				if (*mver == 0) {
					*mver = minf->mver;
					*i = 0;
				}
				if (*i < amap->count) {
					*addr = amap->entries[*i].addr;
					got = true;
				}
			}
		}
	} while (read_seqretry(&minf->seqlock, seq));
	rcu_read_unlock();

	if (got)
		(*i)++;

	return got;
}

/*
 * Called at mount to initially connect to enough servers (mapd and
 * devd) to be operational.  For now we just connect to the devds that
 * were supplied as paramaters.
 */
int rpdfs_map_connect(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct rpdfs_net_transport_addr addr;
	unsigned long i;
	u64 qver = 0;
	int ret;

	while (addr_iter(minf, &qver, &i, &addr, &ret)) {
		ret = rpdfs_net_connect(rfi, &addr, i - 1, qver);
		if (ret < 0)
			break;
	}

	return ret;
}

/*
 * XXX we're just disconnecting from peers today.  An orderly shutdown
 * should see us remove ourselves from the maps, then shutdown once we
 * receive the updated maps that no longer include us.  We could
 * forcefully shutdown if that update/wait fails.
 */
void rpdfs_map_disconnect(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct rpdfs_net_transport_addr addr;
	unsigned long i;
	u64 mver = 0;
	int ret;

	while (addr_iter(minf, &mver, &i, &addr, &ret))
		rpdfs_net_disconnect(rfi, &addr);
}

int rpdfs_map_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_map_info *minf;
	int ret;

	minf = kzalloc(sizeof(struct rpdfs_map_info), GFP_KERNEL);
	if (!minf) {
		ret = -ENOMEM;
	} else {
		seqlock_init(&minf->seqlock);
		SET_RPDFS_MINF(rfi, minf);
		ret = 0;
	}

	return ret;
}

void rpdfs_map_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_map_info *minf = RPDFS_MINF(rfi);
	struct addr_map *amap;

	if (minf) {
		write_seqlock(&minf->seqlock);
		amap = rcu_dereference_protected(minf->amap, true);
		if (amap) {
			kvfree_rcu(amap, rcu);
			rcu_assign_pointer(minf->amap, NULL);
			minf->mver++;
		}
		write_sequnlock(&minf->seqlock);
		kfree(minf);
		SET_RPDFS_MINF(rfi, NULL);
	}
}
