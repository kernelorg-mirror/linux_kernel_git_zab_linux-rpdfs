/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_FORMAT_MSG_H
#define RPDFS_SHARED_FORMAT_MSG_H

#include <linux/math.h>

#include "format-block.h"

enum {
	RPDFS_MSG_BLOCK_READ = 0,
	RPDFS_MSG_BLOCK_READ_RESULT,
	RPDFS_MSG_BLOCK_WRITE,
	RPDFS_MSG_BLOCK_WRITE_RESULT,
	RPDFS_MSG_BLOCK_REQUEST_MODE,
	RPDFS_MSG_BLOCK_GRANT_MODE,
	RPDFS_MSG_BLOCK_REVOKE_MODE,
	RPDFS_MSG_BLOCK_CONFIRM_MODE,
	RPDFS_MSG_FREE_STRIPE_REQUEST,
	RPDFS_MSG_FREE_STRIPE_GRANT,
	RPDFS_MSG__NR,
};

enum {
	RPDFS_MSG_ERR_OK = 0,
	RPDFS_MSG_ERR_EIO,
	RPDFS_MSG_ERR_ENOMEM,
	RPDFS_MSG_ERR__INVALID,
};

/*
 * _NULL is 0 so we can have natural if (mode) tests.  _NONE is the mode
 * that grants no access and causes the client and server to free
 * tracking of the cached block.
 *
 * The modes are also numerically sorted from least to most restrictive.
 * NONE and NULL are compatible with everything, read isn't compatible
 * with write, and write isn't compatible with write.
 */
enum {
	RPDFS_CACHE_MODE_NULL = 0,
	RPDFS_CACHE_MODE_NONE,
	RPDFS_CACHE_MODE_READ,
	RPDFS_CACHE_MODE_WRITE,
	RPDFS_CACHE_MODE__INVALID,
};

#define RPDFS_CACHE_MODE__BITS	bits_per(RPDFS_CACHE_MODE__INVALID - 1)

struct rpdfs_msg_header {
	__le32 crc;
	__le16 data_size;
	__u8 ctl_size;
	__u8 type;
};

#define RPDFS_MSG_MAX_CTL_SIZE	255
#define RPDFS_MSG_MAX_DATA_SIZE 4096

struct rpdfs_msg_block_read {
	__le64 bnr;
	__le64 flags;
	__u8 request_mode;
	__u8 _pad[7];
};

#define RPDFS_MSG_BLOCK_READ_FLAG_DATA	(1 << 0)

/*
 * A block's place is a 128bit value that uniquely identifies the
 * logical location of the block in the global fs block space.  The
 * value is designed so that regular forward block traversal sees
 * increasing place values.  (f.e.: inode block < data mapping blocks <
 * data blocks).
 *
 * Over the wire the place is communicated as 2 64bit values and these
 * shifts and masks encode the first three fields into the hi word.
 */
#define RPDFS_PLACE_DEPTH_BITS	4
#define RPDFS_PLACE_INO_BITS	(64 - RPDFS_BLOCK_SHIFT)
#define RPDFS_PLACE_TYPE_BITS	4

#define RPDFS_PLACE_INO_SHIFT	RPDFS_PLACE_DEPTH_BITS
#define RPDFS_PLACE_TYPE_SHIFT	(RPDFS_PLACE_INO_SHIFT + RPDFS_PLACE_INO_BITS)

#define RPDFS_PLACE_DEPTH_MASK	((1ULL << RPDFS_PLACE_DEPTH_BITS) - 1)
#define RPDFS_PLACE_INO_MASK	((1ULL << RPDFS_PLACE_INO_BITS) - 1)
#define RPDFS_PLACE_TYPE_MASK	((1ULL << RPDFS_PLACE_TYPE_BITS) - 1)

#define RPDFS_PLACE_INODE		4
#define RPDFS_PLACE_XATTR_BTREE		8
#define RPDFS_PLACE_DIRENT_BTREE	12

struct rpdfs_msg_block_details {
	__le64 alloc_ctr;
	__le64 wcount;
	__le64 place_lo;
	__le64 place_hi;
};

struct rpdfs_msg_block_read_result {
	__le64 bnr;
	struct rpdfs_msg_block_details det;
	__u8 grant_mode;
	__u8 err;
	__u8 _pad[6];
};

struct rpdfs_msg_block_write {
	__le64 bnr;
	struct rpdfs_msg_block_details det;
	__u8 confirm_mode;
	__u8 _pad[7];
};

struct rpdfs_msg_block_write_result {
	__le64 bnr;
	__u8 err;
	__u8 _pad[7];
};

struct rpdfs_msg_cache_mode {
	__le64 bnr;
	__u8 mode;
	__u8 _pad[7];
};

/*
 * The bnr is the first fs bnr in the stripe that should be found on the
 * receiving devd.  It is ignored (and must be 0) if _SEARCH is set
 * which tells devd to find a free stripe rather than trying to get
 * write grants in the specific stripe.
 */
struct rpdfs_msg_free_stripe_request {
	__le64 bnr;
	__u8 _pad[7];
	__u8 flags;
};
#define RPDFS_MSG_FREE_STRIPE_REQUEST_FLAG_SEARCH	(1 << 0)

/*
 * This matches the number of dev_details entries in each details block
 * in devd.
 */
#define RPDFS_MSG_BLOCKS_PER_FREE_STRIPE 127

/*
 * Each grant covers a fixed number of blocks.  The bmap indicates which
 * covered block numbers are found in the details array in the data
 * payload.
 *
 * The bnr is the first possible block number.
 */
struct rpdfs_msg_free_stripe_grant {
	__le64 bnr;
	__u8 _pad[7];
	__u8 flags;
	__le64 bmap[DIV_ROUND_UP(RPDFS_MSG_BLOCKS_PER_FREE_STRIPE, 64)];
};

#define RPDFS_MSG_FREE_STRIPE_GRANT_FLAG_SEARCH		(1 << 0)

/*
 * These are sent as the data payload in FREE_STRIPE_GRANT messages.
 * The data size should reflect the number of bits set in the ctl's bmap
 * bitmap.
 */
struct rpdfs_msg_free_stripe_detail {
	__le64 alloc_ctr;
	__le64 wcount;
};

#endif
