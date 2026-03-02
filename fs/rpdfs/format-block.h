/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_FORMAT_BLOCK_H
#define RPDFS_FORMAT_BLOCK_H

#include <linux/types.h>
#include <linux/align.h>

#define RPDFS_BLOCK_SHIFT	12
#define RPDFS_BLOCK_SIZE	(1 << RPDFS_BLOCK_SHIFT)
#define RPDFS_BLOCK_MASK	(RPDFS_BLOCK_SIZE - 1ULL)

struct rpdfs_block_ref {
	__le64 bnr;
	__le64 alloc_counter; /* XXX */
};

/*
 * The height is one greater than the level of the referenced block.
 * It's 0 for an empty tree.
 */
struct rpdfs_btree_root {
	struct rpdfs_block_ref ref;
	__u8 _pad[7];
	__u8 height;
};

/*
 * Keys are relatively large to allow precise deletion of index keys.
 * They contain the rule's output as well as its inode and rule number.
 * Keys are effectively little-endian 128bit values, but they're only
 * aligned to 64bit in the block.
 */
struct rpdfs_btree_key {
	__le64 lsq;
	__le64 msq;
};

/*
 * The first and last keys record the range of item keys that can be
 * found in the block.  It's dependent on (and redundant with) the keys
 * of separating parent items.  Having it in the block makes tracking
 * the range a bit easier to use and to update as we merge and split.
 */
struct rpdfs_btree_block {
	struct rpdfs_btree_key first;
	struct rpdfs_btree_key last;
	__le64 bnr;
	__le16 nr_items;
	__le16 tail_free;
	__le16 total_free;
	__u8 level;
	__u8 _pad[1];
	struct rpdfs_btree_item_header {
		__le16 off;
		__le16 val_size;
	} ihdrs[];
};

/*
 * The item's value payload is 64bit aligned and immediately follows the
 * item struct in the block.
 */
struct rpdfs_btree_item {
	struct rpdfs_btree_key key;
	__u8 val[];
};

/*
 * We want to avoid there only being a few items in a full block so we
 * chose a reasonably small fraction of the block size.  The array of
 * item headers is sized to fit the max number of items with no value
 * payload, while aligning the first item after the array.
 */
#define RPDFS_BTREE_MAX_VAL_SIZE	511
#define RPDFS_BTREE_ITEM_ALIGN		8
#define RPDFS_BTREE_MAX_ITEMS									\
	ALIGN_DOWN(((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_btree_block)) /			\
		   (sizeof(struct rpdfs_btree_key) + sizeof(struct rpdfs_btree_item_header))),	\
		   RPDFS_BTREE_ITEM_ALIGN)
#define RPDFS_BTREE_MAX_FREE									\
	(RPDFS_BLOCK_SIZE - offsetof(struct rpdfs_btree_block, ihdrs[RPDFS_BTREE_MAX_ITEMS]))

/*
 * The inode generation number changes every time a specific inode is
 * freed and reallocated. The inode number and generation number
 * uniquely identify a file/dir over its lifetime. This lets users of
 * references to inodes find out if the inode has changed out from
 * underneath them. Generally, the inode number and generation number
 * should be referenced together, so make a struct to keep them
 * together.
 */

struct rpdfs_ino_gen {
	__le64 ino;	/* inode number, starts at 1 */
	__le64 gen;	/* inode generation, starts at 1 */
};

/*
 * Data blocks are pointed to by a simple tree of indirect blocks rooted
 * in a single field in the inode. The height is one greater than the
 * level of the referenced block. It's 0 for an empty tree.
 */
struct rpdfs_data_root {
	struct rpdfs_block_ref ref;
	__u8 height;
	__u8 _pad[7];
};

/*
 * Indirect blocks are a simple array of block refs. We rely on the
 * number of references per indirect block being a power of 2, so check
 * that at compile time.
 */
#define RPDFS_DATA_REFS_PER_BLK (RPDFS_BLOCK_SIZE / sizeof(struct rpdfs_block_ref))

