/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PARAMS_H
#define RPDFS_PARAMS_H

#include "net.h"

struct rpdfs_params {
	/* XXX arbitrary limit on number of addrs */
	struct rpdfs_net_transport_addr devd_addrs[16];
	size_t nr_addrs;
	bool mkfs;
};

/*
 * We'll probably want some form of protection around dereferencing
 * params which could be updated dynamically from sysfs.  seqlocks?  In
 * any case, make this an interface that can wrap the access.
 */
#define RPDFS_FSINFO_PARAM(rfi, param) \
({ \
	(rfi)->params.param; \
})

#endif
