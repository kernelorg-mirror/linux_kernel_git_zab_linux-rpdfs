# SPDX-License-Identifier: GPL-2.0

#
# When building as an external module let define_trace.h find
# TRACE_INCLUDE_PATH="."
#
CFLAGS_rpdfs_trace.o = -I$(src)

obj-$(CONFIG_RPDFS_FS) += rpdfs.o

rpdfs-y	:= aops.o \
	   balloc.o \
           block.o \
	   btree.o \
	   btree_txn.o \
	   dir.o \
	   ehtable.o \
	   file.o \
	   ht.o \
	   inode.o \
	   map.o \
	   meta.o \
	   mkfs.o \
	   net.o \
	   net_tcp.o \
	   parse.o \
	   preq.o \
	   rht.o \
	   rlock.o \
	   rpdfs_trace.o \
	   super.o \
	   txn.o \
	   xattr.o
