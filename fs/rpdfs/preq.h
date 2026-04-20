/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PREQ_H
#define RPDFS_PREQ_H

#include "format-msg.h"
#include "super.h"

int rpdfs_preq_block_counts(struct rpdfs_fs_info *rfi, struct rpdfs_msg_block_counts_result *bcr);

int rpdfs_preq_setup(struct rpdfs_fs_info *rfi);
void rpdfs_preq_destroy(struct rpdfs_fs_info *rfi);

#endif
