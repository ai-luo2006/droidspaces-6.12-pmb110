// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/atomic.h>
#include <linux/timekeeping.h>
#include <linux/ptrace.h>
#ifdef CONFIG_KERNSC_TP
#include <linux/compat.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/thread_info.h>
#include <linux/tracepoint.h>
#endif
#if defined(CONFIG_KERNSC_TP) || defined(CONFIG_KERNSC_REDIR)
#include <asm/unistd.h>
#endif

#include "hk.h"
#include "hk_kretprobe.h"
#include "hk_patch.h"
#include "hk_redir.h"
#include "sc.h"
#include "sc_slide.h"

#define SC_PATCH_MAX 16
#define SC_MSEC_NS 1000000UL
#define SC_TIER_CHECK_MASK 0x3ff
#define SC_TP_PROBE_MAX 64

static const struct sc_cfg *g_cfg;
static const struct sc_layout *g_layout;

static unsigned long *sys_call_table;
static unsigned long g_ni_addr;
static unsigned long g_ni_cfi;
static int g_slot = -1;

/*
 * the key is copied here at init, so the compare on the call path reads only
 * library owned memory and takes no lock, and so a call copies the key length
 * instead of the whole buffer
 */
static char g_key[SC_KEY_MAX];
static unsigned int g_key_len;

static struct {
	unsigned long nr;
	unsigned long orig;
	bool used;
} g_patches[SC_PATCH_MAX];

struct sc_replay_sess {
	u64 sid;
	u64 nonce;
	u64 last_seq;
	u64 expire_ns;
	bool used;
};

static struct sc_replay_sess g_sessions[SC_REPLAY_SESS_MAX];
static DEFINE_SPINLOCK(g_replay_lock);
static atomic64_t g_nonce_seq = ATOMIC64_INIT(0);

struct sc_fd_perm {
	struct file *file;
	unsigned int perms;
	u64 expire_ns;
};

static struct sc_fd_perm g_fd_perms[SC_FD_PERM_MAX];
static DEFINE_SPINLOCK(g_fd_perm_lock);

static unsigned long resolve(const char *name)
{
	if (g_layout && g_layout->resolve)
		return g_layout->resolve(name);
	return 0;
}

int sc_safe_read(void *dst, const void *src, size_t sz)
{
	return copy_from_kernel_nofault(dst, src, sz);
}

/*
 * the write goes through the HooKern patch entry, which parks the other cores with
 * stop_machine and maps the page through the kernel's own fixmap slot, so a failed
 * write is reported instead of being written anyway. the default path reads the
 * entry back, because a table write that lands nowhere is the failure this channel
 * cannot see otherwise: the value the caller gets is the value the table holds
 */
static int (*g_patch_write)(void *addr, unsigned long val, void *priv);
static void *g_patch_priv;

static int sc_patch_write(void *addr, unsigned long val)
{
	if (g_patch_write)
		return g_patch_write(addr, val, g_patch_priv);
	if (g_cfg->write)
		return hk_write_run(addr, &val, sizeof(val), g_cfg->write);
	return hk_write_poke(addr, &val, sizeof(val));
}

static int sc_hk_init(void)
{
	struct hk_cfg cfg;
	int ret;

	cfg.resolve = g_layout->resolve;
	ret = hk_init(&cfg);
	if (ret && ret != -EALREADY)
		return ret;
	return 0;
}

