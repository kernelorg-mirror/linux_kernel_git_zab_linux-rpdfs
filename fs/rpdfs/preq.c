/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/seqlock.h>
#include <linux/wait.h>
#include <linux/bitmap.h>

#include "format-block.h"
#include "map.h"
#include "net.h"
#include "pr.h"
#include "preq.h"
#include "seqlock.h"
#include "super.h"

/*
 * This parallel request layer manages sending multiple concurrent
 * requests to a set of devd servers, gathering and merging their
 * responses, and then either caching the result or handing it off to
 * consumers.  While each request flow is implemented with shared
 * helpers this is not generic and has specific knowledge of the
 * behavior of each request.
 */

struct parallel_request {
	seqlock_t seqlock;
	struct wait_queue_head waitq;
	ktime_t complete_until;
	ktime_t cache_duration;
	unsigned long nr_devds;
	unsigned long *in_flight_map;
	unsigned long nr_in_flight;
	int error;
	union {
		struct rpdfs_msg_block_counts_result bcr;
	};
};

struct rpdfs_preq_info {
	struct parallel_request block_counts;
};

static struct rpdfs_preq_info *RPDFS_PQINF(struct rpdfs_fs_info *rfi)
{
	return rfi->preq_info;
}

static void SET_RPDFS_PQINF(struct rpdfs_fs_info *rfi, struct rpdfs_preq_info *prinf)
{
	rfi->preq_info = prinf;
}

static void init_req(struct parallel_request *req, u64 cache_duration_ns)
{
	seqlock_init(&req->seqlock);
	init_waitqueue_head(&req->waitq);
	req->cache_duration = ns_to_ktime(cache_duration_ns);
}

static bool req_is_complete(struct parallel_request *req)
{
	return ktime_before(ktime_get(), req->complete_until);
}

static bool complete_or_idle(struct parallel_request *req)
{
	bool ret;

	while_read_seqretry(&req->seqlock)
		ret = req_is_complete(req) || req->nr_in_flight == 0;

	return ret;
}

/*
 * Process a response to one of the requests that's building up the
 * result of the parallel request.  Can return an error if the
 * contribution didn't make sense.
 */
static int finish_one_in_flight(struct parallel_request *req, unsigned long nr, int err)
{
	int ret;

	if (nr >= req->nr_devds || !test_and_clear_bit(nr, req->in_flight_map)) {
		ret = -EPROTO;
		goto out;
	}

	if (err && !req->error)
		req->error = err;

	if (--req->nr_in_flight == 0) {
		req->complete_until = ktime_add(ktime_get(), req->cache_duration);
		wake_up_all(&req->waitq);
	}

	ret = 0;
out:
	return ret;
}

static int try_send_messages(struct rpdfs_fs_info *rfi, struct parallel_request *req, u8 type,
			     void *ctl_buf, u16 ctl_size)
{
	struct rpdfs_net_transport_addr addr;
	struct rpdfs_net_message_desc md;
	unsigned long *map = NULL;
	unsigned long nr_devds;
	unsigned long nr;
	u64 qver;
	bool try;
	int ret;

	while_read_seqretry(&req->seqlock)
		try = !req_is_complete(req) && req->nr_in_flight == 0;
	if (try) {
		ret = rpdfs_map_nr_devds(rfi, &qver);
		if (ret < 0)
			goto out;
		nr_devds = ret;

		map = kmalloc(DIV_ROUND_UP(nr_devds, BITS_PER_LONG) * sizeof(long), GFP_NOFS);
		if (!map) {
			ret = -ENOMEM;
			goto out;
		}

		write_seqlock(&req->seqlock);
		try = !req_is_complete(req) && req->nr_in_flight == 0;
		if (try) {
			if (req->nr_devds != nr_devds)
				swap(map, req->in_flight_map);
			req->nr_devds = nr_devds;
			bitmap_fill(req->in_flight_map, nr_devds);
			req->nr_in_flight = nr_devds;
			req->error = 0;
			memset(&req->bcr, 0, sizeof(req->bcr));
		}
		write_sequnlock(&req->seqlock);
	}
	if (!try) {
		ret = 0;
		goto out;
	}

	md = (struct rpdfs_net_message_desc) {
		.type = type,
		.ctl_buf = ctl_buf,
		.ctl_size = ctl_size,
		.data_page = NULL,
		.data_size = 0,
	};

	for (nr = 0; nr < nr_devds; nr++) {
		/* XXX probably compare qver instead of overwrite */
		ret = rpdfs_map_nth_addr(rfi, nr, &addr, &qver);
		if (ret == 0)
			ret = rpdfs_net_send(rfi, &addr, &md, GFP_NOFS);
		if (ret < 0) {
			write_seqlock(&req->seqlock);
			ret = finish_one_in_flight(req, nr, ret);
			write_sequnlock(&req->seqlock);
		}
	}
out:
	kfree(map);
	return ret;
}

