/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_STRING_H
#define RPDFS_STRING_H

/*
 * Copy as much of the source buffer as fits into the destination
 * buffer.  If the destination is larger then zero the remainder of the
 * destination.  The buffers must not overlap (right there in the memcpy
 * name, not memmove).
 */
static inline void memcpy_and_zero_tail(void *dst, size_t dst_size, void *src, size_t src_size)
{
	size_t tail = dst_size - src_size;

	memcpy(dst, src, min(dst_size, src_size));
	if (tail > 0)
		memset(dst + src_size, 0, tail);
}

#endif