static bool sc_is_root(void)
{
	return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static bool sc_perm_allowed(unsigned int perm, const struct pt_regs *regs,
			    const struct sc_cmd *ent)
{
	switch (perm) {
	case SC_PERM_ROOT:
		return sc_is_root();
	case SC_PERM_MANAGER:
		return g_cfg->is_manager && g_cfg->is_manager();
	case SC_PERM_ROOT_MANAGER:
		return sc_is_root() ||
		       (g_cfg->is_manager && g_cfg->is_manager());
	case SC_PERM_SU:
		if (g_cfg->is_manager && g_cfg->is_manager())
			return true;
		return g_cfg->is_allow_uid &&
		       g_cfg->is_allow_uid(current_uid().val);
	case SC_PERM_DOMAIN:
		return g_cfg->is_domain && g_cfg->is_domain(current);
	case SC_PERM_ALWAYS:
		return true;
	case SC_PERM_CUSTOM:
		return ent && ent->perm_check &&
		       ent->perm_check(regs, g_cfg->priv);
	default:
		return false;
	}
}

int sc_fd_perm_grant(struct file *file, unsigned int perms,
		     unsigned long ttl_ms)
{
	unsigned long flags;
	u64 expire_ns = 0;
	int free_slot = -1;
	int i;

	if (!file || !perms)
		return -EINVAL;
	if (ttl_ms)
		expire_ns = ktime_get_ns() + (u64)ttl_ms * SC_MSEC_NS;

	spin_lock_irqsave(&g_fd_perm_lock, flags);
	for (i = 0; i < SC_FD_PERM_MAX; i++) {
		if (g_fd_perms[i].file == file) {
			g_fd_perms[i].perms = perms;
			g_fd_perms[i].expire_ns = expire_ns;
			spin_unlock_irqrestore(&g_fd_perm_lock, flags);
			return 0;
		}
		if (free_slot < 0 && !g_fd_perms[i].file)
			free_slot = i;
	}
	if (free_slot < 0) {
		spin_unlock_irqrestore(&g_fd_perm_lock, flags);
		return -ENOSPC;
	}
	get_file(file);
	g_fd_perms[free_slot].file = file;
	g_fd_perms[free_slot].perms = perms;
	g_fd_perms[free_slot].expire_ns = expire_ns;
	spin_unlock_irqrestore(&g_fd_perm_lock, flags);
	return 0;
}

unsigned int sc_fd_perm_get(struct file *file)
{
	unsigned long flags;
	unsigned int perms = 0;
	u64 now;
	int i;

	if (!file)
		return 0;

	now = ktime_get_ns();
	spin_lock_irqsave(&g_fd_perm_lock, flags);
	for (i = 0; i < SC_FD_PERM_MAX; i++) {
		if (g_fd_perms[i].file != file)
			continue;
		if (g_fd_perms[i].expire_ns &&
		    now >= g_fd_perms[i].expire_ns)
			g_fd_perms[i].perms = 0;
		perms = g_fd_perms[i].perms;
		break;
	}
	spin_unlock_irqrestore(&g_fd_perm_lock, flags);
	return perms;
}

/* fput releases a reference and can sleep, so it stays out of the lock */
static void sc_fd_perm_take(int idx, struct file *file)
{
	struct file *drop;

	spin_lock_irq(&g_fd_perm_lock);
	drop = NULL;
	if (!file || g_fd_perms[idx].file == file) {
		drop = g_fd_perms[idx].file;
		g_fd_perms[idx].file = NULL;
		g_fd_perms[idx].perms = 0;
		g_fd_perms[idx].expire_ns = 0;
	}
	spin_unlock_irq(&g_fd_perm_lock);

	if (drop)
		fput(drop);
}

void sc_fd_perm_clear(struct file *file)
{
	int i;

	if (!file)
		return;
	for (i = 0; i < SC_FD_PERM_MAX; i++)
		sc_fd_perm_take(i, file);
}

static unsigned int sc_fd_perms_at(const struct pt_regs *regs,
				   unsigned char arg)
{
	struct file *file;
	unsigned int perms;

	if (!arg || arg > SC_ARG_MAX)
		return 0;
	/* fget is used instead of fdget, struct fd is opaque since 6.12 */
	file = fget((unsigned int)regs->regs[arg]);
	if (!file)
		return 0;
	perms = sc_fd_perm_get(file);
	fput(file);
	return perms;
}

static u64 sc_replay_sid(const struct pt_regs *regs)
{
	if (g_cfg->replay_sid)
		return g_cfg->replay_sid(regs, g_cfg->priv);
	return (u64)task_tgid_nr(current);
}

int sc_replay_open(u64 sid, u64 nonce)
{
	unsigned long flags;
	u64 expire_ns = 0;
	u64 now;
	int slot = -1;
	int i;

	if (!nonce)
		return -EINVAL;
	if (!g_cfg)
		return -ENODEV;

	now = ktime_get_ns();
	if (g_cfg->replay_ttl_ms)
		expire_ns = now + (u64)g_cfg->replay_ttl_ms * SC_MSEC_NS;

	spin_lock_irqsave(&g_replay_lock, flags);
	for (i = 0; i < SC_REPLAY_SESS_MAX; i++) {
		if (g_sessions[i].used && g_sessions[i].sid == sid) {
			slot = i;
			break;
		}
		if (slot < 0 && !g_sessions[i].used)
			slot = i;
	}
	if (slot < 0) {
		for (i = 0; i < SC_REPLAY_SESS_MAX; i++) {
			if (g_sessions[i].expire_ns &&
			    now >= g_sessions[i].expire_ns) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0) {
		spin_unlock_irqrestore(&g_replay_lock, flags);
		return -ENOSPC;
	}
	g_sessions[slot].sid = sid;
	g_sessions[slot].nonce = nonce;
	g_sessions[slot].last_seq = 0;
	g_sessions[slot].expire_ns = expire_ns;
	g_sessions[slot].used = true;
	spin_unlock_irqrestore(&g_replay_lock, flags);
	return 0;
}

void sc_replay_close(u64 sid)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&g_replay_lock, flags);
	for (i = 0; i < SC_REPLAY_SESS_MAX; i++) {
		if (g_sessions[i].used && g_sessions[i].sid == sid) {
			g_sessions[i].used = false;
			break;
		}
	}
	spin_unlock_irqrestore(&g_replay_lock, flags);
}

/* the sequence has to move forward, a repeat or a reorder is a replay */
static int sc_replay_verify(const struct pt_regs *regs)
{
	unsigned long flags;
	u64 sid = sc_replay_sid(regs);
	u64 nonce = regs->regs[SC_ARG_NONCE];
	u64 seq = regs->regs[SC_ARG_SEQ];
	u64 now;
	int ret = -EACCES;
	int i;

	if (!nonce || !seq)
		return -EACCES;

	now = ktime_get_ns();
	spin_lock_irqsave(&g_replay_lock, flags);
	for (i = 0; i < SC_REPLAY_SESS_MAX; i++) {
		if (!g_sessions[i].used || g_sessions[i].sid != sid)
			continue;
		if (g_sessions[i].expire_ns && now >= g_sessions[i].expire_ns) {
			g_sessions[i].used = false;
			break;
		}
		if (g_sessions[i].nonce == nonce && seq > g_sessions[i].last_seq) {
			g_sessions[i].last_seq = seq;
			ret = 0;
		}
		break;
	}
	spin_unlock_irqrestore(&g_replay_lock, flags);
	return ret;
}

