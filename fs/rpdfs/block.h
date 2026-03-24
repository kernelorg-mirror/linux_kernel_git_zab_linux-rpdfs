/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_BLOCK_H
#define RPDFS_BLOCK_H

#include <linux/list.h>

#include "block.h"
#include "super.h"

typedef u8 __bitwise rbaf_t;

/*
 * Flags for _block_acquire.
 */
enum {
	/*
	 * Get an exclusive write handle.  The acquired handle *does
	 * not* immediately allow modifying the block.  It intends to
	 * eventually modify blocks via a transaction's apply phase
	 * after the blocks are dirtied.  Acquisition will block until
	 * there are no concurrent read or write handles, but the block
	 * can be being flushed and dirty while a write handle is held.
	 *
	 * Without _WRITE the handle has no intention of modifying the
	 * block and is shared with other !_WRITE handles.
	 */
	_RBAF_WRITE,

	/*
	 * The caller is going to fully overwrite the block contents.
	 * We don't need to get the current block contents.  Only valid
	 * with _WRITE.
	 */
	_RBAF_OVERWRITE,

	/*
	 * Don't block waiting for a cache mode.  If the block's granted
	 * mode is insufficient then return -EAGAIN.
	 */
	_RBAF_NONBLOCK_MODE,

	/*
	 * Don't block waiting for flushing to complete.  If the block
	 * is within the requested flushing boundary then we return
	 * -EAGAIN.
	 */
	_RBAF_NONBLOCK_FLUSH,

	/*
	 * An acquisition for an allocation.  Only satisfy with blocks
	 * that are already granted a write mode and whose alloc_ctr
	 * indicates that they're free.  This won't send requests for
	 * blocks or wait.  Returns -ENODATA if the block wasn't already
	 * cached, writable, and free.
	 */
	_RBAF_ALLOC,

	/*
	 * Only acquires blocks that are already dirty.  Returns
	 * -ENODATA if no block was cached or the cached block wasn't
	 * dirty.
	 */
	_RBAF_ALREADY_DIRTY,
};

#define RBAF_WRITE		((__force rbaf_t)BIT(_RBAF_WRITE))
#define RBAF_OVERWRITE		((__force rbaf_t)BIT(_RBAF_OVERWRITE))
#define RBAF_NONBLOCK_MODE	((__force rbaf_t)BIT(_RBAF_NONBLOCK_MODE))
#define RBAF_NONBLOCK_FLUSH	((__force rbaf_t)BIT(_RBAF_NONBLOCK_FLUSH))
#define RBAF_ALLOC		((__force rbaf_t)BIT(_RBAF_ALLOC))
#define RBAF_ALREADY_DIRTY	((__force rbaf_t)BIT(_RBAF_ALREADY_DIRTY))

/*
 * Shared read or exclusive write handles are acquired in the form of
 * these pointers.  The block properties here are read-only.
 */
struct rpdfs_block_handle {
	u64 bnr;
	u64 alloc_ctr;
	u64 wcount;
	void *data;
};

int rpdfs_block_acquire(struct rpdfs_fs_info *rfi, u64 bnr, struct rpdfs_block_handle **hnd_ret,
			rbaf_t rbaf);
void rpdfs_block_release(struct rpdfs_fs_info *rfi, struct rpdfs_block_handle **hnd);

typedef struct rpdfs_block_handle *(*rpdfs_block_entry_handle_fn_t)(struct list_head *pos);

void rpdfs_block_make_dirty(struct rpdfs_fs_info *rfi, struct list_head *list,
			    rpdfs_block_entry_handle_fn_t entry_handle_fn);
int rpdfs_block_flush(struct rpdfs_fs_info *rfi, u64 bnr, bool wait);
int rpdfs_block_sync(struct rpdfs_fs_info *rfi, bool wait);

bool rpdfs_block_should_request_free(struct rpdfs_fs_info *rfi, u64 until);
int rpdfs_block_request_free(struct rpdfs_fs_info *rfi, u64 *until);

int rpdfs_block_setup(struct rpdfs_fs_info *rfi);
void rpdfs_block_destroy(struct rpdfs_fs_info *rfi);

#endif
