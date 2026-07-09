
#define __rlock_mode_sym(mode) \
	__print_symbolic(mode, \
		{ RPDFS_RLOCK_MODE_NULL, "NULL"}, \
		{ RPDFS_RLOCK_MODE_NONE, "NONE"}, \
		{ RPDFS_RLOCK_MODE_SH_RD, "SH_RD"}, \
		{ RPDFS_RLOCK_MODE_EX_WR, "EX_WR"})

#define __RLOCK_TP_PROTO \
	u8 grant_mode, u8 request_mode, u8 revoke_mode, int rd_holders, int wr_holders
#define __RLOCK_TP_ARGS \
	grant_mode, request_mode, revoke_mode, rd_holders, wr_holders
#define __RLOCK_TP_entry \
	__field(__u8, grant_mode) \
	__field(__u8, request_mode) \
	__field(__u8, revoke_mode) \
	__field(int, rd_holders) \
	__field(int, wr_holders)
#define __RLOCK_TP_assign \
	__entry->grant_mode = grant_mode; \
	__entry->request_mode = request_mode; \
	__entry->revoke_mode = revoke_mode; \
	__entry->rd_holders = rd_holders; \
	__entry->wr_holders = wr_holders;
#define __RLOCK_TP_FMT \
	"gr %u rq %u rk %u rdh %d wrh %d"
#define __RLOCK_TP_ENT_ARGS \
	__entry->grant_mode, __entry->request_mode, __entry->request_mode, __entry->rd_holders, \
	__entry->wr_holders

TRACE_EVENT(rpdfs_rlock_lock_enter,
        TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key, u8 mode),

        TP_ARGS(rfi, key, mode),

        TP_STRUCT__entry(
		RFI_TRACE_FIELDS
                __field(__u64, k0)
                __field(__u64, k1)
                __field(__u8, mode)
        ),

        TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi)
                __entry->k0 = le64_to_cpu(key->k[0]);
                __entry->k1 = le64_to_cpu(key->k[1]);
                __entry->mode = mode;
        ),

        TP_printk(RFI_TRACE_TPF" key %llu.%llu mode %s",
		  RFI_TRACE_TPA, __entry->k0, __entry->k1, __rlock_mode_sym(__entry->mode))
);

TRACE_EVENT(rpdfs_rlock_lock_wait,
        TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key, u8 mode,
		 __RLOCK_TP_PROTO),

        TP_ARGS(rfi, key, mode, __RLOCK_TP_ARGS),

        TP_STRUCT__entry(
		RFI_TRACE_FIELDS
                __field(__u64, k0)
                __field(__u64, k1)
                __field(__u8, mode)
		__RLOCK_TP_entry
        ),

        TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi)
                __entry->k0 = le64_to_cpu(key->k[0]);
                __entry->k1 = le64_to_cpu(key->k[1]);
                __entry->mode = mode;
		__RLOCK_TP_assign
        ),

        TP_printk(RFI_TRACE_TPF" key %llu.%llu mode %s "__RLOCK_TP_FMT,
		  RFI_TRACE_TPA, __entry->k0, __entry->k1, __rlock_mode_sym(__entry->mode),
		  __RLOCK_TP_ENT_ARGS)
);

TRACE_EVENT(rpdfs_rlock_lock_exit,
        TP_PROTO(struct rpdfs_fs_info *rfi, struct rpdfs_rlock_key *key, u8 mode, int ret,
		 __RLOCK_TP_PROTO),

        TP_ARGS(rfi, key, mode, ret, __RLOCK_TP_ARGS),

        TP_STRUCT__entry(
		RFI_TRACE_FIELDS
                __field(__u64, k0)
                __field(__u64, k1)
                __field(__u8, mode)
                __field(int, ret)
		__RLOCK_TP_entry
        ),

        TP_fast_assign(
		RFI_TRACE_ASSIGN(rfi)
                __entry->k0 = le64_to_cpu(key->k[0]);
                __entry->k1 = le64_to_cpu(key->k[1]);
                __entry->mode = mode;
                __entry->ret = ret;
		__RLOCK_TP_assign
        ),

        TP_printk(RFI_TRACE_TPF" key %llu.%llu mode %s ret %d"__RLOCK_TP_FMT,
		  RFI_TRACE_TPA, __entry->k0, __entry->k1, __rlock_mode_sym(__entry->mode),
		  __entry->ret, __RLOCK_TP_ENT_ARGS)
);