static long sc_cmd_hello(const struct pt_regs *regs, void *priv)
{
	return SC_MAGIC;
}

/*
 * the nonce only has to be unpredictable to a caller that does not hold the
 * key, and such a caller cannot reach the command anyway. the monotonic clock,
 * a per boot counter and the caller pid are mixed instead of pulling in a
 * random primitive as another resolver dependency
 */
static u64 sc_nonce_next(void)
{
	u64 seq = (u64)atomic64_inc_return(&g_nonce_seq);

	return (ktime_get_ns() * 0x9E3779B97F4A7C15ULL) ^ (seq << 17) ^
	       ((u64)task_tgid_nr(current) << 33);
}

static long sc_cmd_session(const struct pt_regs *regs, void *priv)
{
	u64 __user *out = (u64 __user *)regs->regs[SC_ARG_NONCE];
	u64 sid = sc_replay_sid(regs);
	u64 nonce = sc_nonce_next();
	int ret;

	if (!g_cfg->replay_enable)
		return -ENOSYS;
	ret = sc_replay_open(sid, nonce);
	if (ret)
		return ret;
	if (copy_to_user(out, &nonce, sizeof(nonce))) {
		sc_replay_close(sid);
		return -EFAULT;
	}
	return 0;
}

static long sc_cmd_batch(const struct pt_regs *regs, void *priv)
{
	return sc_batch_run(regs);
}

static const struct sc_cmd sc_builtin_cmds[] = {
	{
		.cmd = SC_CMD_HELLO,
		.name = "HELLO",
		.handler = sc_cmd_hello,
		.perm = SC_PERM_ROOT,
		.no_replay = true,
		.read_only = true,
	},
	{
		.cmd = SC_CMD_SESSION,
		.name = "SESSION",
		.handler = sc_cmd_session,
		.perm = SC_PERM_ROOT,
		.no_replay = true,
	},
	{
		.cmd = SC_CMD_BATCH,
		.name = "BATCH",
		.handler = sc_cmd_batch,
		.perm = SC_PERM_ROOT_MANAGER,
	},
};

static const struct sc_cmd *sc_cmd_lookup(unsigned long cmd)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sc_builtin_cmds); i++) {
		if (sc_builtin_cmds[i].cmd == cmd)
			return &sc_builtin_cmds[i];
	}
	for (i = 0; g_cfg->cmds && i < g_cfg->ncmds; i++) {
		if (g_cfg->cmds[i].cmd == cmd)
			return &g_cfg->cmds[i];
	}
	return NULL;
}

/*
 * a read only entry that also carries the no replay mark is idempotent: a
 * repeat of it changes nothing, so it needs neither a session nor the fd
 * capability, and the whole path is one compare and one indirect call with
 * no lock and no allocation
 */
static bool sc_cmd_fast(const struct sc_cmd *ent)
{
	return ent->read_only && ent->no_replay;
}

static long sc_cmd_call(const struct sc_cmd *ent, const struct pt_regs *regs)
{
	if (!ent->handler)
		return -ENOSYS;
	return ent->handler(regs, g_cfg->priv);
}

/*
 * nested is a command of a batch: the request it belongs to already passed
 * the replay guard, so the entries are not checked one by one
 */
static long sc_cmd_dispatch(const struct pt_regs *regs, unsigned long cmd,
			    bool nested)
{
	const struct sc_cmd *ent;
	unsigned int fd_perms;
	int ret;

	ent = sc_cmd_lookup(cmd);
	if (!ent) {
		if (!sc_is_root())
			return -EPERM;
		if (g_cfg->dispatch)
			return g_cfg->dispatch((long)cmd, regs, g_cfg->priv);
		return -ENOSYS;
	}

	/* a batch inside a batch multiplies the fan out and the stack use */
	if (nested && ent->cmd == SC_CMD_BATCH)
		return -EINVAL;

	if (sc_cmd_fast(ent)) {
		if (!sc_perm_allowed(ent->perm, regs, ent))
			return -EPERM;
		return sc_cmd_call(ent, regs);
	}

	if (!sc_perm_allowed(ent->perm, regs, ent)) {
		if (!ent->fd_arg)
			return -EPERM;
		fd_perms = sc_fd_perms_at(regs, ent->fd_arg);
		if (!(fd_perms & SC_FD_PERM(ent->perm)))
			return -EPERM;
	}

	if (g_cfg->replay_enable && !ent->no_replay && !nested) {
		ret = sc_replay_verify(regs);
		if (ret)
			return ret;
	}

	return sc_cmd_call(ent, regs);
}

/*
 * the batch runs every entry and reports one return code per entry, a failing
 * entry does not stop the ones behind it: a batch is a polling primitive and
 * the per entry codes are the point of it. the whole descriptor is copied in
 * and out inside this call, no user pointer is kept
 */
