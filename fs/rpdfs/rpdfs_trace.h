/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This is in the fs/rpdfs/ dir because it makes it handy to build
 * against compatible installed host kernels as an external module from
 * within the devel tree.  When we merge upstream well move it over to
 * something like include/trace/events/rpdfs.h .
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM rpdfs

#if !defined(_TRACE_RPDFS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RPDFS_H

#include <linux/tracepoint.h>

#include "trace/common.h"

#endif /* _TRACE_RPDFS_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE rpdfs_trace
#include <trace/define_trace.h>
