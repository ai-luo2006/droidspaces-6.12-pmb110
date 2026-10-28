// SPDX-License-Identifier: GPL-2.0-only
/*
 * Credential side of installing a fake user namespace. Mirrors
 * set_cred_user_ns() from kernel/user_namespace.c: the full capability set goes
 * to the new namespace, where cap_capable() honours it, while every check
 * against the initial namespace fails because of the level comparison.
 */

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/key.h>

#include "misc.h"
#include "misc_ksym.h"
#include "userns.h"

/*
 * the raw id a task takes inside a real namespace. container id 0 becomes uid_base,
 * and a task that arrives with an id the map does not name lands on container id 0
 * rather than on an owner nothing else in the container shares
 */
static u32 droid_lkm_userns_shift(const struct droid_lkm_userns *u, u32 raw,
				  u32 base)
{
	u32 outer = droid_lkm_userns_map_down(u, raw);

	if (outer == (u32)-1)
		return base;

	return outer;
}

void droid_lkm_userns_install_cred(struct cred *new, struct droid_lkm_userns *u)
{
	new->securebits = SECUREBITS_DEFAULT;
	new->cap_inheritable = CAP_EMPTY_SET;
	new->cap_permitted = CAP_FULL_SET;
	new->cap_effective = CAP_FULL_SET;
	new->cap_ambient = CAP_EMPTY_SET;
	new->cap_bset = CAP_FULL_SET;
#ifdef CONFIG_KEYS
	if (new->request_key_auth) {
		droid_lkm_misc_ks.key_put(new->request_key_auth);
		new->request_key_auth = NULL;
	}
#endif
	new->user_ns = &u->uns;

	/*
	 * set_cred_user_ns() leaves the four ids at 0, because on a kernel with user
	 * namespaces 0 is the id the container's root is mapped from. here the map is
	 * carried by the credential itself: the task really is uid_base on the host, and
	 * userns_view.c turns that back into 0 for everything the container reads
	 */
	if (u->real) {
		u32 uid = droid_lkm_userns_shift(u, __kuid_val(new->uid),
						 u->uid_base);
		u32 gid = droid_lkm_userns_shift(u, __kgid_val(new->gid),
						 u->gid_base);

		new->uid = KUIDT_INIT(uid);
		new->euid = KUIDT_INIT(uid);
		new->suid = KUIDT_INIT(uid);
		new->fsuid = KUIDT_INIT(uid);
		new->gid = KGIDT_INIT(gid);
		new->egid = KGIDT_INIT(gid);
		new->sgid = KGIDT_INIT(gid);
		new->fsgid = KGIDT_INIT(gid);
	}
}

int droid_lkm_userns_install_current(struct droid_lkm_userns *u)
{
	struct cred *new;

	new = droid_lkm_misc_ks.prepare_creds();
	if (!new)
		return -ENOMEM;

	droid_lkm_userns_install_cred(new, u);
	droid_lkm_misc_ks.commit_creds(new);

	return 0;
}
