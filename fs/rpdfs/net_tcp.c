/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/net.h>
#include <linux/bio.h>
#include <net/tcp.h>

#include "lists.h"
#include "net.h"
#include "net_tcp.h"
#include "pr.h"

struct rpdfs_net_tcp_info {
	struct workqueue_struct *workq;
};

struct rpdfs_net_tcp_conn_private {
	struct rpdfs_fs_info *rfi;
	struct rpdfs_net_tcp_info *ntinf;
	struct socket *sock;
	struct rpdfs_net_transport_addr addr;

	struct llist_head send_llhead;
	struct list_head send_list;
	struct work_struct send_work;

	struct work_struct recv_work;
	struct tpriv_rx {
		struct rpdfs_net_message_desc md;
		struct iov_iter iter;
		struct bio_vec bvecs[2];
		struct page *hdr_ctl_page;
	} rx;

	void (*orig_data_ready)(struct sock *);
	void (*orig_state_change)(struct sock *);
	void (*orig_write_space)(struct sock *);
};

/*
 * These are allocated as a contig page frag by the net_send core caller
 * and passed in as our send method.
 */
struct rpdfs_net_tcp_send {
	struct llist_node llnode;
	struct list_head head;
	struct bio_vec bvecs[2];
	struct iov_iter iter;
	struct page *data_page;
	/* what follows is initialized by send and is the buf to send over the wire */
	struct rpdfs_msg_header hdr;
	u8 ctl_buf[];
};

#define RPDFS_NET_TCP_SEND_PFRAG_HEAD offsetof(struct rpdfs_net_tcp_send, hdr)

static void rpdfs_net_tcp_send_work_fn(struct work_struct *work)
{
	struct rpdfs_net_tcp_conn_private *tpriv =
		container_of(work, struct rpdfs_net_tcp_conn_private, send_work);
	struct rpdfs_fs_info *rfi = tpriv->rfi;
	struct rpdfs_net_tcp_send *tsend;
	struct rpdfs_net_tcp_send *tsend__;
	unsigned int noreclaim_flag;
	struct llist_node *llnode;
	struct msghdr msg;
	int ret;

	smp_rmb(); /* pairing with wmb as tsends are added */
	llnode = llist_del_all(&tpriv->send_llhead);
	if (llnode)
		llist_reverse_add_tail(tsend, llnode, llnode, &tpriv->send_list, head);

	noreclaim_flag = memalloc_noreclaim_save();

	ret = 0;
	list_for_each_entry_safe(tsend, tsend__, &tpriv->send_list, head) {
		msg = (struct msghdr) {
			.msg_flags = MSG_SPLICE_PAGES | MSG_NOSIGNAL | MSG_DONTWAIT,
		};

		rpdfs_prd("sending tsend %p t %u cs %u ds %u",
			tsend, tsend->hdr.type, tsend->hdr.ctl_size,
			le16_to_cpu(tsend->hdr.data_size));

		/* sure hope this iter copying isn't a terrible idea */
		msg.msg_iter = tsend->iter;
		ret = sock_sendmsg(tpriv->sock, &msg);
		tsend->iter = msg.msg_iter;
		if (ret < 0)
			break;

		if (iov_iter_count(&tsend->iter) == 0) {
			list_del_init(&tsend->head);
			if (tsend->data_page)
				put_page(tsend->data_page);
			page_frag_free(tsend);
		}
	}

	memalloc_noreclaim_restore(noreclaim_flag);

	if (ret < 0) {
		rpdfs_err("send errno %d to %pISpc, shutting down", ret, &tpriv->addr.sa);
		rpdfs_net_shutdown_conn(rfi, tpriv, ret);
	}
}

static int rpdfs_net_tcp_read_actor(read_descriptor_t *desc, struct sk_buff *skb,
				    unsigned int offset, size_t len)
{
	struct iov_iter *iter = desc->arg.data;
	struct skb_seq_state st;
	unsigned int avail;
	const u8 *ptr;
	int used;

	rpdfs_prd("tcp read actor offset %u len %zu", offset, len);

	skb_prepare_seq_read(skb, offset, skb->len, &st);
	used = 0;
	for (;;) {
		avail = skb_seq_read(used, &ptr, &st);
		if (avail == 0)
			break;

		used += copy_to_iter(ptr, avail, iter);
		rpdfs_prd("tcp read actor used %d iter_count %zu", used, iov_iter_count(iter));

		if (iov_iter_count(iter) == 0) {
			skb_abort_seq_read(&st);
			break;
		}
	}

	return used;
}

