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
	RPDFS_MSG_BLOCK_COUNTS_REQUEST,
	RPDFS_MSG_BLOCK_COUNTS_RESULT,
	RPDFS_MSG_RLOCK_REQUEST,
	RPDFS_MSG_RLOCK_GRANT,
	RPDFS_MSG_RLOCK_REVOKE,
	RPDFS_MSG_RLOCK_CONFIRM,
	RPDFS_MSG_RLOCK_RELEASE,
	RPDFS_MSG__NR,
};

enum {
	RPDFS_MSG_ERR_OK = 0,
	RPDFS_MSG_ERR_EIO,
	RPDFS_MSG_ERR_ENOMEM,
	RPDFS_MSG_ERR__INVALID,
};

enum {

	/*
	 * 0 so that we can have natural if (mode) tests.
	 */
	RPDFS_RLOCK_MODE_NULL = 0,
	/*
	 * The client has rlock but not for read or write ops.  Is
	 * compatible with all other modes.  (unused for now, but will
	 * be used to track presence in client caches that prevent
	 * deletion.)
	 */
	RPDFS_RLOCK_MODE_NONE,
	/*
	 * Shared read -- excludes write modes and blocks can be read.
	 */
	RPDFS_RLOCK_MODE_SH_RD,
	/*
	 * Exclusive write -- excludes write and read modes, blocks can be both read
	 * and written.
	 */
	RPDFS_RLOCK_MODE_EX_WR,
	RPDFS_RLOCK_MODE__INVALID,
};

/*
 * _NULL is 0 so we can have natural if (mode) tests.  _NONE is the mode
 * that grants no rlock and causes the client and server to free
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

/*
 * k[0,1] are the inode_nr.  k[2] is a packing of a u8 key and u56
 * index.
 */
struct rpdfs_block_key {
	__le64 k[3];
};

/* the high byte of k[2] is the type */
#define RPDFS_BLOCK_KEY_TYPE__SHIFT	(64 - 8)
/* the low bits of k[2] is the index for the type */
#define RPDFS_BLOCK_KEY_INDEX__MASK	((1ULL << RPDFS_BLOCK_KEY_TYPE__SHIFT) - 1)

#define RPDFS_BLOCK_KEY_TYPE_INODE	0
#define RPDFS_BLOCK_KEY_TYPE_XATTR	1
#define RPDFS_BLOCK_KEY_TYPE_DIRENT	2
#define RPDFS_BLOCK_KEY_TYPE_DATA	3
#define RPDFS_BLOCK_KEY_TYPE__INVALID	4

static inline u8 rpdfs_block_key_type(struct rpdfs_block_key *key)
{
	return le64_to_cpu(key->k[2]) >> RPDFS_BLOCK_KEY_TYPE__SHIFT;
}

struct rpdfs_msg_block_read {
	struct rpdfs_block_key key;
};

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
/* free is always last so that it's flushed after other blocks in its txn */
#define RPDFS_PLACE_FREE		RPDFS_PLACE_TYPE_MASK

struct rpdfs_msg_block_details {
	__le64 alloc_ctr;
	__le64 wcount;
	__le64 place_lo;
	__le64 place_hi;
	__le64 crc;
};

struct rpdfs_msg_block_read_result {
	struct rpdfs_block_key key;
	struct rpdfs_msg_block_details det;
	__u8 _pad[7];
	__u8 err;
};

struct rpdfs_msg_block_write {
	struct rpdfs_block_key key;
	struct rpdfs_msg_block_details det;
};

struct rpdfs_msg_block_write_result {
	struct rpdfs_block_key key;
	__u8 _pad[7];
	__u8 err;
};

struct rpdfs_rlock_key {
	__le64 k[2];
};

struct rpdfs_msg_rlock {
	struct rpdfs_rlock_key key;
	__u8 _pad[6];
	__u8 mode;
	__u8 flags;
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
#define RPDFS_MSG_BLOCKS_PER_FREE_STRIPE 101

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

struct rpdfs_msg_block_counts_result {
	__le64 allocated;
	__le64 inodes;
	__le64 total;
};

#endif
