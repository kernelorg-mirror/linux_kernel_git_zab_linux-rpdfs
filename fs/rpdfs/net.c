/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/rhashtable.h>
#include <linux/refcount.h>
#include <linux/rcupdate.h>
#include <linux/socket.h>
#include <linux/percpu.h>
#include <linux/kmemleak.h>

#include "format-block.h"
#include "net.h"
#include "pr.h"
#include "super.h"

struct rpdfs_net_pcpu_pfrag {
	struct page_frag_cache cache;
	void *preloaded;
	bool have_cache;
};

struct rpdfs_net_info {
	struct rhashtable conn_ht;
	struct workqueue_struct *workq;
	struct rpdfs_net_pcpu_pfrag __percpu *pcpu_pfrag;
	const struct rpdfs_net_transport_ops *ops;
	void *ops_info;
	rpdfs_net_recv_fn_t recv_fns[RPDFS_MSG__NR];
};

static struct rpdfs_net_info *RPDFS_NINF(struct rpdfs_fs_info *rfi)
{
	return rfi->net_info;
}

static void SET_RPDFS_NINF(struct rpdfs_fs_info *rfi, struct rpdfs_net_info *ninf)
{
	rfi->net_info = ninf;
}

struct rpdfs_net_connection {
	struct rhash_head rhead;
	struct rpdfs_fs_info *rfi;
	struct work_struct shutdown_work;
	struct rpdfs_net_transport_addr addr;
	unsigned long peer_nth_devd;
	u64 qver;
	refcount_t refcount;
	struct rcu_head rcu;
	u8 priv[] __aligned(ARCH_KMALLOC_MINALIGN);
};

/*
 * A deep comparison so that we don't have to worry about uninitialized
 * portions of the full addr union.  (Caller treats it like memcmp, but
 * only tests that the output is non-zero, so it's more like "bool
 * not_equal()")
 */
static int cmp_key_addr_conn_addr(struct rhashtable_compare_arg *arg, const void *obj)
{
	const struct rpdfs_net_transport_addr *a = arg->key;
	const struct rpdfs_net_connection *conn = obj;
	const struct rpdfs_net_transport_addr *b = &conn->addr;

	return a->sa.sa_family != b->sa.sa_family ||
	       (a->sa.sa_family == AF_INET &&
	        (a->_sin.sin_port != b->_sin.sin_port ||
	         a->_sin.sin_addr.s_addr != b->_sin.sin_addr.s_addr));
}

static const struct rhashtable_params rpdfs_net_conn_ht_params = {
	.key_len	= sizeof_field(struct rpdfs_net_connection, addr),
	.key_offset	= offsetof(struct rpdfs_net_connection, addr),
	.head_offset	= offsetof(struct rpdfs_net_connection, rhead),
	.obj_cmpfn	= cmp_key_addr_conn_addr,
};

/*
 * Return with an elevated refcount that must be put when done.
 */
static struct rpdfs_net_connection *get_conn(struct rpdfs_net_info *ninf,
					     struct rpdfs_net_transport_addr *addr)
{
	struct rpdfs_net_connection *conn = NULL;

	rcu_read_lock();
	conn = rhashtable_lookup(&ninf->conn_ht, addr, rpdfs_net_conn_ht_params);
	if (conn && !refcount_inc_not_zero(&conn->refcount))
		conn = NULL;
	rcu_read_unlock();

	if (conn)
		rpdfs_prd("conn %p rc %u", conn, refcount_read(&conn->refcount));

	return conn;
}

static void put_conn(struct rpdfs_fs_info *rfi, struct rpdfs_net_info *ninf,
		     struct rpdfs_net_connection *conn)
{
	if (!IS_ERR_OR_NULL(conn) && refcount_dec_and_test(&conn->refcount)) {
		rpdfs_prd("freeing conn %p", conn);
		ninf->ops->free(rfi, ninf->ops_info, conn->priv);
		kfree_rcu(conn, rcu);
	}
}

/*
 * During buildup this is called directly by connect so that the failed
 * connection is shut down before a failed connect returns.  For
 * connections that were established its performed by queued work.  Many
 * shutdown attempts can requeue the work while it's running so we use
 * removal from the hash as the indication to shutdown.
 */