/*
 * Copying until we line up all the right bits of the stack so that our
 * incoming flow can land in rx buffers and end up with blocks aligned
 * to pages.
 */
static void rpdfs_net_tcp_recv_work_fn(struct work_struct *work)
{
	struct rpdfs_net_tcp_conn_private *tpriv =
		container_of(work, struct rpdfs_net_tcp_conn_private, recv_work);
	struct rpdfs_fs_info *rfi = tpriv->rfi;
	struct sock *sk = tpriv->sock->sk;
	struct tpriv_rx *rx = &tpriv->rx;
	struct rpdfs_msg_header *hdr;
	read_descriptor_t desc;
	int ret = 0;

	for (;;) {
		if (iov_iter_count(&rx->iter) == 0) {
			if (rx->bvecs[0].bv_page == NULL) {
				/* first read header */
				bvec_set_page(&rx->bvecs[0], rx->hdr_ctl_page,
					      sizeof(struct rpdfs_msg_header), 0);

			} else {
				/* then check header and read ctl and data */
				hdr = page_address(rx->hdr_ctl_page);
				rx->md.ctl_buf = (void *)(hdr + 1);
				rx->md.data_size = le16_to_cpu(hdr->data_size);
				rx->md.type = hdr->type;
				rx->md.ctl_size = hdr->ctl_size;

				if (rpdfs_net_invalid_sizes(&rx->md)) {
					rpdfs_prd("bad hdr sizes ctl %u data %u",
						  rx->md.ctl_size, rx->md.data_size);
					ret = -EPROTO;
					goto out;
				}

				bvec_set_page(&rx->bvecs[0], rx->hdr_ctl_page,
					      rx->md.ctl_size, sizeof(struct rpdfs_msg_header));

				if (rx->md.data_size) {
					rx->md.data_page = alloc_page(GFP_NOFS);
					if (!rx->md.data_page) {
						ret = -ENOMEM;
						goto out;
					}
					bvec_set_page(&rx->bvecs[1], rx->md.data_page,
						      rx->md.data_size, 0);
				}
			}
			iov_iter_bvec(&rx->iter, ITER_DEST, rx->bvecs,
				      !!rx->bvecs[0].bv_len + !!rx->bvecs[1].bv_len,
				      rx->bvecs[0].bv_len + rx->bvecs[1].bv_len);
		}

		rpdfs_prd("tcp recv work pg %p off %u count %zu",
			rx->bvecs[0].bv_page, rx->bvecs[0].bv_offset, iov_iter_count(&rx->iter));

		desc = (read_descriptor_t) {
			.arg.data = &rx->iter,
			.count = 1, /* const, only actor return stops */
		};
		lock_sock(sk);
		ret = tcp_read_sock(sk, &desc, rpdfs_net_tcp_read_actor);
		release_sock(sk);
		if (ret <= 0)
			goto out;

		/* incoming message complete, call recv */
		if (iov_iter_count(&rx->iter) == 0 && rx->bvecs[0].bv_offset > 0) {
			rpdfs_net_recv(rfi, tpriv, &rx->md);
			if (rx->md.data_page) {
				put_page(rx->md.data_page);
				rx->md.data_page = NULL;
			}
			memset(rx->bvecs, 0, sizeof(rx->bvecs));
		}
	}

out:
	if (ret < 0) {
		rpdfs_err("recv errno %d from %pISpc, shutting down", ret, &tpriv->addr.sa);
		rpdfs_net_shutdown_conn(rfi, tpriv, ret);
	}
}

static void rpdfs_net_tcp_data_ready(struct sock *sk)
{
	struct rpdfs_net_tcp_conn_private *tpriv;

	read_lock_bh(&sk->sk_callback_lock);
	if ((tpriv = sk->sk_user_data))
		queue_work(tpriv->ntinf->workq, &tpriv->recv_work);
	read_unlock_bh(&sk->sk_callback_lock);
}

