/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PR_H
#define RPDFS_PR_H

#include <linux/fs.h>
#include <linux/dynamic_debug.h>
#include <linux/printk.h>
#include "format-msg.h"
#include "super.h"

#define RBKF		"%llx.%llx.%x.%llx"
#define RBKA(bk)	le64_to_cpu((bk)->k[0]), le64_to_cpu((bk)->k[1]), \
			(u8)(le64_to_cpu((bk)->k[2]) >> RPDFS_BLOCK_KEY_TYPE__SHIFT), \
			(le64_to_cpu((bk)->k[2]) & RPDFS_BLOCK_KEY_INDEX__MASK)

#define folio_flag_char(folio, suffix, c) \
	(folio_test_##suffix(folio) ? c : '-')
#define RFF		"f mp %p ind %lu rc %d %c%c%c%c"
#define RFA(folio)	(folio)->mapping, (folio)->index, folio_ref_count(folio), \
			folio_flag_char(folio, locked, 'l'), \
			folio_flag_char(folio, writeback, 'w'), \
			folio_flag_char(folio, uptodate, 'u'), \
			folio_flag_char(folio, dirty, 'd')

#define rpdfs_err(fmt, args...) \
	printk(KERN_ERR "rpdfs: "fmt, ##args)

#if 1
#define _rpdfs_pr_debug(fmt, args...) \
do { \
	/* debug messages are strictly one line, we add so callers don't have to */ \
	BUILD_BUG_ON(fmt[sizeof(fmt) - 2] == '\n'); \
	pr_debug("rpdfs: %s:%u: "fmt"\n", __func__, __LINE__, ##args); \
} while (0)
#define rpdfs_prd(fmt, args...) \
	_rpdfs_pr_debug(fmt, ##args)
#define rpdfs_prd_sb(sb, fmt, args...) \
	_rpdfs_pr_debug("[sb %s] " fmt, (sb)->s_id, ##args)
#define rpdfs_prd_rfi(rfi, fmt, args...) \
	_rpdfs_pr_debug(fmt, ##args)
#else
#define rpdfs_prd(ctx, fmt, args...)
do {
	(void) ({ args## ; });
} while (0)
#endif

#endif
