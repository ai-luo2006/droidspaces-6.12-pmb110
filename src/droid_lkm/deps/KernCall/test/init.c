// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/reboot.h>

#ifndef AF_DECnet
#define AF_DECnet 12
#endif

#define SC_KEY "kerncall"
#define SC_CMD_HELLO 0x1000
#define SC_MAGIC     0x53434831UL
#define SC_TEST_PATCH        0x2001
#define SC_TEST_UNPATCH      0x2002
#define SC_TEST_PATCH_DUP    0x2003
#define SC_TEST_PATCH_BAD    0x2004
#define SC_TEST_ENTRY        0x2005
#define SC_TEST_TP_GETPID    0x2006
#define SC_TEST_TP_UNGETPID  0x2007
#define SC_TEST_TP_INTERCEPT 0x2008
#define SC_TEST_TP_ENTER     0x2009
#define SC_TEST_TP_GETTID    0x200a
#define SC_TEST_TP_UNGETTID  0x200b
#define SC_TEST_TP_SLOT      0x200c
#define SC_TEST_TP_HOOKED    0x200d
#define SC_TEST_TP_REG_BAD   0x200e
#define SC_TEST_TP_REG_BIG   0x200f
#define SC_TEST_TP_REG_NULL  0x2010
#define SC_TEST_TP_ORIG_BAD  0x2011
#define SC_TEST_TP_ORIG_BIG  0x2012
#define SC_TEST_TP_ORIG_NI   0x2013
#define SC_TEST_TP_ORIG_RAW  0x2014
#define SC_TEST_TP_SELF      0x2015
#define SC_TEST_TP_CHAN      0x2016
#define SC_TEST_TP_UNSELF    0x2017
#define SC_TEST_TP_UNCHAN    0x2018
#define SC_TEST_REINIT       0x2019
#define SC_TEST_SOCK_EVENT   0x2020

#define SC_SOCK_PROTO 0x53
#define SC_SOCK_LEVEL 0x5343
#define SC_SOCK_OPT_HELLO 0x1000
#define SC_SOCK_OPT_FAMILY 0x1001
#define SC_SOCK_EVENT_MSG "kerncall-sock-event"

#define TP_MARK       0x600D
#define TP_MARK_TID   0x600E

static const int empty_slots[] = {
	18, 42,
	249, 250, 251, 252, 253, 254, 255, 256, 257,
	295, 296, 297, 298, 299, 300,
	415,
	-1,
};

static int g_fails;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void check(int cond, const char *what)
{
	logmsg("[test] %-38s %s\n", what, cond ? "PASS" : "FAIL");
	if (!cond)
		g_fails++;
}

static void try_mount(const char *dev, const char *dir, const char *type)
{
	if (mount(dev, dir, type, 0, NULL))
		logmsg("mount %s: %m\n", type);
}

