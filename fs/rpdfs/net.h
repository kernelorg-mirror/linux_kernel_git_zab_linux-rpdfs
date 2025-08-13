/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_NET_H
#define RPDFS_NET_H

#include <linux/inet.h>
#include <linux/bvec.h>

#include "format-msg.h"
#include "super.h"

/*
 * A little convenience that gathers that arguments that describe a
 * message that's passed through calls when sending or receiving.
 * Generally, the caller owns the buffers and the receiver has to copy
 * or increment a reference to use the buffers after returning.
 */
struct rpdfs_net_message_desc {
	void *ctl_buf;
	struct page *data_page;
	u16 data_size;
	u8 type;
	u8 ctl_size;
};

/*
 * A wrapper that reserves space for the supported address families of
 * the transports.  ->sa.sa_family specifies the family in use.
 */
struct rpdfs_net_transport_addr {
	union {
		struct sockaddr sa;
		struct sockaddr_in _sin;
	};
};

struct rpdfs_net_transport_ops {
	/*
	 * The size of the private allocation associated with each
	 * connection that is used by the transport.
	 */
	unsigned int priv_size;

	/*
	 * The size that's added to the front of page frag that's
	 * allocated per send and passed to the send method.  The wire
	 * header and ctl buf are at the end (tail) of the buffer.  The
	 * size needs to be aligned to a u64 so that the header at the
	 * tail can be initialized.
	 */
	unsigned int send_pfrag_head;

	/*
	 * Setup state needed across all connections through the transport.  The returned info
	 * pointer will be passed into ops calls and eventually destroyed.
	 */
	void *(*setup)(struct rpdfs_fs_info *rfi);
	/*
	 * Tear down the state created by setup.  Called once all
	 * connections have been shutdown and freed.
	 */
	void (*destroy)(struct rpdfs_fs_info *rfi, void *info);

	/*
	 * Initialize the transport's connection data.   Must have no
	 * side-effects other than initializing the memory in the
	 * transport's data.  Once this returns ->send can be called
	 * before ->connect.  The transport is responsible for queueing
	 * the sends while connect is in progress.  (sends to a failed
	 * connecting socket will be dropped as the connection is
	 * shutdown and freed.)
	 */
	void (*init)(struct rpdfs_fs_info *rfi, void *info, void *priv);
	/*
	 * Establish a connection to the given address.  When this
	 * returns success the net core can start sending messages.  The
	 * core will not call this until the connection is ready for
	 * calls coming from the transport.  It is acceptable for the
	 * transport to call up to the core (particularly with incoming
	 * messages) during this call.
	 */
	int (*connect)(struct rpdfs_fs_info *rfi, void *info, void *priv,
		       struct rpdfs_net_transport_addr *addr);
	/*
	 * Send a message down the previously connected connection.  The caller allocated
	 * and initialized the contiguous header and ctl buf in a page frag.  This is
	 * responsible for freeing the page frag.
	 */
	void (*send)(struct rpdfs_fs_info *rfi, void *info, void *priv, void *pfrag,
		     unsigned int hdr_ctl_size, struct page *data_page, unsigned int data_size);
	/*
	 * Stop traffic on the connection.  One this returns the transport will no
	 * longer call up into the core.  Once all the references in the core drain the core
	 * can ask the transport to free its resources associated with the connection.
	 */
	void (*shutdown)(struct rpdfs_fs_info *rfi, void *info, void *priv);
	/*
	 * Free any resources that were associated with the transport
	 * data.  Once this returns the transport must not reference its
	 * data and the core will be freeing its memory.  This can be
	 * called from any context and should be quick and not block.
	 */
	void (*free)(struct rpdfs_fs_info *rfi, void *info, void *priv);
};

typedef int (*rpdfs_net_recv_fn_t)(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md);

int rpdfs_net_preload(struct rpdfs_fs_info *rfi, gfp_t gfp);
void rpdfs_net_preload_end(struct rpdfs_fs_info *rfi);

int rpdfs_net_connect(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr);
void rpdfs_net_disconnect(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr);

int rpdfs_net_send(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr,
		   struct rpdfs_net_message_desc *md, gfp_t gfp);

void rpdfs_net_recv(struct rpdfs_fs_info *rfi, void *priv, struct rpdfs_net_message_desc *md);
void rpdfs_net_shutdown_conn(struct rpdfs_fs_info *rfi, void *priv, int nerrno);

bool rpdfs_net_invalid_sizes(struct rpdfs_net_message_desc *md);
int rpdfs_net_nerrno(u8 err);
u8 rpdfs_net_err(int nerrno);

/* setup and shutdown */
int rpdfs_net_setup(struct rpdfs_fs_info *rfi, const struct rpdfs_net_transport_ops *ops);
int rpdfs_net_register_recv(struct rpdfs_fs_info *rfi, u8 type, rpdfs_net_recv_fn_t recv_fn);
void rpdfs_net_unregister_recv(struct rpdfs_fs_info *rfi, u8 type, rpdfs_net_recv_fn_t recv_fn);
void rpdfs_net_destroy(struct rpdfs_fs_info *rfi);

#endif