long sc_batch_run(const struct pt_regs *regs)
{
	struct sc_batch batch;
	struct pt_regs scratch;
	unsigned long i;
	unsigned long k;
	long done = 0;

	if (!regs->regs[SC_ARG_BATCH])
		return -EINVAL;
	if (copy_from_user(&batch, (const void __user *)regs->regs[SC_ARG_BATCH],
			   sizeof(batch)))
		return -EFAULT;
	if (!batch.count || batch.count > SC_BATCH_MAX)
		return -EINVAL;

	/*
	 * only the key and the arguments are meaningful for an entry, the rest
	 * of the register file is zeroed rather than copied
	 */
	memset(&scratch, 0, sizeof(scratch));
	scratch.regs[SC_ARG_KEY] = regs->regs[SC_ARG_KEY];

	for (i = 0; i < batch.count; i++) {
		if (fatal_signal_pending(current))
			break;
		scratch.regs[SC_ARG_CMD] = (unsigned long)batch.entry[i].cmd;
		for (k = 0; k < SC_BATCH_ARGS; k++)
			scratch.regs[SC_ARG_ARG0 + k] = batch.entry[i].arg[k];
		batch.ret[i] = sc_cmd_dispatch(&scratch, batch.entry[i].cmd,
					       true);
		done++;
	}
	for (; i < batch.count; i++)
		batch.ret[i] = -EINTR;

	if (copy_to_user((void __user *)regs->regs[SC_ARG_BATCH], &batch,
			 sizeof(batch)))
		return -EFAULT;
	return done;
}

static long sc_handler(const struct pt_regs *regs)
{
	const char __user *key_ptr;
	char kbuf[SC_KEY_MAX] = {0};

	if (!g_cfg)
		return -ENOSYS;

	key_ptr = (const char __user *)regs->regs[SC_ARG_KEY];
	if (strncpy_from_user(kbuf, key_ptr, g_key_len + 1) < 0)
		return -EFAULT;
	if (strncmp(kbuf, g_key, g_key_len + 1))
		return -EACCES;

	return sc_cmd_dispatch(regs, (unsigned long)regs->regs[SC_ARG_CMD],
			       false);
}

static int find_slot_scan(void)
{
	unsigned long v;
	int i;

	if (!sys_call_table)
		return -1;
	for (i = 0; i < 512; i++) {
		if (sc_safe_read(&v, &sys_call_table[i], sizeof(v)))
			continue;
		if (v == g_ni_addr || (g_ni_cfi && v == g_ni_cfi))
			return i;
	}
	return -1;
}

static int find_slot_inner(void)
{
	if (g_cfg->find_slot)
		return g_cfg->find_slot();
	if (g_layout && g_layout->find_slot)
		return g_layout->find_slot();
	return find_slot_scan();
}

#ifdef CONFIG_KERNSC_DISCOVER
unsigned long sc_table_addr(void)
{
	return (unsigned long)sys_call_table;
}

unsigned long sc_entry(unsigned long nr)
{
	unsigned long val;

	if (!sys_call_table)
		return 0;
	if (sc_safe_read(&val, &sys_call_table[nr], sizeof(val)))
		return 0;
	return val;
}

int sc_find_slot_scan(void)
{
	return find_slot_scan();
}
#endif

static int patch_slot(unsigned long nr, unsigned long handler,
		      unsigned long *orig_out)
{
	unsigned long orig;
	int i;

	if (nr >= 512)
		return -EINVAL;
	if (!sys_call_table)
		return -ENODATA;
	if (sc_safe_read(&orig, &sys_call_table[nr], sizeof(orig)))
		return -EFAULT;

	for (i = 0; i < SC_PATCH_MAX; i++) {
		if (g_patches[i].used && g_patches[i].nr == nr)
			return -EEXIST;
	}
	for (i = 0; i < SC_PATCH_MAX; i++) {
		if (!g_patches[i].used) {
			int ret;

			g_patches[i].nr = nr;
			g_patches[i].orig = orig;
			g_patches[i].used = true;
			ret = sc_patch_write(&sys_call_table[nr], handler);
			if (ret) {
				g_patches[i].used = false;
				return ret;
			}
			if (orig_out)
				*orig_out = orig;
			return 0;
		}
	}
	return -ENOSPC;
}

static void unpatch_slot(unsigned long nr)
{
	int i;

	for (i = 0; i < SC_PATCH_MAX; i++) {
		if (g_patches[i].used && g_patches[i].nr == nr) {
			sc_patch_write(&sys_call_table[nr], g_patches[i].orig);
			g_patches[i].used = false;
			return;
		}
	}
}

#ifdef CONFIG_KERNSC_PATCH
int sc_patch(unsigned long nr, unsigned long handler, unsigned long *orig_out)
{
	return patch_slot(nr, handler, orig_out);
}

void sc_unpatch(unsigned long nr)
{
	unpatch_slot(nr);
}
#endif

#if defined(CONFIG_KERNSC_TP) || defined(CONFIG_KERNSC_REDIR)
/* avoid the const sys_call_table declared in asm/syscall.h */
typedef long (*syscall_fn_t)(const struct pt_regs *regs);

