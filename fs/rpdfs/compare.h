/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_COMPARE_H
#define RPDFS_COMPARE_H

/* is this worth it? */
#define rpdfs_compare(a, b)			\
({						\
	typeof(a) a_ = (a);			\
	typeof(b) b_ = (b);			\
						\
	a_ < b_ ? -1 : a_ > b_ ? 1 : 0;		\
})

static inline bool rpdfs_names_match(const char *a, unsigned a_len, const char *b, unsigned b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

#endif