static void rpdfs_net_tcp_state_change(struct sock *sk)
{
	struct rpdfs_net_tcp_conn_private *tpriv;
	void (*state_change)(struct sock *);

	rpdfs_prd("sock state change to %u", sk->sk_state);

	read_lock_bh(&sk->sk_callback_lock);
	if ((tpriv = sk->sk_user_data))
		state_change = tpriv->orig_state_change;
	else
		state_change = sk->sk_state_change;
	read_unlock_bh(&sk->sk_callback_lock);

	if (tpriv) {
		switch (sk->sk_state)  {
		case TCP_ESTABLISHED:
			queue_work(tpriv->ntinf->workq, &tpriv->send_work);
			queue_work(tpriv->ntinf->workq, &tpriv->recv_work);
			break;
		case TCP_CLOSE_WAIT:
		case TCP_CLOSE:
			rpdfs_err("tcp state change to %s on %pISpc, shutting down",
					sk->sk_state == TCP_CLOSE_WAIT ? "TCP_CLOSE_WAIT" :
									 "TCP_CLOSE",
					&tpriv->addr.sa);
			rpdfs_net_shutdown_conn(tpriv->rfi, tpriv, -ECONNRESET);
			break;
		}
	}

	state_change(sk);
}

static void rpdfs_net_tcp_write_space(struct sock *sk)
{
	struct rpdfs_net_tcp_conn_private *tpriv;

	read_lock_bh(&sk->sk_callback_lock);
	if ((tpriv = sk->sk_user_data))
		queue_work(tpriv->ntinf->workq, &tpriv->send_work);
	read_unlock_bh(&sk->sk_callback_lock);
}

static void set_callbacks(struct socket *sock, struct rpdfs_net_tcp_conn_private *tpriv)
{
	struct sock *sk = sock->sk;

	/* assign new callbacks */
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = tpriv;
	tpriv->orig_data_ready = sk->sk_data_ready;
	tpriv->orig_state_change = sk->sk_state_change;
	tpriv->orig_write_space = sk->sk_write_space;
	sk->sk_data_ready = rpdfs_net_tcp_data_ready;
	sk->sk_state_change = rpdfs_net_tcp_state_change;
	sk->sk_write_space = rpdfs_net_tcp_write_space;
	write_unlock_bh(&sk->sk_callback_lock);
}

static void restore_callbacks(struct socket *sock, struct rpdfs_net_tcp_conn_private *tpriv)
{
	struct sock *sk = sock->sk;

	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data    = NULL;
	sk->sk_data_ready   = tpriv->orig_data_ready;
	sk->sk_state_change = tpriv->orig_state_change;
	sk->sk_write_space  = tpriv->orig_write_space;
	sk->sk_no_check_tx = 0;
	write_unlock_bh(&sk->sk_callback_lock);
}

static void *rpdfs_net_tcp_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_net_tcp_info *ntinf;

	ntinf = kzalloc(sizeof(struct rpdfs_net_tcp_info), GFP_NOFS);
	if (ntinf) {
		ntinf->workq = alloc_workqueue("rpdfs-net-tcp", WQ_MEM_RECLAIM, 0);
		if (!ntinf->workq) {
			kfree(ntinf);
			ntinf = NULL;
		}
	}

	return ntinf;
}

static void rpdfs_net_tcp_destroy(struct rpdfs_fs_info *rfi, void *info)
{
	struct rpdfs_net_tcp_info *ntinf = info;

	if (ntinf) {
		destroy_workqueue(ntinf->workq);
		kfree(ntinf);
	}
}

static void rpdfs_net_tcp_init(struct rpdfs_fs_info *rfi, void *info, void *priv)
{
	struct rpdfs_net_tcp_info *ntinf = info;
	struct rpdfs_net_tcp_conn_private *tpriv = priv;

	tpriv->rfi = rfi;
	tpriv->ntinf = ntinf;
	init_llist_head(&tpriv->send_llhead);
	INIT_LIST_HEAD(&tpriv->send_list);
	INIT_WORK(&tpriv->send_work, rpdfs_net_tcp_send_work_fn);
	INIT_WORK(&tpriv->recv_work, rpdfs_net_tcp_recv_work_fn);
}

static int rpdfs_net_tcp_connect(struct rpdfs_fs_info *rfi, void *info, void *priv,
				 struct rpdfs_net_transport_addr *addr)
{
	struct rpdfs_net_tcp_conn_private *tpriv = priv;
	struct socket *sock = NULL;
	int ret;