static void shutdown_conn(struct rpdfs_fs_info *rfi, struct rpdfs_net_info *ninf,
			  struct rpdfs_net_connection *conn)
{
	int ret;

	ret = rhashtable_remove_fast(&ninf->conn_ht, &conn->rhead, rpdfs_net_conn_ht_params);
	if (ret == 0) {
		ninf->ops->shutdown(rfi, ninf->ops_info, conn->priv);
		put_conn(rfi, ninf, conn);
	} else {
		WARN_ON_ONCE(ret != -ENOENT);
	}
}

/*
 * Transports can trigger conn shutdown from within our ->transport
 * calls, or within their recv processing contexts.  We queue shutdown
 * over in its own work context so that the transport call pattern
 * matches allocation and initialization.
 */
static void rpdfs_net_shutdown_work_fn(struct work_struct *work)
{
	struct rpdfs_net_connection *conn =
		container_of(work, struct rpdfs_net_connection, shutdown_work);
	struct rpdfs_fs_info *rfi = conn->rfi;
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);

	shutdown_conn(rfi, ninf, conn);
	put_conn(rfi, ninf, conn);
}

/*
 * Give the caller exclusive access to a pfrag cache.
 */
static void borrow_pfrag_cache(struct rpdfs_net_pcpu_pfrag *pcpf, struct page_frag_cache *cache)
{
	if (!pcpf->have_cache)
		page_frag_cache_init(&pcpf->cache);
	else
		pcpf->have_cache = false;

	*cache = pcpf->cache;
}

static void return_pfrag_cache(struct rpdfs_net_pcpu_pfrag *pcpf, struct page_frag_cache *cache)
{
	if (!pcpf->have_cache) {
		pcpf->cache = *cache;
		pcpf->have_cache = true;
	} else {
		page_frag_cache_drain(cache);
	}
}

/*
 * If this returns 0 then pre-emption has been disabled and the next
 * _send() will have sufficient resources to not block.  It can still
 * return errors.  The caller must call _preload_end() to re-enable
 * preemption once they're done.
 */
int rpdfs_net_preload(struct rpdfs_fs_info *rfi, gfp_t gfp)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_pcpu_pfrag *pcpf;
	struct page_frag_cache cache;
	void *pfrag;
	int ret;

	WARN_ON_ONCE(!gfpflags_allow_blocking(gfp));

	pcpf = get_cpu_ptr(ninf->pcpu_pfrag);
	if (pcpf->preloaded) {
		ret = 0;
		goto out;
	}

	borrow_pfrag_cache(pcpf, &cache);
	put_cpu_ptr(ninf->pcpu_pfrag);

	/* preload->use can cross cgroups, don't account */
	pfrag = page_frag_alloc_align(&cache, ninf->ops->send_pfrag_head +
				      sizeof(struct rpdfs_msg_header) + RPDFS_MSG_MAX_CTL_SIZE,
				      gfp & ~__GFP_ACCOUNT, ARCH_KMALLOC_MINALIGN);

	pcpf = get_cpu_ptr(ninf->pcpu_pfrag);
	return_pfrag_cache(pcpf, &cache);
	if (pfrag) {
		if (pcpf->preloaded == NULL)
			pcpf->preloaded = pfrag;
		else
			page_frag_free(pfrag);
		ret = 0;
	} else {
		ret = -ENOMEM;
		put_cpu_ptr(ninf->pcpu_pfrag);
	}
out:
	return ret;
}

void rpdfs_net_preload_end(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);

	put_cpu_ptr(ninf->pcpu_pfrag);
}

/*
 * Modeled after radix tree preloading.  If we can't block and aren't in
 * an interrupt then we try normal allocation, and if that fails use the
 * preloaded allocation.
 */
