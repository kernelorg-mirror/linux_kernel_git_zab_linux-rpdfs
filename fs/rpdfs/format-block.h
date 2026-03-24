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
 * The first and last keys record the range of item keys that can be
 * found in the block.  It's dependent on (and redundant with) the keys
 * of separating parent items.  Having it in the block makes tracking
 * the range a bit easier to use and to update as we merge and split.
 */
struct rpdfs_btree_block {
	__le16 nr_items;
	__le16 avail_free;
	__le16 total_free;
	__u8 _pad;
	__u8 level;
	__le64 items[];
};

/*
 * Low bits of the key are used to resolve hash collisions so that an
 * unlucky birthday paradox winner doesn't see errors.
 *
 * We ensure that all the collisions for a key are kept in the same
 * leaf.  This means that we can have to move all the collisions
 * together as we split or merge.  Today the max item size is a
 * worryingly large portion of the block.  We have to keep the number of
 * collision bits very low to avoid the space taken up by collisions
 * exceeding half a block and throwing a bunch of balancing assumptions
 * out the window.
 */
#define RPDFS_BTREE_KEY_COLL_BITS	1
#define RPDFS_BTREE_KEY_COLL_MASK	((1ULL << RPDFS_BTREE_KEY_COLL_BITS) - 1)

#define RPDFS_BTREE_VAL_ALIGN_SHIFT	3
#define RPDFS_BTREE_VAL_ALIGN		(1 << RPDFS_BTREE_VAL_ALIGN_SHIFT)

/*
 * The smaller the max item size, the closer we can come to balancing
 * blocks by used size rather than item count.  This is made a bit worse
 * by keeping all key collisions in the same leaf block.  The max we can
 * be out of balance isn't one item but the number of collisions.  So we
 * push the max size down as far as is reasonable.  The specific max is
 * roughly the size of an xattr with a full compliment of block
 * references.
 */
#define RPDFS_BTREE_MAX_VAL_SIZE	(8 + 255 + (8 * 16))

/*
 * We pack the fields of each item into a word.  Each shift here is to
 * go from the native form into the packed bit position.  The masks are
 * in terms of the native value.
 *
 * The value off is a bit special because it is aligned and the low bits
 * are always clear.  The shift is to go between the full precision byte
 * offset and the packed offset as a factor of the alignment.  It has
 * masks for both the native and packed form.
 *
 * The key is packed into the most significant bits so that we can shift
 * a search key to match and perform efficient binary searches of words.
 */
#define RPDFS_BTREE_ITEM_OFF_BITS	(RPDFS_BLOCK_SHIFT - RPDFS_BTREE_VAL_ALIGN_SHIFT)
#define RPDFS_BTREE_ITEM_SIZE_BITS	order_base_2(RPDFS_BTREE_MAX_VAL_SIZE + 1)
#define RPDFS_BTREE_ITEM_KEY_BITS	(64 - (RPDFS_BTREE_ITEM_OFF_BITS + \
					       RPDFS_BTREE_ITEM_SIZE_BITS))

#define RPDFS_BTREE_ITEM_OFF_SHIFT	RPDFS_BTREE_VAL_ALIGN_SHIFT
#define RPDFS_BTREE_ITEM_OFF_MASK	((1ULL << (RPDFS_BTREE_ITEM_OFF_BITS + \
						   RPDFS_BTREE_VAL_ALIGN_SHIFT)) - 1)
#define RPDFS_BTREE_ITEM_OFF_PACK_MASK	((1ULL << RPDFS_BTREE_ITEM_OFF_BITS) - 1)
#define RPDFS_BTREE_ITEM_SIZE_SHIFT	RPDFS_BTREE_ITEM_OFF_BITS
#define RPDFS_BTREE_ITEM_SIZE_MASK	((1ULL << RPDFS_BTREE_ITEM_SIZE_BITS) - 1)
#define RPDFS_BTREE_ITEM_KEY_SHIFT	(RPDFS_BTREE_ITEM_SIZE_SHIFT + \
					 RPDFS_BTREE_ITEM_SIZE_BITS)
#define RPDFS_BTREE_ITEM_KEY_MASK	((1ULL << RPDFS_BTREE_ITEM_KEY_BITS) - 1)

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

/* size of the dirent struct stored in the item before the name */
#define RPDFS_DIRENT_SIZEOF offsetof(struct rpdfs_dirent, name)

/* max dirent name length, without null term */
#define RPDFS_NAME_MAX	255

/* just a random value */
#define RPDFS_DIRENT_HASH_SEED	0xce94cad8f038f79a

/* reserved hash values for . and .. */
#define RPDFS_DIRENT_DOT_HASH	 	0ULL
#define RPDFS_DIRENT_DOT_DOT_HASH	1ULL
/* the btree clears collision bits so we must use a min past them */
#define RPDFS_DIRENT_MIN_HASH		(RPDFS_BTREE_KEY_COLL_MASK + 1)

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
	union {
		__u8 pad[1]; /* pad to alignment, stored will be bigger */
		DECLARE_FLEX_ARRAY(__u8, name);
	};
};

/* size of the xattr struct stored in the item before the name */
#define RPDFS_XATTR_SIZEOF offsetof(struct rpdfs_xattr, name)

/* max xattr name length, without null term */
#define	RPDFS_XATTR_MAX_NAME_LEN	RPDFS_NAME_MAX

/*
 * Maximum length of all null terminated xattr names per inode. This is
 * the VFS-imposed limit for listxattr.
 */
#define RPDFS_XATTR_MAX_NAMES_LEN	65536

#define RPDFS_XATTR_HASH_SEED		0xfadefadefadefade

#endif