/*
 * Because blocks per ref is based on the size of a struct, we can't do
 * the smart thing and define the shift first and then the value, we
 * have to go backwards and define the shift from the value instead.
 */

#define RPDFS_DATA_REFS_PER_BLK_SHIFT const_ilog2(RPDFS_DATA_REFS_PER_BLK)

struct rpdfs_indirect_block {
	struct rpdfs_block_ref refs[RPDFS_DATA_REFS_PER_BLK];
};

/*
 * Inodes are stored in inode blocks.  Inode blocks numbers are directly
 * calculated from the inode number.  The block itself is formatted as a
 * btree block and the inodes (and other inline inode data) are stored
 * as btree items in the block.
 */
struct rpdfs_inode {
	struct rpdfs_ino_gen ig;
	__le64 size;
	__le64 version;			/* changed on file content/metadata changes */
	struct rpdfs_ino_gen parent_ig;	/* only valid for directories */
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le32 flags;
	__le32 pad;
	__le32 xattr_names_len;	/* total length of null-terminated xattr names */
	__le64 xattr_creates;	/* update on each xattr create and use in key */
	__le64 atime_nsec;
	__le64 ctime_nsec;
	__le64 mtime_nsec;
	__le64 crtime_nsec;
	struct rpdfs_btree_root dirents;
	struct rpdfs_btree_root xattrs;
	struct rpdfs_data_root data;
};

#define RPDFS_ROOT_INO 1
#define RPDFS_ROOT_GEN 1

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
	struct rpdfs_ino_gen ig; /* inode number and generation */
	__u8 pers_dtype; /* rpdfs persistent directory entry type */
	__u8 name_len; /* no null termination */
	union {
		__u8 pad[6]; /* pad to alignment, stored can be smaller */
		DECLARE_FLEX_ARRAY(__u8, name);
	};
};

/* max dirent name length, without null term */
#define RPDFS_NAME_MAX	255

/* dirents must have at least 1 name byte */
#define RPDFS_DIRENT_MIN_VAL_SIZE offsetof(struct rpdfs_dirent, name[1])

/* just a random value */
#define RPDFS_DIRENT_HASH_SEED	0xce94cad8f038f79a

/*
 * The low bit of the dirent key value (and readdir pos) is manually
 * assigned to handle colliding name hash values.  We don't want the
 * unlikely event of a single hash collision to prevent creation.
 */
#define RPDFS_DIRENT_COLL_BIT	1ULL

/*
 * We clear the high bit to avoid signed long telldir/seekdir and
 * initially clear the collision bits.
 */
#define RPDFS_DIRENT_HASH_MASK	(U64_MAX ^ (1ULL << 63) ^ RPDFS_DIRENT_COLL_BIT)

/* reserved hash values for . and .. */
#define RPDFS_DIRENT_DOT_HASH	 	0ULL
#define RPDFS_DIRENT_DOT_DOT_HASH	1ULL
#define RPDFS_DIRENT_MIN_HASH		2ULL

/*
 * xattrs are currently implemented as btree items, whose keys are the
 * hash of the name combined with the xattr_create_counter value in the
 * inode, which makes the keys unique - no collisions.
 */
struct rpdfs_xattr {
	__le16 val_len;
	__u8 name_len;
	union {
		__u8 pad[1]; /* pad to alignment, stored will be bigger */
		DECLARE_FLEX_ARRAY(__u8, name);
	};
};

/*
 * Maximum size of the xattr struct plus non-null-terminated xattr
 * name/value (see xattr_size()).
 *
 * TODO: support much larger xattrs by storing in blocks instead of
 * btree items.
 */
#define RPDFS_XATTR_MAX_SIZE	RPDFS_BTREE_MAX_VAL_SIZE

/*
 * Maximum length of all null terminated xattr names per inode. This is
 * the VFS-imposed limit for listxattr.
 */
#define RPDFS_XATTR_MAX_NAMES_LEN	65536

#define RPDFS_XATTR_HASH_SEED		0xfadefadefadefade

#endif
