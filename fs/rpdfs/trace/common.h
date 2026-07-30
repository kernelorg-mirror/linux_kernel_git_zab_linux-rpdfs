/* SPDX-License-Identifier: GPL-2.0 */

#define RFI_TRACE_FIELDS	__field(__u32, id)
#define RFI_TRACE_ID(rfi) \
({ \
	__typeof__(rfi) _rfi = (rfi); \
	(_rfi) ? le32_to_cpup((__le32 *)(void *)_rfi->client_uuid) >> 8 : 0; \
})
#define RFI_TRACE_ASSIGN(rfi)	__entry->id = RFI_TRACE_ID(rfi); /* intentional semicolon */
#define RFI_TRACE_TPF		"%06x"
#define RFI_TRACE_TPA		__entry->id