static int load_module(const char *path, const char *params)
{
	int fd = open(path, O_RDONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = syscall(SYS_finit_module, fd, params, 0);
	close(fd);
	return ret;
}

static long probe_slot(const char *key)
{
	int i;

	for (i = 0; empty_slots[i] >= 0; i++) {
		if (syscall(empty_slots[i], key, SC_CMD_HELLO, 0, 0, 0, 0) ==
		    (long)SC_MAGIC)
			return empty_slots[i];
	}
	return -1;
}

static long test_round(const char *mode, const char *params)
{
	long slot;
	long r;

	logmsg("[test] === round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}

	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;
	logmsg("[test] slot = %ld\n", slot);

	r = syscall(slot, SC_KEY, SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == (long)SC_MAGIC, "hello handshake");

	r = syscall(slot, "wrong_key", SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == -1 && errno == EACCES, "wrong key rejected");

	r = syscall(slot, SC_KEY, SC_TEST_PATCH, 0, 0, 0, 0);
	check(r == 0, "patch slot 249");

	r = syscall(249, 0, 0, 0, 0, 0, 0);
	check(r == 0xDEADBEEF, "patched handler active");

	r = syscall(slot, SC_KEY, SC_TEST_PATCH_DUP, 0, 0, 0, 0);
	check(r == -1 && errno == EEXIST, "duplicate patch rejected");

	r = syscall(slot, SC_KEY, SC_TEST_PATCH_BAD, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "patch nr 512 rejected");

	r = syscall(slot, SC_KEY, SC_TEST_ENTRY, slot, 0, 0, 0);
	check(r != 0 && r != (long)SC_MAGIC, "sc_entry reads table");

	r = syscall(slot, SC_KEY, SC_TEST_UNPATCH, 0, 0, 0, 0);
	check(r == 0, "unpatch slot 249");

	r = syscall(249, 0, 0, 0, 0, 0, 0);
	check(r == -1 && errno == ENOSYS, "slot 249 restored to ni");

unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

static long test_tp_round(const char *mode, const char *params, int nochan)
{
	long slot;
	long r;
	long pid;

	logmsg("[test] === tp round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}

	if (nochan) {
		slot = probe_slot(SC_KEY);
		check(slot < 0, "no channel slot exposed");
	} else {
		slot = probe_slot(SC_KEY);
		check(slot >= 0, "slot discovery");
		if (slot < 0)
			goto unload;
	}

	pid = getpid();
	check(pid > 0, "getpid normal before hook");

	if (nochan)
		goto unload;

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETPID, 0, 0, 0, 0);
	check(r == 0, "tp register getpid hook");

	r = getpid();
	check(r == pid, "getpid passthrough via orig");

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETPID, 0, 0, 0, 0);
	check(r == -1 && errno == EEXIST, "tp double register rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_INTERCEPT, 1, 0, 0, 0);
	check(r == 0, "tp intercept on");

	r = getpid();
	check(r == TP_MARK, "getpid intercepted");

	r = syscall(slot, SC_KEY, SC_TEST_TP_INTERCEPT, 0, 0, 0, 0);
	check(r == 0, "tp intercept off");

	r = getpid();
	check(r == pid, "getpid passthrough again");

	r = syscall(slot, SC_KEY, SC_TEST_TP_ENTER, 0, 0, 0, 0);
	check(r >= 3, "tp observer counted getpid");

	r = syscall(slot, SC_KEY, SC_TEST_TP_UNGETPID, 0, 0, 0, 0);
	check(r == 0, "tp unregister getpid hook");

	r = getpid();
	check(r == pid, "getpid direct after unregister");

unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

static long test_tp_api_round(const char *mode, const char *params)
{
	long slot;
	long r;
	long pid;

	logmsg("[test] === tp api round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}
	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;

	r = syscall(slot, SC_KEY, SC_TEST_TP_HOOKED, 0, 0, 0, 0);
	check(r == 0, "hooked false initially");

	r = syscall(slot, SC_KEY, SC_TEST_TP_REG_BAD, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "register nr -1 rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_REG_BIG, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "register nr 512 rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_REG_NULL, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "register NULL fn rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_ORIG_BAD, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "orig nr -1 rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_ORIG_BIG, 0, 0, 0, 0);
	check(r == -1 && errno == EINVAL, "orig nr 512 rejected");

	r = syscall(slot, SC_KEY, SC_TEST_TP_ORIG_NI, 0, 0, 0, 0);
	check(r == -1 && errno == ENOSYS, "orig on ni slot");

	pid = getpid();
	r = syscall(slot, SC_KEY, SC_TEST_TP_ORIG_RAW, 0, 0, 0, 0);
	check(r == pid, "orig without hook");

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETPID, 0, 0, 0, 0);
	check(r == 0, "register getpid");

	r = syscall(slot, SC_KEY, SC_TEST_TP_HOOKED, 0, 0, 0, 0);
	check(r == 1, "hooked true after register");

	r = syscall(slot, SC_KEY, SC_TEST_TP_UNGETPID, 0, 0, 0, 0);
	check(r == 0, "unregister getpid");

	r = getpid();
	check(r == pid, "getpid normal after unregister");

	r = syscall(slot, SC_KEY, SC_TEST_REINIT, 0, 0, 0, 0);
	check(r == -1 && errno == EALREADY, "reinit rejected");

	r = syscall(slot, SC_KEY, SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == (long)SC_MAGIC, "channel alive after reinit attempt");

unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

static long test_tp_multi_round(const char *mode, const char *params)
{
	long slot;
	long r;
	long pid;
	long tid;

	logmsg("[test] === tp multi round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}
	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;

	pid = getpid();
	tid = syscall(SYS_gettid);

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETPID, 0, 0, 0, 0);
	check(r == 0, "register getpid");

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETTID, 0, 0, 0, 0);
	check(r == 0, "register gettid");

	r = syscall(slot, SC_KEY, SC_TEST_TP_INTERCEPT, 1, 0, 0, 0);
	check(r == 0, "intercept on");

	r = getpid();
	check(r == TP_MARK, "getpid intercepted");

	r = syscall(SYS_gettid);
	check(r == TP_MARK_TID, "gettid intercepted");

	r = syscall(slot, SC_KEY, SC_TEST_TP_UNGETTID, 0, 0, 0, 0);
	check(r == 0, "unregister gettid only");

	r = syscall(SYS_gettid);
	check(r == tid, "gettid direct after unregister");

	r = getpid();
	check(r == TP_MARK, "getpid still intercepted");

	r = syscall(slot, SC_KEY, SC_TEST_TP_INTERCEPT, 0, 0, 0, 0);
	check(r == 0, "intercept off");

	r = getpid();
	check(r == pid, "getpid passthrough");

	r = syscall(SYS_gettid);
	check(r == tid, "gettid passthrough");

unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

static long test_tp_slot_round(const char *mode, const char *params)
{
	long slot;
	long tp_slot;
	long r;

	logmsg("[test] === tp slot round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}
	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;

	r = syscall(slot, SC_KEY, SC_TEST_TP_SLOT, 0, 0, 0, 0);
	check(r > 0 && r != slot, "tp slot distinct from channel");
	tp_slot = r;
	logmsg("[test] tp slot = %ld\n", tp_slot);

	r = syscall(slot, SC_KEY, SC_TEST_TP_SELF, 0, 0, 0, 0);
	check(r == 0, "register on tp slot allowed");

	r = syscall(tp_slot, SC_KEY, SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == -1 && errno == ENOSYS, "tp slot call stays ni");

	r = syscall(slot, SC_KEY, SC_TEST_TP_UNSELF, 0, 0, 0, 0);
	check(r == 0, "unregister tp slot hook");

	r = syscall(slot, SC_KEY, SC_TEST_TP_CHAN, 0, 0, 0, 0);
	check(r == 0, "register on channel slot allowed");

	r = syscall(slot, SC_KEY, SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == (long)SC_MAGIC, "channel hello via hook passthrough");

	r = syscall(slot, SC_KEY, SC_TEST_TP_UNCHAN, 0, 0, 0, 0);
	check(r == 0, "unregister channel slot hook");

	r = syscall(slot, SC_KEY, SC_CMD_HELLO, 0, 0, 0, 0);
	check(r == (long)SC_MAGIC, "channel hello direct");

unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

static long test_tp_dirty_round(const char *mode, const char *params)
{
	long slot;
	long r;
	long pid;

	logmsg("[test] === tp dirty round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}
	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;

	pid = getpid();

	r = syscall(slot, SC_KEY, SC_TEST_TP_GETPID, 0, 0, 0, 0);
	check(r == 0, "register getpid");

	r = syscall(slot, SC_KEY, SC_TEST_TP_INTERCEPT, 1, 0, 0, 0);
	check(r == 0, "intercept on");

	r = getpid();
	check(r == TP_MARK, "getpid intercepted");

	if (syscall(SYS_delete_module, "kerncall", 0)) {
		logmsg("delete_module: %d (%m)\n", errno);
		check(0, "dirty unload");
		goto unload;
	}

	r = getpid();
	check(r == pid, "getpid normal after dirty unload");

unload:
	return slot;
}

static long test_sock_round(const char *mode, const char *params)
{
	char buf[64];
	long slot;
	long r;
	socklen_t len;
	ssize_t n;
	int fd;
	int val;

	logmsg("[test] === sock round: %s ===\n", mode);
	if (load_module("/kerncall.ko", params)) {
		logmsg("finit_module: %d (%m)\n", errno);
		return -1;
	}
	slot = probe_slot(SC_KEY);
	check(slot >= 0, "slot discovery");
	if (slot < 0)
		goto unload;

	fd = socket(AF_DECnet, SOCK_RAW, SC_SOCK_PROTO);
	check(fd >= 0, "socket create");
	if (fd < 0)
		goto unload;

	val = 0;
	len = sizeof(val);
	r = getsockopt(fd, SC_SOCK_LEVEL, SC_SOCK_OPT_HELLO, &val, &len);
	check(r == 0 && val == 0x53434831, "socket hello");

	val = 0;
	len = sizeof(val);
	r = getsockopt(fd, SC_SOCK_LEVEL, SC_SOCK_OPT_FAMILY, &val, &len);
	check(r == 0 && val >= 0, "socket family");

	r = syscall(slot, SC_KEY, SC_TEST_SOCK_EVENT, 0, 0, 0, 0);
	check(r == 0, "kernel send event");

	memset(buf, 0, sizeof(buf));
	n = recv(fd, buf, sizeof(buf), 0);
	check(n == (ssize_t)(sizeof(SC_SOCK_EVENT_MSG) - 1) &&
	      memcmp(buf, SC_SOCK_EVENT_MSG, n) == 0, "recv event");

	close(fd);
unload:
	if (syscall(SYS_delete_module, "kerncall", 0))
		logmsg("delete_module: %d (%m)\n", errno);
	return slot;
}

int main(void)
{
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	try_mount("proc", "/proc", "proc");
	try_mount("sysfs", "/sys", "sysfs");

	test_round("consumer find_slot", "key=" SC_KEY);
	test_round("default find_slot", "key=" SC_KEY " find_slot_mode=1");
	test_sock_round("socket", "key=" SC_KEY);
	test_tp_round("consumer find_slot", "key=" SC_KEY " tp_enable=1", 0);
	test_tp_round("default find_slot",
		      "key=" SC_KEY " find_slot_mode=1 tp_enable=1", 0);
	test_tp_round("tp only", "key=" SC_KEY " tp_enable=1 no_patch=1 "
			       "tp_hook_init=1", 1);
	test_tp_api_round("api errors", "key=" SC_KEY " tp_enable=1");
	test_tp_multi_round("multi hook", "key=" SC_KEY " tp_enable=1");
	test_tp_slot_round("slot conflicts", "key=" SC_KEY " tp_enable=1");
	test_tp_dirty_round("dirty unload", "key=" SC_KEY " tp_enable=1");
	test_tp_round("mark current", "key=" SC_KEY " tp_enable=1 "
		      "tp_mark_all=0", 0);

	logmsg("[test] %s (%d fails)\n", g_fails ? "FAILED" : "ALL PASS",
	       g_fails);
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART, NULL);
	for (;;)
		sleep(3600);
	return 0;
}
