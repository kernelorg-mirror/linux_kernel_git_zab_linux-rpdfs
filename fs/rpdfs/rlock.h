/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_RLOCK_H
#define RPDFS_RLOCK_H

#include "rlock.h"
#include "super.h"

struct rpdfs_rlock;
struct rpdfs_rlock_hold {
	struct rpdfs_rlock *rlock;
	u8 mode;
};

#define INIT_RPDFS_RLOCK_HOLD {NULL, }

#define DECLARE_RPDFS_RLOCK_HOLD(name) \
	struct rpdfs_rlock_hold name = INIT_RPDFS_RLOCK_HOLD

static inline void rpdfs_rlock_init_hold(struct rpdfs_rlock_hold *hold)
{
	*hold = (struct rpdfs_rlock_hold) INIT_RPDFS_RLOCK_HOLD;
}

int rpdfs_rlock_lock(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key, u8 mode,
		     struct rpdfs_rlock_hold *hold);
void rpdfs_rlock_unlock(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_hold *hold);

int rpdfs_rlock_setup(struct rpdfs_fs_info *rfi);
void rpdfs_rlock_destroy(struct rpdfs_fs_info *rfi);

#endif
