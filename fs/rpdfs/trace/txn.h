/* SPDX-License-Identifier: GPL-2.0 */

TRACE_EVENT(rpdfs_txn_acquire_alloc,
	TP_PROTO(struct rpdfs_fs_info *rfi, void *txn, u64 bnr, int ret),

	TP_ARGS(rfi, txn, bnr, ret),

	TP_STRUCT__entry(
		RFI_TRACE_FIELDS
		__field(void *, txn)
		__field(__u64, bnr)
		__field(int, ret)
	),

	TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi);
		__entry->txn = txn;
		__entry->bnr = bnr;
		__entry->ret = ret;
	),

	TP_printk(RFI_TRACE_TPF" txn %p bnr %llu ret %d",
		  RFI_TRACE_TPA, __entry->txn, __entry->bnr, __entry->ret)
);
