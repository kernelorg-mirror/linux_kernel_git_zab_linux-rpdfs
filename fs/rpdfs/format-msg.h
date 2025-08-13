/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_FORMAT_MSG_H
#define RPDFS_SHARED_FORMAT_MSG_H

enum {
	RPDFS_MSG_BLOCK_READ = 0,
	RPDFS_MSG_BLOCK_READ_RESULT,
	RPDFS_MSG_BLOCK_WRITE,
	RPDFS_MSG_BLOCK_WRITE_RESULT,
	RPDFS_MSG_BLOCK_REQUEST_MODE,
	RPDFS_MSG_BLOCK_GRANT_MODE,
	RPDFS_MSG_BLOCK_REVOKE_MODE,
	RPDFS_MSG_BLOCK_CONFIRM_MODE,
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

struct rpdfs_msg_block_read_result {
	__le64 bnr;
	__le64 wcount;
	__u8 grant_mode;
	__u8 err;
	__u8 _pad[6];
};

struct rpdfs_msg_block_write {
	__le64 bnr;
	__le64 wcount;
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

#endif