	if (addr->sa.sa_family != AF_INET) {
		ret = -EAFNOSUPPORT;
		goto out;
	}

	tpriv->rx.hdr_ctl_page = alloc_page(GFP_NOFS);
	if (!tpriv->rx.hdr_ctl_page) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0)
		goto out;

	tpriv->sock = sock;
	tpriv->addr = *addr;

	tcp_sock_set_nodelay(sock->sk);
	set_callbacks(sock, tpriv);

	ret = kernel_connect(sock, &addr->sa, sizeof(addr->_sin), O_NONBLOCK);
	if (ret == -EINPROGRESS)
		ret = 0;
out:
	if (ret < 0) {
		rpdfs_prd_rfi(rfi, "connect to %pISpc ret %d", &addr->sa, ret);
		tpriv->sock = NULL;
		sock_release(sock);
	}
	return ret;
}

static void rpdfs_net_tcp_send(struct rpdfs_fs_info *rfi, void *info, void *priv,
			       void *pfrag, unsigned int hdr_ctl_size,
			       struct page *data_page, unsigned int data_size)
{
	struct rpdfs_net_tcp_info *ntinf = info;
	struct rpdfs_net_tcp_conn_private *tpriv = priv;
	struct rpdfs_net_tcp_send *tsend = pfrag;

	init_llist_node(&tsend->llnode);
	INIT_LIST_HEAD(&tsend->head);

	tsend->data_page = data_page;
	if (tsend->data_page)
		get_page(tsend->data_page);

	bvec_set_virt(&tsend->bvecs[0], &tsend->hdr, hdr_ctl_size);
	if (data_size)
		bvec_set_page(&tsend->bvecs[1], tsend->data_page, data_size, 0);
	iov_iter_bvec(&tsend->iter, ITER_SOURCE, tsend->bvecs, 1 + !!data_size,
		      hdr_ctl_size + data_size);

	rpdfs_prd("tsend %p queueing t %u cs %u ds %u",
		tsend, tsend->hdr.type, tsend->hdr.ctl_size, le16_to_cpu(tsend->hdr.data_size));

	smp_wmb(); /* tsend initialized before visible in llist */
	llist_add(&tsend->llnode, &tpriv->send_llhead);
	queue_work(ntinf->workq, &tpriv->send_work);
}

static void rpdfs_net_tcp_shutdown(struct rpdfs_fs_info *rfi, void *info, void *priv)
{
	struct rpdfs_net_tcp_conn_private *tpriv = priv;
	struct socket *sock = tpriv->sock;

	if (!sock)
		return;

	kernel_sock_shutdown(sock, SHUT_RDWR);

	lock_sock(sock->sk);
	restore_callbacks(sock, tpriv);
	release_sock(sock->sk);

	cancel_work_sync(&tpriv->recv_work);
	cancel_work_sync(&tpriv->send_work);

	sock_release(sock);
}

static void rpdfs_net_tcp_free(struct rpdfs_fs_info *rfi, void *info, void *priv)
{
	struct rpdfs_net_tcp_conn_private *tpriv = priv;
	struct rpdfs_net_tcp_send *tsend;
	struct rpdfs_net_tcp_send *tsend__;
	struct llist_node *llnode;

	llnode = llist_del_all(&tpriv->send_llhead);
	llist_for_each_entry_safe(tsend, tsend__, llnode, llnode)
		page_frag_free(tsend);
	list_for_each_entry_safe(tsend, tsend__, &tpriv->send_list, head)
		page_frag_free(tsend);

	if (tpriv->rx.hdr_ctl_page)
		put_page(tpriv->rx.hdr_ctl_page);
	if (tpriv->rx.md.data_page)
		put_page(tpriv->rx.md.data_page);
}

const struct rpdfs_net_transport_ops rpdfs_net_tcp_ops = {
	.priv_size		= sizeof(struct rpdfs_net_tcp_conn_private),
	.send_pfrag_head	= RPDFS_NET_TCP_SEND_PFRAG_HEAD,
	.setup			= rpdfs_net_tcp_setup,
	.destroy		= rpdfs_net_tcp_destroy,
	.init			= rpdfs_net_tcp_init,
	.connect		= rpdfs_net_tcp_connect,
	.send			= rpdfs_net_tcp_send,
	.shutdown		= rpdfs_net_tcp_shutdown,
	.free			= rpdfs_net_tcp_free,
};
