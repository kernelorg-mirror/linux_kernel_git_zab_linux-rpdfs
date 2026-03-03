/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SEQLOCK_H
#define RPDFS_SEQLOCK_H

/*
 * Execute the for() loop statement at least once, and continue
 * executing it as long as read_seqretry() says we have to.  The caller
 * can safely break out of the statement but they can't trust any output
 * of the statement if they do so.
 */
#define while_read_seqretry(seql) \
	for (unsigned seq__, retry__ = 1; \
	     retry__ && ({ seq__ = read_seqbegin(seql); true; }); \
	     retry__ = read_seqretry((seql), seq__))

#endif
