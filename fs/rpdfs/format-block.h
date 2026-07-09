/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_FORMAT_BLOCK_H
#define RPDFS_FORMAT_BLOCK_H

#include <linux/types.h>
#include <linux/align.h>
#include <linux/bitfield.h>

#define RPDFS_BLOCK_SHIFT	12
#define RPDFS_BLOCK_SIZE	(1 << RPDFS_BLOCK_SHIFT)
#define RPDFS_BLOCK_MASK	(RPDFS_BLOCK_SIZE - 1ULL)

/*
 * Hand tuned to be a power of two that doesn't limit the number of
 * reasonably sized dirents/xattrs in a block.
 *
 * It might be worth it to squeeze the size of the entry down so we
 * could go up another power of two.
 */
#define RPDFS_EHTABLE_ENTRIES_SHIFT	6
#define RPDFS_EHTABLE_ENTRIES		(1 << RPDFS_EHTABLE_ENTRIES_SHIFT)
#define RPDFS_EHTABLE_ENTRIES_MASK	(RPDFS_EHTABLE_ENTRIES - 1)

#define RPDFS_EHTABLE_FULL_ENTRIES	(RPDFS_EHTABLE_ENTRIES * 80 / 100)

/* resolve collisions by assigning low bits, enospc once consumed */
#define RPDFS_EHTABLE_POS_BITS		2
#define RPDFS_EHTABLE_POS_MASK		((1UL << RPDFS_EHTABLE_POS_BITS) - 1)

/*
 * Each level removes a high bit of the hash and then we want enough
 * bits remaining for the number of entries in a block.
 */
#define RPDFS_EHTABLE_MAX_DEPTH	(32 - RPDFS_EHTABLE_ENTRIES_SHIFT)

struct rpdfs_ehtable_desc {
	__le32 nr_keys;
	__le32 total_key_size;
};

struct rpdfs_ehtable_block {
	__le16 tail_free;
	__le16 total_free;
	__le16 nr_entries;
	__u8 depth;
	__u8 _pad;
	struct rpdfs_ehtable_entry {
		__le32 hash;
		__le16 offset;
		__u8 probe_len;
		__u8 pos;
	} entries[RPDFS_EHTABLE_ENTRIES];
};

/*
 * Items are an unaligned payload that starts with the le9 key_size and
 * u7 val_size which are followed by the bytes of the key and value.
 */
struct rpdfs_ehtable_item {
	__le16 key_val_sizes;
} __packed;

#define RPDFS_EHTABLE_KEY_SIZE_FIELD	GENMASK_U16(8, 0)
#define RPDFS_EHTABLE_VAL_SIZE_FIELD	GENMASK_U16(15, 9)
#define RPDFS_EHTABLE_MAX_KEY_SIZE	FIELD_MAX(RPDFS_EHTABLE_KEY_SIZE_FIELD)
#define RPDFS_EHTABLE_MAX_VAL_SIZE	FIELD_MAX(RPDFS_EHTABLE_VAL_SIZE_FIELD)

struct rpdfs_inode_nr {
	__le64 i[2];
};

#define RPDFS_INIT_ROOT_INODE_NR { { cpu_to_le64(1), cpu_to_le64(1) } }

struct rpdfs_inode {
	struct rpdfs_inode_nr ino;
	__le64 size;
	__le64 version;			/* changed on file content/metadata changes */
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le32 flags;
	__le64 atime_nsec;
	__le64 ctime_nsec;
	__le64 mtime_nsec;
	__le64 crtime_nsec;
	struct rpdfs_ehtable_desc dirent_eht;
	struct rpdfs_ehtable_desc xattr_eht;
};

/*
 * This is totally arbitrary.  It looks like it's 32bit in the stat ABI.
 * Most local file systems have around U16_MAX, but some have U32_MAX.
 */
#define RPDFS_LINK_MAX	S32_MAX

enum rpdfs_dentry_type {
	RPDFS_DT_FIFO = 0,
	RPDFS_DT_CHR,
	RPDFS_DT_DIR,
	RPDFS_DT_BLK,
	RPDFS_DT_REG,
	RPDFS_DT_LNK,
	RPDFS_DT_SOCK,
};

struct rpdfs_dirent {
	struct rpdfs_inode_nr ino;
	__u8 pad[6];
	__u8 pers_dtype; /* rpdfs persistent directory entry type */
	__u8 name_len; /* no null termination */
};

/* size of the dirent struct stored in the item before the name */
#define RPDFS_DIRENT_SIZEOF offsetof(struct rpdfs_dirent, name)

/* max dirent name length, without null term */
#define RPDFS_NAME_MAX	255

/* just a random value */
#define RPDFS_DIRENT_HASH_SEED	0xce94cad8

/* reserved hash values for . and .. */
#define RPDFS_DIRENT_DOT_HASH	 	0ULL
#define RPDFS_DIRENT_DOT_DOT_HASH	1ULL
/* the btree clears collision bits so we must use a min past them */
#define RPDFS_DIRENT_MIN_HASH		(RPDFS_EHTABLE_POS_MASK + 1)

/*
 * An empty dir contains pseudo entries for "." and "..". The reported
 * size of directory is the length of the null-terminated names of all
 * the directory entries. (The actual size is the number of blocks
 * necessary to store the dirents btree.)
 */
#define RPDFS_EMPTY_DIR_LEN	5

struct rpdfs_xattr {
	__le16 val_len;
	__u8 name_len;
	__u8 _pad;
};

/* max xattr name length, without null term */
#define	RPDFS_XATTR_MAX_NAME_LEN	RPDFS_NAME_MAX

/*
 * Maximum length of all null terminated xattr names per inode. This is
 * the VFS-imposed limit for listxattr.
 */
#define RPDFS_XATTR_MAX_NAMES_LEN	65536

#define RPDFS_XATTR_HASH_SEED		0x416a8d97

#endif
