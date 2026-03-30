/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PLACE_H
#define RPDFS_PLACE_H

#include "format-msg.h"

#define RPF		"%llx%016llx"
#define RPA(place)	(u64)((place) >> 64), (u64)(place)

static inline u64 rpdfs_place_hi(u8 type, u64 ino, u8 depth)
{
	return ((type & RPDFS_PLACE_TYPE_MASK) << RPDFS_PLACE_TYPE_SHIFT) |
	       ((ino & RPDFS_PLACE_INO_MASK) << RPDFS_PLACE_INO_SHIFT) |
	       (depth & RPDFS_PLACE_DEPTH_MASK);
}

static inline u128 rpdfs_place_full(u8 type, u64 ino, u8 depth, u64 off)
{
	return ((u128)rpdfs_place_hi(type, ino, depth) << 64) | off;
}

static inline u64 rpdfs_place_hi_type(u64 place_hi)
{
	return (place_hi >> RPDFS_PLACE_TYPE_SHIFT) & RPDFS_PLACE_TYPE_MASK;
}

static inline u64 rpdfs_place_hi_ino(u64 place_hi)
{
	return (place_hi >> RPDFS_PLACE_INO_SHIFT) & RPDFS_PLACE_INO_MASK;
}

static inline u64 rpdfs_place_type(u128 place)
{
	return rpdfs_place_hi_type(place >> 64);
}

static inline u64 rpdfs_place_ino(u128 place)
{
	return rpdfs_place_hi_ino(place >> 64);
}

static inline void rpdfs_place_split_le(__le64 *lo, __le64 *hi, u128 full)
{
	*lo = cpu_to_le64((u64)full);
	*hi = cpu_to_le64(full >> 64);
}

static inline u128 rpdfs_place_combine_le(__le64 lo, __le64 hi)
{
	return ((u128)le64_to_cpu(hi) << 64) | le64_to_cpu(lo);
}

#endif
