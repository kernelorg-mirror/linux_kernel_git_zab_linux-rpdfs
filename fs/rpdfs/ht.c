/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/rhashtable.h>

#include "ht.h"

/*
 * Some quick helpers to manage refcounted presence in an rcu hash
 * table.  This is based around the usage model where put examines the
 * object and decides that it's no longer needed and can be removed if
 * there are no users.
 *
 * Presence in the hash table holds a reference so that dec_if_one can
 * be used to decide to remove the entry from the table.  As long as all
 * puts attempt this there will always be a final put that sees the
 * freeable object.
 *
 * Table lookups use inc_not_zero to only return refs to objects that
 * aren't being removed from the table.
 *
 * The caller is responsible for object lifetimes.  Typically by
 * embedding the entry in the object, using container_of, and freeing
 * with something like kfree_rcu as put returns true.
 */


/*
 * Insert an entry in the table.  The object needs to be fully
 * initialized before calling.  It will be visible to lookups while in
 * this function.
 *
 * Returns the entry that is present at the inserting entry's key.  Can
 * return ERR_PTR on error.  If the return doesn't match the caller's
 * err then it should be freed and then either the return can be used or
 * an error can be returned.
 *
 * If this returns an existing entry then the caller's insertion entry
 * will have been initialized so that _ht_put can still be used to free
 * it.
 */
struct rpdfs_ht_entry *rpdfs_ht_insert(struct rhashtable *ht, struct rpdfs_ht_entry *hte,
				       const struct rhashtable_params params)
{
	struct rpdfs_ht_entry *found;
	bool retry;

	refcount_set_release(&hte->refcount, 2);

	do {
		rcu_read_lock();
		found = rhashtable_lookup_get_insert_fast(ht, &hte->rhead, params);
		retry = !IS_ERR_OR_NULL(found) && !refcount_inc_not_zero(&found->refcount);
		rcu_read_unlock();
		if (retry)
			cpu_relax();
	} while (retry);

	if (found) {
		refcount_dec(&hte->refcount);
		hte = found;
	}

	return hte;
}

struct rpdfs_ht_entry *rpdfs_ht_get(struct rhashtable *ht, void *key,
				    const struct rhashtable_params params)
{
	struct rpdfs_ht_entry *hte;

	rcu_read_lock();
	hte = rhashtable_lookup_fast(ht, key, params);
	if (hte && !refcount_inc_not_zero(&hte->refcount))
		hte = NULL;
	rcu_read_unlock();

	return hte;
}

/*
 * Drop the caller's reference.  And then, if requested, try to drop the
 * refcount to zero and remove the entry from the table.  We protect the
 * additional refcount dec with RCU so that the entry won't be freed
 * until we're done.
 *
 * The caller can continue the rcu protection of the object the entry is
 * embedded in by surrounding the call with another RCU section.
 * Perhaps to lock the test that provides the try_remove condition.
 *
 * Returns true if the refcount dropped to 0 and was removed from the
 * hash table.  There will be no more references to the entry.
 */
bool rpdfs_ht_put(struct rhashtable *ht, struct rpdfs_ht_entry *hte,
		  const struct rhashtable_params params, bool try_remove)
{
	bool zero;

	rcu_read_lock();
	zero = refcount_dec_and_test(&hte->refcount);
	if (!zero && try_remove && (zero = refcount_dec_if_one(&hte->refcount)))
		rhashtable_remove_fast(ht, &hte->rhead, params);
	rcu_read_unlock();

	return zero;
}