static int recv_block_counts_result(struct rpdfs_fs_info *rfi, struct rpdfs_net_message_desc *md)
{
	struct rpdfs_preq_info *prinf = RPDFS_PQINF(rfi);
	struct rpdfs_msg_block_counts_result *bcr = md->ctl_buf;
	struct parallel_request *req = &prinf->block_counts;
	int ret;

	if ((md->ctl_size != sizeof(struct rpdfs_msg_block_counts_result)) ||
	    (md->data_size != 0)) {
		ret = -EPROTO;
		goto out;
	}

	write_seqlock(&req->seqlock);

	ret = finish_one_in_flight(req, md->sender_nth_devd, 0); /* XXX no err? */
	if (ret == 0) {
		le64_add_cpu(&req->bcr.allocated, le64_to_cpu(bcr->allocated));
		le64_add_cpu(&req->bcr.inodes, le64_to_cpu(bcr->inodes));
		le64_add_cpu(&req->bcr.total, le64_to_cpu(bcr->total));
	}
	if (ret < 0 && ret != -EPROTO)
		ret = 0;

	write_sequnlock(&req->seqlock);

out:
	return ret;
}

int rpdfs_preq_block_counts(struct rpdfs_fs_info *rfi, struct rpdfs_msg_block_counts_result *bcr)
{
	struct rpdfs_preq_info *prinf = RPDFS_PQINF(rfi);
	struct parallel_request *req = &prinf->block_counts;
	bool complete;
	int ret;

	do {
		while_read_seqretry(&req->seqlock) {
			complete = req_is_complete(req);
			if (complete) {
				if (!req->error) {
					*bcr = req->bcr;
					ret = 0;
				} else {
					ret = req->error;
				}
			}
		}
		if (complete)
			break;

		ret = try_send_messages(rfi, req, RPDFS_MSG_BLOCK_COUNTS_REQUEST, NULL, 0) ?:
		      wait_event_interruptible(req->waitq, complete_or_idle(req));
	} while (ret == 0);

	return ret;
}

int rpdfs_preq_setup(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_preq_info *prinf;
	int ret;

	prinf = kzalloc(sizeof(struct rpdfs_preq_info), GFP_KERNEL);
	if (!prinf) {
		ret = -ENOMEM;
		goto out;
	}

	/* arbitrary limit on request frequency to seem reasonable to a human */
	init_req(&prinf->block_counts, NSEC_PER_SEC / 4);

	ret = rpdfs_net_register_recv(rfi, RPDFS_MSG_BLOCK_COUNTS_RESULT, recv_block_counts_result);
	if (ret < 0)
		goto out;

	SET_RPDFS_PQINF(rfi, prinf);
	ret = 0;
out:
	if (ret < 0)
		kfree(prinf);
	return ret;
}

void rpdfs_preq_destroy(struct rpdfs_fs_info *rfi)
{
	struct rpdfs_preq_info *prinf = RPDFS_PQINF(rfi);

	if (prinf) {
		rpdfs_net_unregister_recv(rfi, RPDFS_MSG_BLOCK_COUNTS_RESULT,
					  recv_block_counts_result);
		kfree(prinf->block_counts.in_flight_map);
		kfree(prinf);
		SET_RPDFS_PQINF(rfi, NULL);
	}
}
