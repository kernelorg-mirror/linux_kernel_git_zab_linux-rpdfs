/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PR_H
#define RPDFS_PR_H

#include <linux/fs.h>
#include <linux/dynamic_debug.h>
#include <linux/printk.h>

#include "super.h"

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
