/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/inet.h>

#include "parse.h"
#include "pr.h"

int rpdfs_parse_ipv4(const char *str, struct sockaddr_in *sockaddr)
{
	char *addr = NULL;
	int ret = -EINVAL;
	char *sep;
	u16 port;

	rpdfs_prd("'%s'", str);
	if (!str || !*str)
		goto out;

	sep = strchr(str, ':');
	if (!sep || sep == str || (*(sep + 1) == '\0'))
		goto out;

	addr = kstrndup(str, sep - str, GFP_USER);
	if (!addr) {
		ret = -ENOMEM;
		goto out;
	}

	ret = kstrtou16(sep + 1, 0, &port);
	if (ret == 0) {
		sockaddr->sin_family = AF_INET;
		sockaddr->sin_port = cpu_to_be16(port);
		/* XXX better address checking */
		sockaddr->sin_addr.s_addr = in_aton(addr);
	}
out:
	kfree(addr);
	return ret;
}
