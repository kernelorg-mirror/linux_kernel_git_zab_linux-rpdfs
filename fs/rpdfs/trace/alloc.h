/* SPDX-License-Identifier: GPL-2.0 */

#include "../balloc.h"

TRACE_EVENT(rpdfs_alloc_bnr,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_balloc_region *reg, u64 bnr, int ret),

	TP_ARGS(rfi, reg, bnr, ret),

	TP_STRUCT__entry(
		RFI_TRACE_FIELDS
		__field(__u64, reg_bnr)
		__field(unsigned int, reg_nr_set)
		__field(__u64, bnr)
		__field(int, ret)
	),

	TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi);
		__entry->reg_bnr = reg->base_bnr;
		__entry->reg_nr_set = reg->nr_set;
		__entry->bnr = bnr;
		__entry->ret = ret;
	),

	TP_printk(RFI_TRACE_TPF" reg bnr %llu nr_set %u bnr %llu ret %d",
		  RFI_TRACE_TPA, __entry->reg_bnr, __entry->reg_nr_set, __entry->bnr, __entry->ret)
);
