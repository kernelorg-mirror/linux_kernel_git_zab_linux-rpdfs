/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_META_H
#define RPDFS_META_H

#include "format-msg.h"
#include "inode.h"

int rpdfs_folio_loc_from_block_key(struct rpdfs_iget_data *igd, pgoff_t *index,
				   struct rpdfs_block_key *key);
void rpdfs_block_key_from_folio(struct rpdfs_block_key *key, struct folio *folio);

/**
 * typedef fgf_t - Flags for getting rpdfs block folios from the page cache.
 *
 * * %MGF_NEW - notupdate and uninitialized contents, caller must initialize
 * * %MGF_READ - block waiting for uptodate
 * * %MGF_WRITE - block waiting for uptodate, won't be under writeback
 */
typedef unsigned int __bitwise mgf_t;

#define MGF_NEW		((__force mgf_t)0x00000001)
#define MGF_READ	((__force mgf_t)0x00000002)
#define MGF_WRITE	((__force mgf_t)0x00000004)
#define MGF__INVALID	((__force mgf_t)~(0x00000008 - 1))

struct folio *rpdfs_meta_get_folio(struct inode *inode, mgf_t mgf, u8 type, u64 t_index);

void rpdfs_meta_dirty_folio(struct folio *folio);

int rpdfs_meta_alloc_shadow(struct inode *inode);
void rpdfs_meta_evict_shadow(struct inode *inode);

#endif
