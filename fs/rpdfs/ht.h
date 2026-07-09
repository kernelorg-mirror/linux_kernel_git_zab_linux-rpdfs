/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_HT_H
#define RPDFS_HT_H

#include <linux/refcount.h>
#include <linux/rhashtable.h>

struct rpdfs_ht_entry {
	struct rcu_head rcu;
	struct rhash_head rhead;
	refcount_t refcount;
};

struct rpdfs_ht_entry *rpdfs_ht_insert(struct rhashtable *ht, struct rpdfs_ht_entry *hte,
				       const struct rhashtable_params params);
struct rpdfs_ht_entry *rpdfs_ht_get(struct rhashtable *ht, void *key,
				    const struct rhashtable_params params);
void rpdfs_ht_inc(struct rpdfs_ht_entry *hte);
bool rpdfs_ht_put(struct rhashtable *ht, struct rpdfs_ht_entry *hte,
		  const struct rhashtable_params params, bool try_remove);

#endif