/*
 * call what the entry of nr points at. every route that has to reach the original
 * reads the entry here and keeps no copy, so a slot another writer moved is
 * answered with what it holds now
 */
static long __nocfi sc_call_entry(unsigned long nr, const struct pt_regs *regs)
{
	unsigned long orig;
	syscall_fn_t fn;

	if (nr >= __NR_syscalls)
		return -EINVAL;
	if (!sys_call_table)
		return -ENODATA;
	if (sc_safe_read(&orig, &sys_call_table[nr], sizeof(orig)))
		return -EFAULT;
	if (!orig)
		return -ENODATA;
	fn = (syscall_fn_t)orig;
	return fn(regs);
}
#endif

#ifdef CONFIG_KERNSC_REDIR
#define SC_REDIR_MAX 16

/*
 * one redirect. the table entry keeps its value, so what carries a call is the
 * breakpoint on the function the entry points at, and nr is only the name the
 * caller gave that function
 */
struct sc_redir_slot {
	unsigned long nr;
	unsigned long handler;
	sc_redir_fn fn;
	struct hk_redir redir;
	bool used;
};

static struct sc_redir_slot g_redirs[SC_REDIR_MAX];

static struct sc_redir_slot *redir_find(unsigned long nr)
{
	int i;

	for (i = 0; i < SC_REDIR_MAX; i++) {
		if (g_redirs[i].used && g_redirs[i].nr == nr)
			return &g_redirs[i];
	}
	return NULL;
}

int sc_redir(unsigned long nr, sc_redir_fn fn, unsigned long *orig_out)
{
	struct sc_redir_slot *slot = NULL;
	unsigned long handler;
	int i;
	int ret;

	if (!sys_call_table)
		return -ENODATA;
	if (!fn || nr >= __NR_syscalls)
		return -EINVAL;
	if (sc_safe_read(&handler, &sys_call_table[nr],
			  sizeof(handler)))
		return -EFAULT;
	if (!handler)
		return -ENODATA;

	for (i = 0; i < SC_REDIR_MAX; i++) {
		if (!g_redirs[i].used) {
			if (!slot)
				slot = &g_redirs[i];
			continue;
		}
		if (g_redirs[i].nr == nr)
			return -EEXIST;
		/*
		 * the kernel runs one handler of an address, so a second probe on the
		 * same function is never reached. an entry shared by several syscall
		 * numbers, which is what an empty slot holds, takes one redirect, and
		 * fn tells the numbers apart by reading regs->syscallno
		 */
		if (g_redirs[i].handler == handler)
			return -EEXIST;
	}
	/* a patched slot holds a handler of this module, a breakpoint there is not it */
	for (i = 0; i < SC_PATCH_MAX; i++) {
		if (g_patches[i].used && g_patches[i].nr == nr)
			return -EEXIST;
	}
	if (!slot)
		return -ENOSPC;

	ret = hk_redir_install_at(&slot->redir, handler, (unsigned long)fn,
				  HK_REDIR_KPROBE);
	if (ret)
		return ret;

	slot->fn = fn;
	slot->handler = handler;
	slot->nr = nr;
	slot->used = true;
	if (orig_out)
		*orig_out = handler;
	pr_info("[kerncall] redirect nr=%lu handler=0x%lx\n", nr, handler);
	return 0;
}

void sc_unredir(unsigned long nr)
{
	struct sc_redir_slot *slot = redir_find(nr);
	int ret;

	if (!slot)
		return;

	ret = hk_redir_remove(&slot->redir);
	if (ret) {
		/* the probe is still armed, the exit path counts it and blocks on it */
		pr_warn("[kerncall] redirect nr=%lu not taken back %d\n", nr, ret);
		return;
	}
	slot->used = false;
	slot->fn = NULL;
}

bool sc_redir_hooked(unsigned long nr)
{
	return redir_find(nr) != NULL;
}

void sc_redir_leave(unsigned long nr)
{
	struct sc_redir_slot *slot = redir_find(nr);

	if (slot)
		hk_redir_leave(&slot->redir);
}

long sc_redir_orig(unsigned long nr, const struct pt_regs *regs)
{
	return sc_call_entry(nr, regs);
}
#endif

#ifdef CONFIG_KERNSC_TP
#define TP_ORIG_NR(r) ((r)->regs[8])

typedef void (*srcu_sync_fn)(struct srcu_struct *sp);

static sc_tp_hook_fn sc_tp_hooks[__NR_syscalls];
static int g_tp_slot = -1;
static struct tracepoint *g_tp_sys_enter;
static bool g_tp_mark_all = true;
static void (*g_tp_mark_cb)(struct task_struct *p, bool on);
static void (*g_tp_on_enter)(int id, struct pt_regs *regs);

static unsigned int g_tp_tier_mask = SC_TP_TIER_ALL;
static unsigned int (*g_tp_tier_cb)(const struct task_struct *p, void *priv);
static int g_tp_reg_count;
static bool g_tp_foreign;
static bool g_tp_registered;
static unsigned int g_tp_tier_check;
static DEFINE_SPINLOCK(g_tp_reg_lock);
static struct hk_kretprobe g_tp_reg_probe;
static struct hk_kretprobe g_tp_unreg_probe;
static unsigned long g_tp_stub;

