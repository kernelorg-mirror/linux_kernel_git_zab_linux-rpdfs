/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_MAP_H
#define RPDFS_MAP_H

#include "net.h"

int rpdfs_map_add_addr(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr);

int rpdfs_map_nr_devds(struct rpdfs_fs_info *rfi, u64 *mver);
int rpdfs_map_bnr_to_addr(struct rpdfs_fs_info *rfi, u64 bnr,
			  struct rpdfs_net_transport_addr *addr, u64 *mver);
int rpdfs_map_nth_addr(struct rpdfs_fs_info *rfi, unsigned int n,
		       struct rpdfs_net_transport_addr *addr, u64 *mver);
int rpdfs_map_alloc_stripe_geom(struct rpdfs_fs_info *rfi, u64 bnr, unsigned long *this_stripe,
				unsigned long *stripes, u64 *mver);

int rpdfs_map_connect(struct rpdfs_fs_info *rfi);
void rpdfs_map_disconnect(struct rpdfs_fs_info *rfi);

int rpdfs_map_setup(struct rpdfs_fs_info *rfi);
void rpdfs_map_destroy(struct rpdfs_fs_info *rfi);

#endif
