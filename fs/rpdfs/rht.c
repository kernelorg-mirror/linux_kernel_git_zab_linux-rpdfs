/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/rhashtable.h>
#include <linux/bug.h>

#include "rht.h"

/*
 * Call a callback on the objects starting from a given bucket index.
 *
 * This is like the rhashtable*_walk_ API but we can set the starting
 * point and it doesn't return -EAGAIN while resizing.
 *
 * The caller must hold the RCU read lock.
 *
 * This will cycle through resizing tables as long as they appear in
 * future_tbl.  Thus the same object can be given to the callback
 * multiple times in one walk call.
 */
unsigned int rhashtable_walk_buckets(struct rhashtable *rht, unsigned int index,
			bool (*obj_callback)(struct rhash_head *pos, void *data), void *data)
{
	struct bucket_table *tbl;
	struct rhash_head *pos;
	unsigned int i;
	bool stop;
	u32 hash;

	WARN_ON_ONCE(!rcu_read_lock_held());

	tbl = rht_dereference_rcu(rht->tbl, rht);
	stop = false;
	do {
		for (i = 0; i < tbl->size && !stop; i++) {
			hash = index++ % tbl->size;
			rht_for_each_rcu(pos, tbl, hash)
				stop |= obj_callback(pos, data);
		}
	} while (!stop && !IS_ERR_OR_NULL((tbl = rht_dereference_rcu(tbl->future_tbl, rht))));

	return index;
}