static void *alloc_pfrag_preloaded(struct rpdfs_net_info *ninf, unsigned int size, gfp_t gfp)
{
	struct rpdfs_net_pcpu_pfrag *pcpf;
	struct page_frag_cache cache;
	bool try_preloaded = false;
	void *pfrag;

	try_preloaded = !gfpflags_allow_blocking(gfp) && !in_interrupt();
	if (try_preloaded)
		gfp |= __GFP_NOWARN;

	pcpf = get_cpu_ptr(ninf->pcpu_pfrag);
	borrow_pfrag_cache(pcpf, &cache);
	put_cpu_ptr(ninf->pcpu_pfrag);

	pfrag = page_frag_alloc_align(&cache, size, gfp | __GFP_NOWARN, ARCH_KMALLOC_MINALIGN);

	pcpf = get_cpu_ptr(ninf->pcpu_pfrag);
	return_pfrag_cache(pcpf, &cache);
	if (!pfrag && try_preloaded && pcpf->preloaded) {
		pfrag = pcpf->preloaded;
		pcpf->preloaded = NULL;
		kmemleak_update_trace(pfrag);
	}
	put_cpu_ptr(ninf->pcpu_pfrag);

	return pfrag;
}

/*
 * If this is called between _preload() and _preload_end() then it won't
 * sleep and is safe to call from locked contexts.  If not, it can sleep
 * allocating and might return allocation errors.
 */
int rpdfs_net_send(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr,
		   struct rpdfs_net_message_desc *md, gfp_t gfp)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_connection *conn = NULL;
	struct rpdfs_msg_header *hdr;
	void *pfrag = NULL;
	int ret;

	if (WARN_ON_ONCE(rpdfs_net_invalid_sizes(md) ||
			 (md->data_size > 0 && md->data_page == NULL))) {
		ret = -EINVAL;
		goto out;
	}

	conn = get_conn(ninf, addr);
	if (!conn) {
		ret = -ENOTCONN;
		goto out;
	}

	pfrag = alloc_pfrag_preloaded(ninf, ninf->ops->send_pfrag_head +
				      sizeof(struct rpdfs_msg_header) + md->ctl_size, gfp);
	if (!pfrag) {
		ret = -ENOMEM;
		goto out;
	}

	hdr = pfrag + ninf->ops->send_pfrag_head;
	hdr->data_size = cpu_to_le16(md->data_size);
	hdr->ctl_size = md->ctl_size;
	hdr->type = md->type;
	if (md->ctl_size)
		memcpy((void *)hdr + sizeof(struct rpdfs_msg_header), md->ctl_buf, md->ctl_size);

	/* send is responsible for freeing pfrag */
	ninf->ops->send(rfi, ninf->ops_info, conn->priv, pfrag,
			sizeof(struct rpdfs_msg_header) + md->ctl_size,
			md->data_page, md->data_size);
	ret = 0;
out:
	put_conn(rfi, ninf, conn);
	if (ret < 0)
		rpdfs_prd_rfi(rfi, "send addr %pISpc err %d", &addr->sa, ret);

	return ret;
}

int rpdfs_net_register_recv(struct rpdfs_fs_info *rfi, u8 type, rpdfs_net_recv_fn_t recv_fn)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	rpdfs_net_recv_fn_t old;
	int ret;

	if (type >= ARRAY_SIZE(ninf->recv_fns)) {
		ret = -EINVAL;
	} else {
		old = cmpxchg(&ninf->recv_fns[type], NULL, recv_fn);
		ret = old == NULL ? 0 : -EEXIST;
	}

	return ret;
}

/*
 * This doesn't mind trying to unregister a fn that wasn't registered so
 * that shutdown doesn't need to maintain state that it had registered.
 */
void rpdfs_net_unregister_recv(struct rpdfs_fs_info *rfi, u8 type, rpdfs_net_recv_fn_t recv_fn)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);

	if (type < ARRAY_SIZE(ninf->recv_fns))
		cmpxchg(&ninf->recv_fns[type], recv_fn, NULL);
}

bool rpdfs_net_invalid_sizes(struct rpdfs_net_message_desc *md)
{
	return md->ctl_size > RPDFS_MSG_MAX_CTL_SIZE ||
	       md->data_size > RPDFS_MSG_MAX_DATA_SIZE;
}

/*
 * Receive an incoming message.  The transport has only received the
 * buffers, it hasn't checked the contents.
 *
 * Returning an error from the recv_fn triggers a shutdown of the
 * connection.
 */