static int find_slot_for_tp(void)
{
	unsigned long v;
	int i;

	if (!sys_call_table)
		return -1;
	for (i = 0; i < 512; i++) {
		if (i == g_slot)
			continue;
		if (sc_safe_read(&v, &sys_call_table[i], sizeof(v)))
			continue;
		if (v == g_ni_addr || (g_ni_cfi && v == g_ni_cfi))
			return i;
	}
	return -1;
}

/*
 * the live probe array is the exact answer to how many users the sys_enter
 * tracepoint has. syscall_regfunc runs on the key transition only, so a second
 * user that joins while the key is already on never reaches the kretprobes
 */
static int tp_probe_count(void)
{
	struct tracepoint_func *funcs;
	int count = 0;
	int i;

	if (!g_tp_sys_enter)
		return 0;

	/* no resolver call here, this also runs from kretprobe context */
	rcu_read_lock();
	funcs = rcu_dereference(g_tp_sys_enter->funcs);
	for (i = 0; funcs && i < SC_TP_PROBE_MAX; i++) {
		if (!funcs[i].func)
			break;
		if ((unsigned long)funcs[i].func == g_tp_stub)
			continue;
		count++;
	}
	rcu_read_unlock();
	return count;
}

static bool tp_foreign_now(void)
{
	if (tp_probe_count() > 1)
		WRITE_ONCE(g_tp_foreign, true);
	return READ_ONCE(g_tp_foreign);
}

static bool tp_tier_match(struct task_struct *p)
{
	if (!g_tp_tier_cb)
		return true;
	return (g_tp_tier_cb(p, g_cfg->priv) & g_tp_tier_mask) != 0;
}

static void tp_mark_proc(struct task_struct *p, bool on, bool all)
{
	if (on && !all)
		on = tp_tier_match(p);
	if (g_tp_mark_cb) {
		g_tp_mark_cb(p, on);
		return;
	}
	if (on)
		set_tsk_thread_flag(p, TIF_SYSCALL_TRACEPOINT);
	else
		clear_tsk_thread_flag(p, TIF_SYSCALL_TRACEPOINT);
}

static void tp_mark_processes(bool on)
{
	struct task_struct *p, *t;
	bool all;

	all = !g_tp_tier_cb || tp_foreign_now();

	if (!g_tp_mark_all) {
		tp_mark_proc(current, on, all);
		return;
	}
	rcu_read_lock();
	for_each_process_thread(p, t)
		tp_mark_proc(t, on, all);
	rcu_read_unlock();
}

/*
 * a foreign user that joins after us is invisible to syscall_regfunc, the
 * sys_enter callback is the remaining place that can notice it. the walk is
 * rare and stops for good once a foreign user was seen
 */
static void tp_check_foreign(void)
{
	if (!g_tp_tier_cb || READ_ONCE(g_tp_foreign))
		return;
	g_tp_tier_check++;
	if ((g_tp_tier_check & SC_TIER_CHECK_MASK) != SC_TIER_CHECK_MASK)
		return;
	if (tp_probe_count() <= 1)
		return;
	WRITE_ONCE(g_tp_foreign, true);
	tp_mark_processes(true);
}

static int tp_regfunc_probe(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned long flags;

	spin_lock_irqsave(&g_tp_reg_lock, flags);
	if (g_tp_reg_count >= 1)
		g_tp_foreign = true;
	g_tp_reg_count++;
	spin_unlock_irqrestore(&g_tp_reg_lock, flags);

	tp_mark_processes(true);
	return 0;
}

static int tp_unregfunc_probe(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned long flags;
	bool left;

	spin_lock_irqsave(&g_tp_reg_lock, flags);
	if (g_tp_reg_count > 0)
		g_tp_reg_count--;
	left = g_tp_reg_count > 0;
	if (!left)
		g_tp_foreign = false;
	spin_unlock_irqrestore(&g_tp_reg_lock, flags);

	tp_mark_processes(left);
	return 0;
}

static void tp_probe_remove(void)
{
	hk_kretprobe_remove(&g_tp_reg_probe);
	hk_kretprobe_remove(&g_tp_unreg_probe);
}

static long sc_tp_dispatcher(const struct pt_regs *regs)
{
	sc_tp_hook_fn fn;
	int orig_nr;

	if (g_tp_slot < 0)
		return -ENOSYS;
	if (regs->syscallno != g_tp_slot)
		return -ENOSYS;

	orig_nr = (int)TP_ORIG_NR(regs);
	if (regs->syscallno == orig_nr)
		return -ENOSYS;

	((struct pt_regs *)regs)->syscallno = orig_nr;
	((struct pt_regs *)regs)->regs[8] = orig_nr;

	if (orig_nr >= 0 && orig_nr < __NR_syscalls) {
		fn = READ_ONCE(sc_tp_hooks[orig_nr]);
		if (fn)
			return fn(orig_nr, regs);
	}
	return -ENOSYS;
}

