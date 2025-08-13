/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_PARSE_H
#define RPDFS_PARSE_H

#include <linux/inet.h>

int rpdfs_parse_ipv4(const char *str, struct sockaddr_in *sockaddr);

#endif