void rpdfs_net_recv(struct rpdfs_fs_info *rfi, void *priv, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_connection *conn = container_of(priv, struct rpdfs_net_connection, priv);
	rpdfs_net_recv_fn_t recv_fn;
	int ret;

	if (md->type < ARRAY_SIZE(ninf->recv_fns) && (recv_fn = ninf->recv_fns[md->type])) {
		md->sender_nth_devd = conn->peer_nth_devd;
		md->conn_qver = conn->qver;
		ret = recv_fn(rfi, md);
	} else {
		ret = -EPROTO;
	}

	if (ret < 0) {
		rpdfs_err("recv fn err %d for %u on %pISpc, shutting down",
			  ret, md->type, &conn->addr.sa);
		rpdfs_net_shutdown_conn(rfi, priv, ret);
	}
}

/*
 * Establish a connection through a transport to a remote address.  The
 * connection will be considered live as connect is being called.  The
 * transport is responsible for queueing sends that are called while the
 * connection is coming up.  We are ready for incoming transport calls
 * for the connection before the successful connect call returns.
 *
 * If the caller tries to connect to an address that is already
 * connected then the old connection is torn down.  The presumption is
 * that the caller has another endpoint that is now present at the
 * address.  (map versions protect conflicting use of the same address).
 */
int rpdfs_net_connect(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr,
		      unsigned long n, u64 qver)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_connection *conn = NULL;
	struct rpdfs_net_connection *exist;
	int ret;

	conn = kzalloc(sizeof(struct rpdfs_net_connection) + ninf->ops->priv_size, GFP_NOFS);
	if (!conn) {
		ret = -ENOMEM;
		goto out;
	}

	conn->rfi = rfi;
	INIT_WORK(&conn->shutdown_work, rpdfs_net_shutdown_work_fn);
	conn->addr = *addr;
	conn->peer_nth_devd = n;
	conn->qver = qver;
	refcount_set(&conn->refcount, 1);
	ninf->ops->init(rfi, ninf->ops_info, conn->priv);

	/* sends and shutdown attempts can see the conn once present in the hash */
	refcount_inc(&conn->refcount);
	for (;;) {
		rcu_read_lock();
		exist = rhashtable_lookup_get_insert_fast(&ninf->conn_ht, &conn->rhead,
							  rpdfs_net_conn_ht_params);
		rcu_read_unlock();
		if (!IS_ERR_OR_NULL(exist))
			shutdown_conn(rfi, ninf, exist);
		else
			break;
	}

	/* hard failure to insert into the hash table */
	if (IS_ERR(exist)) {
		put_conn(rfi, ninf, conn);
		ret = PTR_ERR(exist);
		goto out;
	}

	/* conn is considered live during connect, transport can call core before returning */
	ret = ninf->ops->connect(rfi, ninf->ops_info, conn->priv, addr);
	if (ret < 0)
		shutdown_conn(rfi, ninf, conn);
out:
	put_conn(rfi, ninf, conn);
	return ret;
}

/*
 * This is async.  The conn can still be up when this returns.  Maybe it
 * shouldn't be.
 */
void rpdfs_net_disconnect(struct rpdfs_fs_info *rfi, struct rpdfs_net_transport_addr *addr)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_connection *conn;

	conn = get_conn(ninf, addr);
	if (conn) {
		rpdfs_net_shutdown_conn(rfi, conn->priv, 0);
		put_conn(rfi, ninf, conn);
	}
}

/*
 * A transport has seen an error on a connection.  We turn around and
 * ask them to shut down so that we maintain a consistent calling
 * pattern for transport ops.
 */
void rpdfs_net_shutdown_conn(struct rpdfs_fs_info *rfi, void *priv, int nerrno)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_connection *conn = container_of(priv, struct rpdfs_net_connection, priv);

	if (refcount_inc_not_zero(&conn->refcount) &&
	    !queue_work(ninf->workq, &conn->shutdown_work))
		put_conn(rfi, ninf, conn);
}

/*
 * Map an error in the protocol to a negative host errno.
 */
int rpdfs_net_nerrno(u8 err)
{
	static int nerrnos[] = {
		[RPDFS_MSG_ERR_EIO] = EIO,
		[RPDFS_MSG_ERR_ENOMEM] = ENOMEM,
	};

	return err >= ARRAY_SIZE(nerrnos) ? -EIO : -nerrnos[err];
}