static void sc_tp_sys_enter(void *data, struct pt_regs *regs, long id)
{
	struct pt_regs *kregs;

	if (unlikely(is_compat_task()))
		return;
	if (g_tp_slot < 0)
		return;
	if (id < 0 || id >= __NR_syscalls)
		return;

	tp_check_foreign();

	if (g_tp_on_enter)
		g_tp_on_enter(id, regs);

	if (id == g_tp_slot)
		return;
	if (!READ_ONCE(sc_tp_hooks[id]))
		return;

	kregs = task_pt_regs(current);
	if (!kregs)
		return;
	kregs->regs[8] = id;
	kregs->syscallno = g_tp_slot;
}

int sc_tp_register(int nr, sc_tp_hook_fn fn)
{
	if (nr < 0 || nr >= __NR_syscalls)
		return -EINVAL;
	if (!fn)
		return -EINVAL;
	if (READ_ONCE(sc_tp_hooks[nr]))
		return -EEXIST;
	WRITE_ONCE(sc_tp_hooks[nr], fn);
	return 0;
}

void sc_tp_unregister(int nr)
{
	if (nr < 0 || nr >= __NR_syscalls)
		return;
	WRITE_ONCE(sc_tp_hooks[nr], NULL);
}

bool sc_tp_hooked(int nr)
{
	if (nr < 0 || nr >= __NR_syscalls)
		return false;
	return READ_ONCE(sc_tp_hooks[nr]) != NULL;
}

long __nocfi sc_tp_orig(int nr, const struct pt_regs *regs)
{
	if (nr < 0)
		return -EINVAL;
	return sc_call_entry((unsigned long)nr, regs);
}

int sc_tp_slot(void)
{
	return g_tp_slot;
}

int sc_tp_reg_count(void)
{
	int count = tp_probe_count();

	if (count > 0)
		return count;
	return READ_ONCE(g_tp_reg_count);
}

static int tp_setup(void)
{
	struct tracepoint *tp;
	int slot;
	int ret;

	g_tp_mark_all = g_cfg->tp_mark_all;
	g_tp_mark_cb = g_cfg->tp_mark_cb;
	g_tp_on_enter = g_cfg->tp_on_enter;
	g_tp_tier_cb = g_cfg->tp_tier_cb;
	if (g_cfg->tp_tier_mask)
		g_tp_tier_mask = g_cfg->tp_tier_mask;

	tp = (struct tracepoint *)resolve("__tracepoint_sys_enter");
	if (!tp) {
		pr_warn("[kerncall] __tracepoint_sys_enter not found\n");
		return -ENODATA;
	}
	g_tp_sys_enter = tp;
	/*
	 * resolved before any probe is live, the probe count is also read
	 * from kretprobe and tracepoint context where a symbol walk may sleep
	 */
	g_tp_stub = resolve("tp_stub_func");

	slot = find_slot_for_tp();
	if (slot < 0) {
		pr_warn("[kerncall] no free slot for tp dispatcher\n");
		ret = -EBUSY;
		goto fail;
	}
	ret = patch_slot(slot, (unsigned long)sc_tp_dispatcher, NULL);
	if (ret) {
		pr_warn("[kerncall] tp dispatcher patch failed\n");
		goto fail;
	}
	g_tp_slot = slot;

	/*
	 * both probes have to be in place before our own registration, the
	 * regfunc of that registration is the one moment that tells whether the
	 * tracepoint had a user before us
	 */
	if (hk_kretprobe_install(&g_tp_reg_probe, "syscall_regfunc",
				 tp_regfunc_probe))
		pr_warn("[kerncall] syscall_regfunc kretprobe unavailable\n");
	if (hk_kretprobe_install(&g_tp_unreg_probe, "syscall_unregfunc",
				 tp_unregfunc_probe))
		pr_warn("[kerncall] syscall_unregfunc kretprobe unavailable\n");

	if (static_key_enabled(&tp->key))
		g_tp_foreign = true;

	ret = tracepoint_probe_register(tp, sc_tp_sys_enter, NULL);
	if (ret) {
		pr_warn("[kerncall] tracepoint register failed %d\n", ret);
		goto fail;
	}
	g_tp_registered = true;

	tp_mark_processes(true);
	pr_info("[kerncall] tp dispatcher slot=%d users=%d\n", g_tp_slot,
		sc_tp_reg_count());
	return 0;

fail:
	tp_probe_remove();
	if (g_tp_slot >= 0) {
		unpatch_slot(g_tp_slot);
		g_tp_slot = -1;
	}
	g_tp_sys_enter = NULL;
	return ret;
}

static __nocfi bool call_srcu_sync(struct srcu_struct *sp)
{
	srcu_sync_fn sync;

	sync = (srcu_sync_fn)resolve("synchronize_srcu");
	if (!sync)
		return false;
	sync(sp);
	return true;
}

static void tp_sync_probes(void)
{
	struct srcu_struct *sp;

	sp = (struct srcu_struct *)resolve("tracepoint_srcu");
	if (sp && call_srcu_sync(sp))
		return;
	/* tracepoint_srcu trimmed by TRIM_UNUSED_KSYMS on some builds */
	synchronize_rcu();
}

