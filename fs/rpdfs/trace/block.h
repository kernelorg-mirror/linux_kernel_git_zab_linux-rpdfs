/* SPDX-License-Identifier: GPL-2.0 */

#include "../balloc.h"

#if !defined(TRACE_HEADER_MULTI_READ)

enum {
	RPDFS_BLOCK_TRACE_FLAG_REMOVAL		= (1 << 0),
	RPDFS_BLOCK_TRACE_FLAG_ACCESSED		= (1 << 1),
	RPDFS_BLOCK_TRACE_FLAG_DIRTY		= (1 << 2),
	RPDFS_BLOCK_TRACE_FLAG_UPD_META		= (1 << 3),
	RPDFS_BLOCK_TRACE_FLAG_UPD_DATA		= (1 << 4),
	RPDFS_BLOCK_TRACE_FLAG_WRITER		= (1 << 5),
};

/*
 * We're not exporting the block struct with all its weird fields.  And
 * we're not having tracepoints with a million trillion arguments.  And
 * we're not embedding some traceable fields struct that makes accesses
 * awkward and shares rich types like refcounts.  So we have this crappy
 * translation struct that trace calls fill from the args of the
 * tracepoint so it should be mostly still avoided when tracing isn't
 * enabled.  The least crappy of a bunch of crappy options?
 */
struct rpdfs_trace_block_params {
	u128 place;
	u64 bnr;
	u64 alloc_ctr;
	u64 write_ctr;
	u64 refcount;
	u64 dirty_seq;
	unsigned long flags;
	unsigned int dirty_list_nr;
	u8 request;
	u8 grant;
	u8 confirm;
	u8 write_confirm;
};

#define __rpdfs_print_block_flags(flags) \
        __print_flags(flags, "|", \
		{ RPDFS_BLOCK_TRACE_FLAG_REMOVAL,	"REMOVAL" }, \
		{ RPDFS_BLOCK_TRACE_FLAG_ACCESSED,	"ACCESSED" }, \
		{ RPDFS_BLOCK_TRACE_FLAG_DIRTY,		"DIRTY" }, \
		{ RPDFS_BLOCK_TRACE_FLAG_UPD_META,	"UPD_META" }, \
		{ RPDFS_BLOCK_TRACE_FLAG_UPD_DATA,	"UPD_DATA" }, \
		{ RPDFS_BLOCK_TRACE_FLAG_WRITER,	"WRITER" })

#define __rpdfs_print_cache_mode(mode) \
	__print_symbolic(mode, \
		{ RPDFS_CACHE_MODE_NULL,	"NULL" }, \
		{ RPDFS_CACHE_MODE_NONE,	"NONE" }, \
		{ RPDFS_CACHE_MODE_READ,	"READ" }, \
		{ RPDFS_CACHE_MODE_WRITE,	"WRITE" })

#endif

DECLARE_EVENT_CLASS(rpdfs_block_class,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p),

	TP_STRUCT__entry(
		RFI_TRACE_FIELDS
		__field(__u64, bnr)
		__field(__u64, place_hi)
		__field(__u64, place_lo)
		__field(__u64, alloc_ctr)
		__field(__u64, write_ctr)
		__field(__u64, refcount)
		__field(__u64, dirty_seq)
		__field(__u64, flags)
		__field(unsigned int, dirty_list_nr)
		__field(__u8, request)
		__field(__u8, grant)
		__field(__u8, confirm)
		__field(__u8, write_confirm)
	),

	TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi);
		__entry->bnr = p->bnr;
		__entry->place_hi = p->place >> 64;
		__entry->place_lo = (u64)(p->place);
		__entry->alloc_ctr = p->alloc_ctr;
		__entry->write_ctr = p->write_ctr;
		__entry->refcount = p->refcount;
		__entry->dirty_seq = p->dirty_seq;
		__entry->flags = p->flags;
		__entry->dirty_list_nr = p->dirty_list_nr;
		__entry->request = p->request;
		__entry->grant = p->grant;
		__entry->confirm = p->confirm;
		__entry->write_confirm = p->write_confirm;
	),

	TP_printk(RFI_TRACE_TPF" bnr %llu place %llx%016llx ac %llu wc %llu rc %llu ds %llu fl %s dl %u rq %s gr %s cf %s wcf %s",
		  RFI_TRACE_TPA, __entry->bnr, __entry->place_hi, __entry->place_lo,
		  __entry->alloc_ctr, __entry->write_ctr, __entry->refcount, __entry->dirty_seq,
		  __rpdfs_print_block_flags(__entry->flags), __entry->dirty_list_nr,
		  __rpdfs_print_cache_mode(__entry->request),
		  __rpdfs_print_cache_mode(__entry->grant),
		  __rpdfs_print_cache_mode(__entry->confirm),
		  __rpdfs_print_cache_mode(__entry->write_confirm))
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_clean,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_dirty,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_inserted,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_put_freed,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_send_confirm,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

DEFINE_EVENT(rpdfs_block_class, rpdfs_block_send_write,
	TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_trace_block_params *p),
	TP_ARGS(rfi, p)
);

TRACE_EVENT(rpdfs_block_send_uncached_confirm,
	TP_PROTO(struct rpdfs_fs_info *rfi, u64 bnr, u8 mode),
	TP_ARGS(rfi, bnr, mode),

	TP_STRUCT__entry(
		RFI_TRACE_FIELDS
		__field(__u64, bnr)
		__field(__u64, mode)
	),

	TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi);
		__entry->bnr = bnr;
		__entry->mode = mode;
	),

	TP_printk(RFI_TRACE_TPF" bnr %llu mode %s",
		  RFI_TRACE_TPA, __entry->bnr, __rpdfs_print_cache_mode(__entry->mode))
);
