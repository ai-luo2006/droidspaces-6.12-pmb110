// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <net/sock.h>

#include "sc_sock.h"

struct sc_sock {
	struct sock sk;
	struct list_head list;
	void *priv;
};

static const struct sc_sock_consumer_ops *g_ops;

static int sc_sock_create(struct net *net, struct socket *sock, int protocol,
			  int kern);

static struct proto sc_sock_proto = {
	.name = "KERNSC",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct sc_sock),
};

static struct net_proto_family sc_sock_family_ops = {
	.family = AF_DECnet,
	.create = sc_sock_create,
	.owner = THIS_MODULE,
};

static struct proto_ops sc_sock_proto_ops;

static int sc_sock_family_nr = -1;
static LIST_HEAD(sc_sock_list);
static DEFINE_SPINLOCK(sc_sock_lock);

static int sc_sock_create(struct net *net, struct socket *sock, int protocol,
			  int kern)
{
	struct sc_sock *ss;
	struct sock *sk;

	if (!capable(CAP_NET_BIND_SERVICE))
		return -EACCES;
	if (sock->type != SOCK_RAW)
		return -EOPNOTSUPP;
	if (protocol != SC_SOCK_PROTO)
		return -EPROTONOSUPPORT;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	sock->state = SS_UNCONNECTED;
	sk = sk_alloc(net, sc_sock_family_nr, GFP_KERNEL, &sc_sock_proto, kern);
	if (!sk) {
		module_put(THIS_MODULE);
		return -ENOBUFS;
	}

	sock->ops = &sc_sock_proto_ops;
	sock_init_data(sock, sk);

	ss = (struct sc_sock *)sk;
	ss->priv = NULL;
	INIT_LIST_HEAD(&ss->list);

	spin_lock(&sc_sock_lock);
	list_add_tail(&ss->list, &sc_sock_list);
	spin_unlock(&sc_sock_lock);

	return 0;
}

static int sc_sock_release(struct socket *sock)
{
	struct sc_sock *ss;
	struct sock *sk;

	sk = sock->sk;
	if (!sk)
		return 0;

	ss = (struct sc_sock *)sk;
	spin_lock(&sc_sock_lock);
	list_del(&ss->list);
	spin_unlock(&sc_sock_lock);

	module_put(THIS_MODULE);
	sock_orphan(sk);
	sock_put(sk);
	return 0;
}

int sc_sock_send_event_to(const void *data, size_t len, struct socket *sock)
{
	struct sc_sock *ss;
	struct sk_buff *skb;
	unsigned long flags;

	if (sc_sock_family_nr < 0)
		return -ENODEV;
	if (!data || !len || len > SC_SOCK_EVENT_MAX)
		return -EINVAL;
	if (!sock || !sock->sk)
		return -EINVAL;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, data, len);

	ss = (struct sc_sock *)sock->sk;
	spin_lock_irqsave(&sc_sock_lock, flags);
	skb_queue_tail(&ss->sk.sk_receive_queue, skb);
	ss->sk.sk_data_ready(&ss->sk);
	spin_unlock_irqrestore(&sc_sock_lock, flags);

	return 0;
}

int sc_sock_send_event(const void *data, size_t len)
{
	struct sc_sock *ss;
	struct sk_buff *clone;
	struct sk_buff *skb;
	unsigned long flags;
	int ret = 0;

	if (sc_sock_family_nr < 0)
		return -ENODEV;
	if (!data || !len || len > SC_SOCK_EVENT_MAX)
		return -EINVAL;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, data, len);

	spin_lock_irqsave(&sc_sock_lock, flags);
	list_for_each_entry(ss, &sc_sock_list, list) {
		clone = skb_clone(skb, GFP_ATOMIC);
		if (!clone) {
			ret = -ENOMEM;
			continue;
		}
		skb_queue_tail(&ss->sk.sk_receive_queue, clone);
		ss->sk.sk_data_ready(&ss->sk);
	}
	spin_unlock_irqrestore(&sc_sock_lock, flags);

	kfree_skb(skb);
	return ret;
}

static int sc_sock_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			   int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int ret;

	skb = skb_dequeue(&sk->sk_receive_queue);
	if (!skb) {
		if (flags & MSG_DONTWAIT)
			return -EAGAIN;
		if (wait_event_interruptible(*sk_sleep(sk),
					     !skb_queue_empty(&sk->sk_receive_queue)))
			return -EINTR;
		skb = skb_dequeue(&sk->sk_receive_queue);
		if (!skb)
			return -EAGAIN;
	}

	ret = skb_copy_datagram_msg(skb, 0, msg, skb->len);
	if (ret == 0)
		ret = skb->len;
	kfree_skb(skb);
	return ret;
}

static int sc_sock_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	if (g_ops && g_ops->sendmsg)
		return g_ops->sendmsg(sock, msg, len);
	return -EOPNOTSUPP;
}