/*
 * Map a negative host errno to an error for the protocol.
 */
u8 rpdfs_net_err(int nerrno)
{
	switch (nerrno) {
		case -EIO: return RPDFS_MSG_ERR_EIO;
		case -ENOMEM: default: return RPDFS_MSG_ERR_EIO;
	}
}

int rpdfs_net_setup(struct rpdfs_fs_info *rfi, const struct rpdfs_net_transport_ops *ops)
{
	struct rpdfs_net_info *ninf = NULL;
	struct rpdfs_net_pcpu_pfrag *pcpf;
	int cpu;
	int ret;

	/* arbitrary, to limit pfrag preload of a max ctl_size to smaller than a page */
	if (WARN_ON_ONCE(ops->send_pfrag_head > 128) ||
	    WARN_ON_ONCE(ops->send_pfrag_head % sizeof(u64))) {
		ret = -EINVAL;
		goto out;
	}

	ninf = kzalloc(sizeof(struct rpdfs_net_info), GFP_NOFS);
	if (ninf) {
		ninf->workq = alloc_workqueue("rpdfs-net", WQ_MEM_RECLAIM, 0);
		ninf->pcpu_pfrag = alloc_percpu(struct rpdfs_net_pcpu_pfrag);
		ninf->ops_info = ops->setup(rfi);
	}
	if (!ninf || !ninf->workq || !ninf->pcpu_pfrag || !ninf->ops_info) {
		ret = -ENOMEM;
		goto out;
	}

	ninf->ops = ops;

	for_each_possible_cpu(cpu) {
		pcpf = per_cpu_ptr(ninf->pcpu_pfrag, cpu);
		page_frag_cache_init(&pcpf->cache);
		pcpf->have_cache = true;
	}

	ret = rhashtable_init(&ninf->conn_ht, &rpdfs_net_conn_ht_params);
	if (ret < 0)
		goto out;

	SET_RPDFS_NINF(rfi, ninf);
	ret = 0;
out:
	if (ret < 0 && ninf) {
		if (ninf->workq)
			destroy_workqueue(ninf->workq);
		if (ninf->ops_info)
			ops->destroy(rfi, ninf->ops_info);
		free_percpu(ninf->pcpu_pfrag);
		kfree(ninf);
	}
	return ret;
}

/*
 * Queue shutdown work on all conns so that when the workqueue is
 * drained all the conns will have been removed and freed.
 */
static void queue_shutdown_all(struct rpdfs_fs_info *rfi, struct rpdfs_net_info *ninf)
{
	struct rpdfs_net_connection *conn;
	struct rhashtable_iter iter;

	rhashtable_walk_enter(&ninf->conn_ht, &iter);
	do {
		rhashtable_walk_start(&iter);

		while (!IS_ERR_OR_NULL((conn = rhashtable_walk_next(&iter))))
			rpdfs_net_shutdown_conn(rfi, conn->priv, 0);

		rhashtable_walk_stop(&iter);

	} while (conn == ERR_PTR(-EAGAIN) && ({ cpu_relax(); cond_resched(); true; }));
	rhashtable_walk_exit(&iter);
}

void rpdfs_net_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_net_info *ninf = RPDFS_NINF(rfi);
	struct rpdfs_net_pcpu_pfrag *pcpf;
	int cpu;

	if (ninf) {
		queue_shutdown_all(rfi, ninf);
		destroy_workqueue(ninf->workq);
		rhashtable_destroy(&ninf->conn_ht);
		ninf->ops->destroy(rfi, ninf->ops_info);

		for_each_possible_cpu(cpu) {
			pcpf = per_cpu_ptr(ninf->pcpu_pfrag, cpu);
			if (pcpf->preloaded)
				page_frag_free(pcpf->preloaded);
			if (pcpf->have_cache)
				page_frag_cache_drain(&pcpf->cache);
		}
		free_percpu(ninf->pcpu_pfrag);

		kfree(ninf);
		SET_RPDFS_NINF(rfi, NULL);
	}
}
