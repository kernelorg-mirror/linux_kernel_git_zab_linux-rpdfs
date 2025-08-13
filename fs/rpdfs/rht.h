/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_RHT_H
#define RPDFS_RHT_H

unsigned int rhashtable_walk_buckets(struct rhashtable *rht, unsigned int index,
			bool (*obj_callback)(struct rhash_head *pos, void *data), void *data);

#endif