static int sc_sock_getsockopt(struct socket *sock, int level, int optname,
			      char __user *optval, int __user *optlen)
{
	int val;
	int len;

	if (level != SC_SOCK_LEVEL)
		return -ENOPROTOOPT;

	switch (optname) {
	case SC_SOCK_OPT_HELLO:
		val = 0x53434831;
		break;
	case SC_SOCK_OPT_FAMILY:
		val = sc_sock_family_nr;
		break;
	default:
		if (g_ops && g_ops->getsockopt)
			return g_ops->getsockopt(sock, level, optname, optval,
						optlen);
		return -ENOPROTOOPT;
	}

	if (get_user(len, optlen))
		return -EFAULT;
	if (len < sizeof(val))
		return -EINVAL;
	if (copy_to_user(optval, &val, sizeof(val)))
		return -EFAULT;
	return put_user(sizeof(val), optlen);
}

static int sc_sock_setsockopt(struct socket *sock, int level, int optname,
			      sockptr_t optval, unsigned int optlen)
{
	if (g_ops && g_ops->setsockopt)
		return g_ops->setsockopt(sock, level, optname, optval, optlen);
	return -ENOPROTOOPT;
}

static int sc_sock_ioctl(struct socket *sock, unsigned int cmd,
			 unsigned long arg)
{
	if (g_ops && g_ops->ioctl)
		return g_ops->ioctl(sock, cmd, arg);
	return -ENOTTY;
}

static __poll_t sc_sock_poll(struct file *file, struct socket *sock,
			     poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, sk_sleep(sock->sk), wait);
	if (!skb_queue_empty(&sock->sk->sk_receive_queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int sc_sock_bind(struct socket *sock, struct sockaddr *addr, int len)
{
	return -EOPNOTSUPP;
}

static int sc_sock_connect(struct socket *sock, struct sockaddr *addr, int len,
			   int flags)
{
	return -EOPNOTSUPP;
}

static int sc_sock_socketpair(struct socket *sock1, struct socket *sock2)
{
	return -EOPNOTSUPP;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static int sc_sock_accept(struct socket *sock, struct socket *newsock,
			  struct proto_accept_arg *arg)
{
	return -EOPNOTSUPP;
}
#else
static int sc_sock_accept(struct socket *sock, struct socket *newsock,
			  int flags, bool kern)
{
	return -EOPNOTSUPP;
}
#endif

static int sc_sock_getname(struct socket *sock, struct sockaddr *addr,
			   int peer)
{
	return -EOPNOTSUPP;
}

static int sc_sock_listen(struct socket *sock, int backlog)
{
	return -EOPNOTSUPP;
}

static int sc_sock_shutdown(struct socket *sock, int how)
{
	return -EOPNOTSUPP;
}

static int sc_sock_mmap(struct file *file, struct socket *sock,
			struct vm_area_struct *vma)
{
	if (g_ops && g_ops->mmap)
		return g_ops->mmap(file, sock, vma);
	return -EOPNOTSUPP;
}

static struct proto_ops sc_sock_proto_ops = {
	.family = AF_DECnet,
	.owner = THIS_MODULE,
	.release = sc_sock_release,
	.bind = sc_sock_bind,
	.connect = sc_sock_connect,
	.socketpair = sc_sock_socketpair,
	.accept = sc_sock_accept,
	.getname = sc_sock_getname,
	.poll = sc_sock_poll,
	.ioctl = sc_sock_ioctl,
	.listen = sc_sock_listen,
	.shutdown = sc_sock_shutdown,
	.setsockopt = sc_sock_setsockopt,
	.getsockopt = sc_sock_getsockopt,
	.sendmsg = sc_sock_sendmsg,
	.recvmsg = sc_sock_recvmsg,
	.mmap = sc_sock_mmap,
};

int sc_sock_init(const struct sc_sock_consumer_ops *ops)
{
	int fam;
	int ret;

	g_ops = ops;
	ret = proto_register(&sc_sock_proto, 1);
	if (ret) {
		g_ops = NULL;
		return ret;
	}

	for (fam = AF_DECnet; fam < NPROTO; fam++) {
		sc_sock_family_ops.family = fam;
		if (sock_register(&sc_sock_family_ops) == 0) {
			sc_sock_family_nr = fam;
			sc_sock_proto_ops.family = fam;
			pr_info("[kerncall] sc_sock family=%d\n", fam);
			return 0;
		}
	}

	proto_unregister(&sc_sock_proto);
	g_ops = NULL;
	return -EADDRINUSE;
}

void sc_sock_exit(void)
{
	if (sc_sock_family_nr < 0)
		return;

	sock_unregister(sc_sock_family_nr);
	proto_unregister(&sc_sock_proto);
	sc_sock_family_nr = -1;
	g_ops = NULL;
}

int sc_sock_family(void)
{
	return sc_sock_family_nr;
}

void *sc_sock_priv(struct socket *sock)
{
	struct sc_sock *ss;

	if (!sock || !sock->sk)
		return NULL;
	ss = (struct sc_sock *)sock->sk;
	return ss->priv;
}

int sc_sock_set_priv(struct socket *sock, void *priv)
{
	struct sc_sock *ss;

	if (!sock || !sock->sk)
		return -EINVAL;
	ss = (struct sc_sock *)sock->sk;
	ss->priv = priv;
	return 0;
}

struct file *sc_sock_file(struct socket *sock)
{
	if (!sock)
		return NULL;
	return sock->file;
}
