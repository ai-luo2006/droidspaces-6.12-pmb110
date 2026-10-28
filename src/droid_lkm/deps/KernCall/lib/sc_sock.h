// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef KERNCALL_SC_SOCK_H
#define KERNCALL_SC_SOCK_H

#include <linux/sockptr.h>
#include <linux/types.h>

#define SC_SOCK_PROTO 0x53
#define SC_SOCK_LEVEL 0x5343
#define SC_SOCK_OPT_HELLO 0x1000
#define SC_SOCK_OPT_FAMILY 0x1001
#define SC_SOCK_EVENT_MAX 4096

struct socket;
struct msghdr;
struct vm_area_struct;

struct sc_sock_consumer_ops {
	int (*ioctl)(struct socket *sock, unsigned int cmd, unsigned long arg);
	int (*sendmsg)(struct socket *sock, struct msghdr *msg, size_t len);
	int (*mmap)(struct file *file, struct socket *sock,
		    struct vm_area_struct *vma);
	int (*setsockopt)(struct socket *sock, int level, int optname,
			  sockptr_t optval, unsigned int optlen);
	int (*getsockopt)(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen);
};

int sc_sock_init(const struct sc_sock_consumer_ops *ops);
void sc_sock_exit(void);
int sc_sock_family(void);
int sc_sock_send_event(const void *data, size_t len);
int sc_sock_send_event_to(const void *data, size_t len, struct socket *sock);
void *sc_sock_priv(struct socket *sock);
int sc_sock_set_priv(struct socket *sock, void *priv);

/* the file of the socket, the handle sc_fd_perm_grant takes */
struct file *sc_sock_file(struct socket *sock);

#endif
