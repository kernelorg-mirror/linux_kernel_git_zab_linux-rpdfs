/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_LISTS_H
#define RPDFS_LISTS_H

/*
 * Add the members of the llist to the tail of the list_head list in
 * reverse order, returning the number of members added.  The llist
 * nodes are re-initialized.
 */
#define llist_reverse_add_tail(pos_, node_, node_member_, head_, head_member_)	\
({										\
	LIST_HEAD(reverse_);							\
	unsigned int count_ = 0;						\
	__typeof__(pos_) n_;							\
										\
	llist_for_each_entry_safe(pos_, n_, node_, node_member_) {		\
		init_llist_node(&(pos_)->node_member_);				\
		list_add(&(pos_)->head_member_, &reverse_);			\
		count_++;							\
	}									\
	list_splice_tail(&reverse_, head_);					\
										\
	count_;									\
})

#endif
