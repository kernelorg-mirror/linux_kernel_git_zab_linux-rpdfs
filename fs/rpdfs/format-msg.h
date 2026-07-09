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

struct rpdfs_msg_block_details {
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

struct rpdfs_msg_block_counts_result {
	__le64 allocated;
	__le64 inodes;
	__le64 total;
};

#endif