static void tp_teardown(void)
{
	bool registered = g_tp_registered;
	bool foreign = false;

	if (registered) {
		foreign = tp_foreign_now();
		tracepoint_probe_unregister(g_tp_sys_enter, sc_tp_sys_enter,
					   NULL);
		tp_sync_probes();
		g_tp_sys_enter = NULL;
		g_tp_registered = false;
	}
	tp_probe_remove();
	if (g_tp_slot >= 0) {
		unpatch_slot(g_tp_slot);
		g_tp_slot = -1;
	}
	/*
	 * without our registration the marks are not ours, and with another
	 * user of the tracepoint still registered they now serve it
	 */
	if (registered && !foreign)
		tp_mark_processes(false);
	memset(sc_tp_hooks, 0, sizeof(sc_tp_hooks));
	g_tp_mark_cb = NULL;
	g_tp_on_enter = NULL;
	g_tp_tier_cb = NULL;
	g_tp_tier_mask = SC_TP_TIER_ALL;
	g_tp_reg_count = 0;
	g_tp_foreign = false;
	g_tp_registered = false;
	g_tp_tier_check = 0;
	g_tp_stub = 0;
}

#endif

int sc_init(const struct sc_cfg *cfg)
{
	unsigned int i;
	int slot;
	int ret;

	if (!cfg || !cfg->key[0])
		return -EINVAL;
	if (g_cfg)
		return -EALREADY;

	g_cfg = cfg;
	g_layout = cfg->layout;
	if (!g_layout || !g_layout->resolve) {
		pr_warn("[kerncall] no layout resolver\n");
		g_cfg = NULL;
		g_layout = NULL;
		return -EINVAL;
	}

	g_patch_write = cfg->patch_write;
	g_patch_priv = cfg->patch_priv;

	g_key_len = strnlen(cfg->key, SC_KEY_MAX);
	memcpy(g_key, cfg->key, g_key_len + 1);

	/*
	 * a read only entry that misses the no replay mark or that wants the
	 * fd capability would silently fall back to the slow path, say so at
	 * init instead
	 */
	for (i = 0; cfg->cmds && i < cfg->ncmds; i++) {
		if (!cfg->cmds[i].read_only)
			continue;
		if (!cfg->cmds[i].no_replay)
			pr_warn("[kerncall] cmd 0x%lx is read only without no_replay, it stays on the slow path\n",
				cfg->cmds[i].cmd);
		if (cfg->cmds[i].fd_arg)
			pr_warn("[kerncall] cmd 0x%lx is read only and takes an fd, the fd capability is not consulted on the fast path\n",
				cfg->cmds[i].cmd);
	}

	ret = sc_hk_init();
	if (ret) {
		pr_warn("[kerncall] hook library init failed %d\n", ret);
		g_cfg = NULL;
		g_layout = NULL;
		return ret;
	}

	g_ni_addr = resolve("__arm64_sys_ni_syscall");
	if (!g_ni_addr) {
		pr_warn("[kerncall] __arm64_sys_ni_syscall not found\n");
		return -ENODATA;
	}
	g_ni_cfi = resolve("__arm64_sys_ni_syscall.cfi_jt");
	sys_call_table = (unsigned long *)resolve("sys_call_table");
	if (!sys_call_table) {
		pr_warn("[kerncall] sys_call_table not found\n");
		return -ENODATA;
	}

	if (!cfg->no_patch) {
		slot = find_slot_inner();
		if (slot < 0) {
			pr_warn("[kerncall] no free syscall slot found\n");
			return -EBUSY;
		}

		if (patch_slot(slot, (unsigned long)sc_handler, NULL)) {
			pr_warn("[kerncall] patch failed\n");
			return -EIO;
		}
		g_slot = slot;
	}

#ifdef CONFIG_KERNSC_TP
	if (cfg->tp_enable) {
		ret = tp_setup();
		if (ret) {
			pr_warn("[kerncall] tp dispatcher setup failed %d\n",
				ret);
			return ret;
		}
	}
#endif

	if (cfg->no_patch) {
		pr_info("[kerncall] layout ready, channel not hooked\n");
		return 0;
	}

	pr_info("[kerncall] syscall channel slot=%d key=%s\n", g_slot,
		cfg->key);
	return 0;
}

void sc_exit(void)
{
	int i;

#ifdef CONFIG_KERNSC_TP
	tp_teardown();
#endif
	if (g_slot >= 0)
		unpatch_slot(g_slot);
	for (i = 0; i < SC_PATCH_MAX; i++) {
		if (g_patches[i].used)
			unpatch_slot(g_patches[i].nr);
	}
#ifdef CONFIG_KERNSC_REDIR
	for (i = 0; i < SC_REDIR_MAX; i++) {
		if (g_redirs[i].used)
			sc_unredir(g_redirs[i].nr);
	}
#endif
	for (i = 0; i < SC_FD_PERM_MAX; i++) {
		if (g_fd_perms[i].file)
			sc_fd_perm_take(i, NULL);
	}
	memset(g_sessions, 0, sizeof(g_sessions));
	memset(g_key, 0, sizeof(g_key));
	g_key_len = 0;
	g_slot = -1;
	g_cfg = NULL;
	g_layout = NULL;
	pr_info("[kerncall] syscall channel closed\n");
}

int sc_get_slot(void)
{
	return g_slot;
}
