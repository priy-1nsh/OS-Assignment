// SPDX-License-Identifier: GPL-2.0
/*
 * fork_guard: graduated fork-storm containment + tiered CPU scheduling.
 *
 * Mechanism 1 (containment) and Mechanism 2 (tiers) use SEPARATE tables and
 * SEPARATE locks. fg_check_fork() never reads the tier table, so tier
 * privilege cannot bypass containment.
 *
 * Containment ladder (per non-root user, sliding window):
 *   stage 0 -> 1 THROTTLE : refuse fork (-EAGAIN), penalty window + sleep
 *   stage 1 -> 2 FREEZE   : SIGSTOP the offending process group
 *   stage 2 -> 3 REAP     : SIGKILL the offending process group
 * REAP also fires fg_reap_delay_ms after a FREEZE (frozen group is reaped
 * if the user has not been forgiven by then), so a fully frozen bomb that can
 * no longer fork is still cleaned up.
 * De-escalation: quiet for fg_cooldown_s -> stage 0.
 */
#include <linux/fork_guard.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/minmax.h>
#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/version.h>

#define FG_MAX_USERS	64

enum fg_tier { FG_SILVER = 0, FG_GOLD, FG_BRONZE };
enum fg_action { FG_ACT_NONE = 0, FG_ACT_FREEZE, FG_ACT_REAP };

/* ------------------------------------------------------------------ */
/* Tunables (sysctl: kernel.fg_*)                                      */
/* ------------------------------------------------------------------ */
static unsigned int fg_enable = 1;
static unsigned int fg_threshold = 200;		/* forks per window */
static unsigned int fg_window_ms = 1000;
static unsigned int fg_throttle_ms = 40;	/* penalty delay */
static unsigned int fg_cooldown_s = 10;
static unsigned int fg_reap_delay_ms = 250;	/* FREEZE -> REAP grace */
static int fg_gold_nice = -7;			/* weight 4904 */
static int fg_bronze_nice = 5;			/* weight 335  */

/* sysctl bounds. The nice clamps are the anti-starvation guarantee:
 * worst case GOLD -10 (9548) vs BRONZE +10 (110) still leaves BRONZE ~1.1%.
 */
static unsigned int fg_u_zero;
static unsigned int fg_u_one = 1;
static unsigned int fg_u_max = 1000000;
static int fg_gold_lo = -10, fg_gold_hi = -1;
static int fg_bronze_lo = 1, fg_bronze_hi = 10;

/* ------------------------------------------------------------------ */
/* Mechanism 1: containment state                                      */
/* ------------------------------------------------------------------ */
struct fg_user {
	kuid_t uid;
	unsigned int count;		/* forks in current window */
	unsigned int stage;		/* 0..3 */
	unsigned int n_throttle;
	unsigned int n_freeze;
	unsigned int n_reap;
	unsigned long win_start;
	unsigned long penalty_until;
	unsigned long last_over;
	unsigned long reap_at;
	struct pid *frozen;		/* group SIGSTOPped, awaiting reap */
};

static struct fg_user fg_users[FG_MAX_USERS];
static unsigned int fg_nusers;
static DEFINE_SPINLOCK(fg_lock);

static void fg_reap_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(fg_reap_dw, fg_reap_fn);

/* caller holds fg_lock */
static struct fg_user *fg_find_or_create(kuid_t uid, unsigned long now)
{
	struct fg_user *u;
	unsigned int i;

	for (i = 0; i < fg_nusers; i++)
		if (uid_eq(fg_users[i].uid, uid))
			return &fg_users[i];

	if (fg_nusers < FG_MAX_USERS) {
		u = &fg_users[fg_nusers++];
	} else {
		/* table full: recycle an idle slot, otherwise fail open */
		for (i = 0; i < FG_MAX_USERS; i++)
			if (!fg_users[i].stage && !fg_users[i].frozen)
				break;
		if (i == FG_MAX_USERS)
			return NULL;
		u = &fg_users[i];
	}
	memset(u, 0, sizeof(*u));
	u->uid = uid;
	u->win_start = now;
	return u;
}

/* caller holds fg_lock: forgive an offender that has been quiet long enough */
static void fg_decay(struct fg_user *u, unsigned long now)
{
	unsigned int cd_s = READ_ONCE(fg_cooldown_s);

	if (u->stage && time_after(now, u->last_over + (unsigned long)cd_s * HZ)) {
		pr_info("fork_guard: FORGIVEN uid=%u (quiet for %u s, stage %u -> 0)\n",
			from_kuid(&init_user_ns, u->uid), cd_s, u->stage);
		u->stage = 0;
		u->count = 0;
		u->win_start = now;
	}
}

static void fg_kill(struct pid *pgrp, int sig)
{
	/* never signal pgid 0/1 */
	if (pgrp && pid_nr(pgrp) > 1)
		kill_pgrp(pgrp, sig, 1);
}

static struct pid *fg_get_cur_pgrp(void)
{
	struct pid *pgrp;

	rcu_read_lock();
	pgrp = get_pid(task_pgrp(current));
	rcu_read_unlock();
	return pgrp;
}

static void fg_do_freeze(struct fg_user *u)
{
	unsigned int delay = READ_ONCE(fg_reap_delay_ms);
	struct pid *pgrp, *old;
	unsigned long flags;

	pgrp = fg_get_cur_pgrp();
	fg_kill(pgrp, SIGSTOP);

	if (!delay) {			/* pure "still over threshold" semantics */
		put_pid(pgrp);
		return;
	}

	spin_lock_irqsave(&fg_lock, flags);
	old = u->frozen;
	u->frozen = pgrp;		/* ownership of the reference moves to u */
	u->reap_at = jiffies + msecs_to_jiffies(delay);
	spin_unlock_irqrestore(&fg_lock, flags);

	put_pid(old);
	schedule_delayed_work(&fg_reap_dw, msecs_to_jiffies(delay));
}

static void fg_do_reap(struct pid *old_frozen)
{
	struct pid *pgrp = fg_get_cur_pgrp();

	fg_kill(pgrp, SIGKILL);
	fg_kill(old_frozen, SIGKILL);
	put_pid(pgrp);
	put_pid(old_frozen);
}

/* Delayed work: reap groups that were frozen and are still in stage 2. */
static void fg_reap_fn(struct work_struct *work)
{
	struct pid *victims[FG_MAX_USERS];
	bool do_kill[FG_MAX_USERS];
	unsigned int nv = 0, i;
	unsigned long flags, now = jiffies;
	unsigned long next = 0;
	bool have_next = false;

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < fg_nusers; i++) {
		struct fg_user *u = &fg_users[i];

		if (!u->frozen)
			continue;
		if (time_before(now, u->reap_at)) {
			unsigned long rem = u->reap_at - now;

			if (!have_next || rem < next) {
				next = rem;
				have_next = true;
			}
			continue;
		}
		victims[nv] = u->frozen;
		u->frozen = NULL;
		if (u->stage == 2) {
			u->stage = 3;
			u->n_reap++;
			u->last_over = now;
			do_kill[nv] = true;
			pr_alert("fork_guard: REAP uid=%u pgid=%d (SIGKILL process group)\n",
				 from_kuid(&init_user_ns, u->uid),
				 pid_nr(victims[nv]));
		} else {
			do_kill[nv] = false;	/* forgiven meanwhile */
		}
		nv++;
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	for (i = 0; i < nv; i++) {
		if (do_kill[i])
			fg_kill(victims[i], SIGKILL);
		put_pid(victims[i]);
	}
	if (have_next)
		schedule_delayed_work(&fg_reap_dw, next ? next : 1);
}

int fg_check_fork(void)
{
	kuid_t kuid;
	uid_t uid;
	struct fg_user *u;
	unsigned long flags, now;
	unsigned int thr, win_ms, thr_ms;
	enum fg_action act = FG_ACT_NONE;
	struct pid *old_frozen = NULL;
	unsigned int delay_ms = 0;
	int ret = 0;

	if (!READ_ONCE(fg_enable))
		return 0;
	kuid = current_uid();
	if (uid_eq(kuid, GLOBAL_ROOT_UID))	/* root is exempt */
		return 0;

	uid = from_kuid(&init_user_ns, kuid);
	thr = READ_ONCE(fg_threshold);
	win_ms = READ_ONCE(fg_window_ms);
	thr_ms = READ_ONCE(fg_throttle_ms);
	now = jiffies;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find_or_create(kuid, now);
	if (!u)
		goto unlock;			/* table exhausted: fail open */

	fg_decay(u, now);

	/* Inside a penalty window: refuse everything from this user. */
	if (time_before(now, u->penalty_until)) {
		u->last_over = now;
		ret = -EAGAIN;
		delay_ms = thr_ms;
		goto unlock;
	}

	if (time_after(now, u->win_start + msecs_to_jiffies(win_ms))) {
		u->win_start = now;
		u->count = 0;
	}
	if (++u->count <= thr)
		goto unlock;

	/* Over the threshold within one window: climb the ladder. */
	u->last_over = now;
	u->count = 0;
	u->win_start = now;
	ret = -EAGAIN;

	if (u->stage == 0) {
		u->stage = 1;
		u->n_throttle++;
		u->penalty_until = now + msecs_to_jiffies(thr_ms);
		delay_ms = thr_ms;
		pr_warn("fork_guard: THROTTLE uid=%u pgid=%d (>%u forks in %u ms)\n",
			uid, task_pgrp_nr_ns(current, &init_pid_ns), thr, win_ms);
	} else if (u->stage == 1) {
		u->stage = 2;
		u->n_freeze++;
		act = FG_ACT_FREEZE;
		pr_warn("fork_guard: FREEZE uid=%u pgid=%d (SIGSTOP process group)\n",
			uid, task_pgrp_nr_ns(current, &init_pid_ns));
	} else {
		u->stage = 3;
		u->n_reap++;
		act = FG_ACT_REAP;
		old_frozen = u->frozen;		/* reap the frozen group too */
		u->frozen = NULL;
		pr_alert("fork_guard: REAP uid=%u pgid=%d (SIGKILL process group)\n",
			 uid, task_pgrp_nr_ns(current, &init_pid_ns));
	}

unlock:
	spin_unlock_irqrestore(&fg_lock, flags);

	if (act == FG_ACT_FREEZE)
		fg_do_freeze(u);
	else if (act == FG_ACT_REAP)
		fg_do_reap(old_frozen);

	if (delay_ms)
		schedule_timeout_killable(msecs_to_jiffies(delay_ms));
	return ret;
}

/* ------------------------------------------------------------------ */
/* Mechanism 2: tiers (independent table + lock)                       */
/* ------------------------------------------------------------------ */
struct fg_tier_ent {
	kuid_t uid;
	enum fg_tier tier;
};

struct fg_change {
	kuid_t uid;
	int nice;
};

static struct fg_tier_ent fg_tiers[FG_MAX_USERS];	/* only GOLD/BRONZE */
static unsigned int fg_ntiers;
static DEFINE_SPINLOCK(fg_tier_lock);

static const char *fg_tier_name(enum fg_tier t)
{
	switch (t) {
	case FG_GOLD:
		return "GOLD";
	case FG_BRONZE:
		return "BRONZE";
	default:
		return "SILVER";
	}
}

/* The clamp: whatever sysctl says, GOLD stays in [-10,-1], BRONZE in [1,10]. */
static int fg_nice_of(enum fg_tier t)
{
	switch (t) {
	case FG_GOLD:
		return clamp(READ_ONCE(fg_gold_nice), -10, -1);
	case FG_BRONZE:
		return clamp(READ_ONCE(fg_bronze_nice), 1, 10);
	default:
		return 0;
	}
}

/* caller holds fg_tier_lock */
static void fg_tier_del(unsigned int i)
{
	fg_tiers[i] = fg_tiers[fg_ntiers - 1];
	WRITE_ONCE(fg_ntiers, fg_ntiers - 1);
}

bool fg_tier_lookup_nice(int *nice)
{
	kuid_t kuid;
	unsigned long flags;
	unsigned int i;
	bool found = false;

	if (!READ_ONCE(fg_ntiers))	/* fast path; also safe very early in boot */
		return false;

	kuid = current_uid();
	spin_lock_irqsave(&fg_tier_lock, flags);
	for (i = 0; i < fg_ntiers; i++) {
		if (uid_eq(fg_tiers[i].uid, kuid)) {
			*nice = fg_nice_of(fg_tiers[i].tier);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&fg_tier_lock, flags);
	return found;
}

static unsigned int fg_tier_snapshot(struct fg_tier_ent *out)
{
	unsigned long flags;
	unsigned int n;

	spin_lock_irqsave(&fg_tier_lock, flags);
	n = fg_ntiers;
	memcpy(out, fg_tiers, n * sizeof(*out));
	spin_unlock_irqrestore(&fg_tier_lock, flags);
	return n;
}

static enum fg_tier fg_tier_of(const struct fg_tier_ent *snap, unsigned int n,
			       kuid_t uid)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (uid_eq(snap[i].uid, uid))
			return snap[i].tier;
	return FG_SILVER;
}

/*
 * Assign a tier. Fills chg[] with the users whose running tasks must be
 * re-niced (at most 2: the target and a demoted GOLD incumbent).
 */
static int fg_tier_set(kuid_t uid, enum fg_tier tier, struct fg_change *chg,
		       int *n)
{
	unsigned long flags;
	int idx = -1, ret = 0;
	unsigned int i;

	*n = 0;
	spin_lock_irqsave(&fg_tier_lock, flags);

	for (i = 0; i < fg_ntiers; i++)
		if (uid_eq(fg_tiers[i].uid, uid))
			idx = i;
	if (idx < 0 && tier != FG_SILVER && fg_ntiers >= FG_MAX_USERS) {
		ret = -ENOSPC;
		goto out;
	}

	/* at most one GOLD: demote the incumbent to SILVER */
	if (tier == FG_GOLD) {
		for (i = 0; i < fg_ntiers; i++) {
			if (fg_tiers[i].tier == FG_GOLD &&
			    !uid_eq(fg_tiers[i].uid, uid)) {
				chg[*n].uid = fg_tiers[i].uid;
				chg[*n].nice = 0;
				(*n)++;
				fg_tier_del(i);
				break;
			}
		}
	}

	idx = -1;			/* indices may have moved */
	for (i = 0; i < fg_ntiers; i++)
		if (uid_eq(fg_tiers[i].uid, uid))
			idx = i;

	if (tier == FG_SILVER) {
		if (idx >= 0)
			fg_tier_del(idx);
	} else if (idx >= 0) {
		fg_tiers[idx].tier = tier;
	} else {
		fg_tiers[fg_ntiers].uid = uid;
		fg_tiers[fg_ntiers].tier = tier;
		WRITE_ONCE(fg_ntiers, fg_ntiers + 1);
	}
	chg[*n].uid = uid;
	chg[*n].nice = fg_nice_of(tier);
	(*n)++;
out:
	spin_unlock_irqrestore(&fg_tier_lock, flags);
	return ret;
}

static void fg_tier_reset(struct fg_change *chg, int *n)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&fg_tier_lock, flags);
	for (i = 0; i < fg_ntiers; i++) {
		chg[i].uid = fg_tiers[i].uid;
		chg[i].nice = 0;
	}
	*n = fg_ntiers;
	WRITE_ONCE(fg_ntiers, 0);
	spin_unlock_irqrestore(&fg_tier_lock, flags);
}

/*
 * Re-tier tasks that are ALREADY running: walk every thread of every process
 * owned by this uid and set its nice value (set_user_nice() re-queues the task
 * and recomputes its CFS weight, so the effect is immediate; no restart).
 */
static void fg_retier_uid(kuid_t uid, int nice)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (t->flags & PF_KTHREAD)
			continue;
		if (!uid_eq(__task_cred(t)->uid, uid))
			continue;
		if (t->policy != SCHED_NORMAL && t->policy != SCHED_BATCH)
			continue;
		set_user_nice(t, nice);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Control file: /proc/fork_guard_ctl  (write-only, root)              */
/*   "set <uid> <gold|silver|bronze>"   |   "reset"                    */
/* ------------------------------------------------------------------ */
static ssize_t fg_ctl_write(struct file *file, const char __user *ubuf,
			    size_t len, loff_t *ppos)
{
	struct fg_change chg[FG_MAX_USERS];
	char buf[64], name[16];
	unsigned int uid;
	int n = 0, i, ret = 0;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "set %u %15s", &uid, name) == 2) {
		enum fg_tier t;
		kuid_t ku = make_kuid(&init_user_ns, uid);

		if (!uid_valid(ku))
			return -EINVAL;
		if (!strcmp(name, "gold"))
			t = FG_GOLD;
		else if (!strcmp(name, "silver"))
			t = FG_SILVER;
		else if (!strcmp(name, "bronze"))
			t = FG_BRONZE;
		else
			return -EINVAL;

		ret = fg_tier_set(ku, t, chg, &n);
		if (ret)
			return ret;
		pr_info("fork_guard: TIER uid=%u -> %s (nice %d)\n", uid,
			fg_tier_name(t), fg_nice_of(t));
		for (i = 0; i < n; i++) {
			if (!uid_eq(chg[i].uid, ku))
				pr_info("fork_guard: TIER uid=%u demoted to SILVER (GOLD is exclusive)\n",
					from_kuid(&init_user_ns, chg[i].uid));
			fg_retier_uid(chg[i].uid, chg[i].nice);
		}
	} else if (!strncmp(buf, "reset", 5)) {
		fg_tier_reset(chg, &n);
		pr_info("fork_guard: TIER reset, all users SILVER\n");
		for (i = 0; i < n; i++)
			fg_retier_uid(chg[i].uid, 0);
	} else {
		return -EINVAL;
	}
	return len;
}

static const struct proc_ops fg_ctl_ops = {
	.proc_write	= fg_ctl_write,
	.proc_lseek	= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* /proc/fork_guard : per-user stats + live tunables (read-only)       */
/* ------------------------------------------------------------------ */
struct fg_snap {
	kuid_t kuid;
	unsigned int forks, stage, nt, nf, nr;
};

static int fg_stats_show(struct seq_file *m, void *v)
{
	struct fg_snap *snap;
	struct fg_tier_ent *tiers;
	unsigned long flags, now = jiffies;
	unsigned long win = msecs_to_jiffies(READ_ONCE(fg_window_ms));
	unsigned int n, nt, i, j;

	/* heap, not stack: two 64-entry arrays blow the 2048-byte frame limit */
	snap = kmalloc_array(FG_MAX_USERS, sizeof(*snap), GFP_KERNEL);
	tiers = kmalloc_array(FG_MAX_USERS, sizeof(*tiers), GFP_KERNEL);
	if (!snap || !tiers) {
		kfree(snap);
		kfree(tiers);
		return -ENOMEM;
	}

	spin_lock_irqsave(&fg_lock, flags);
	n = fg_nusers;
	for (i = 0; i < n; i++) {
		struct fg_user *u = &fg_users[i];

		fg_decay(u, now);
		snap[i].kuid = u->uid;
		snap[i].forks = time_after(now, u->win_start + win) ? 0 : u->count;
		snap[i].stage = u->stage;
		snap[i].nt = u->n_throttle;
		snap[i].nf = u->n_freeze;
		snap[i].nr = u->n_reap;
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	nt = fg_tier_snapshot(tiers);

	seq_printf(m, "enabled=%u threshold=%u window_ms=%u throttle_ms=%u cooldown_s=%u reap_delay_ms=%u gold_nice=%d bronze_nice=%d\n\n",
		   fg_enable, fg_threshold, fg_window_ms, fg_throttle_ms,
		   fg_cooldown_s, fg_reap_delay_ms, fg_nice_of(FG_GOLD),
		   fg_nice_of(FG_BRONZE));
	seq_puts(m, "UID    TIER    FORKS  STAGE  THROTTLE  FREEZE  REAP\n");

	for (i = 0; i < n; i++)
		seq_printf(m, "%-6u %-7s %-6u %-6u %-9u %-7u %-u\n",
			   from_kuid(&init_user_ns, snap[i].kuid),
			   fg_tier_name(fg_tier_of(tiers, nt, snap[i].kuid)),
			   snap[i].forks, snap[i].stage, snap[i].nt, snap[i].nf,
			   snap[i].nr);

	/* tiered users who have never forked */
	for (j = 0; j < nt; j++) {
		bool seen = false;

		for (i = 0; i < n; i++)
			if (uid_eq(snap[i].kuid, tiers[j].uid))
				seen = true;
		if (!seen)
			seq_printf(m, "%-6u %-7s %-6u %-6u %-9u %-7u %-u\n",
				   from_kuid(&init_user_ns, tiers[j].uid),
				   fg_tier_name(tiers[j].tier), 0, 0, 0, 0, 0);
	}
	kfree(snap);
	kfree(tiers);
	return 0;
}

static int fg_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_stats_show, NULL);
}

static const struct proc_ops fg_stats_ops = {
	.proc_open	= fg_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* /proc/sched_tier : tier assignments */
static int fg_tier_show(struct seq_file *m, void *v)
{
	struct fg_tier_ent tiers[FG_MAX_USERS];
	unsigned int n, i;

	n = fg_tier_snapshot(tiers);
	seq_puts(m, "UID    TIER    NICE\n");
	for (i = 0; i < n; i++)
		seq_printf(m, "%-6u %-7s %d\n",
			   from_kuid(&init_user_ns, tiers[i].uid),
			   fg_tier_name(tiers[i].tier),
			   fg_nice_of(tiers[i].tier));
	return 0;
}

static int fg_tier_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_tier_show, NULL);
}

static const struct proc_ops fg_tier_ops = {
	.proc_open	= fg_tier_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ------------------------------------------------------------------ */
/* sysctl: /proc/sys/kernel/fg_*                                       */
/* ------------------------------------------------------------------ */
static struct ctl_table fg_table[] = {
	{
		.procname	= "fg_enable",
		.data		= &fg_enable,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_zero,
		.extra2		= &fg_u_one,
	},
	{
		.procname	= "fg_threshold",
		.data		= &fg_threshold,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_one,
		.extra2		= &fg_u_max,
	},
	{
		.procname	= "fg_window_ms",
		.data		= &fg_window_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_one,
		.extra2		= &fg_u_max,
	},
	{
		.procname	= "fg_throttle_ms",
		.data		= &fg_throttle_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_zero,
		.extra2		= &fg_u_max,
	},
	{
		.procname	= "fg_cooldown_s",
		.data		= &fg_cooldown_s,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_one,
		.extra2		= &fg_u_max,
	},
	{
		.procname	= "fg_reap_delay_ms",
		.data		= &fg_reap_delay_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_u_zero,
		.extra2		= &fg_u_max,
	},
	{
		.procname	= "fg_gold_nice",
		.data		= &fg_gold_nice,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &fg_gold_lo,
		.extra2		= &fg_gold_hi,
	},
	{
		.procname	= "fg_bronze_nice",
		.data		= &fg_bronze_nice,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &fg_bronze_lo,
		.extra2		= &fg_bronze_hi,
	},
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	{ }	/* sentinel needed before 6.11 */
#endif
};

static int __init fork_guard_init(void)
{
	if (!register_sysctl("kernel", fg_table))
		pr_err("fork_guard: sysctl registration failed\n");
	if (!proc_create("fork_guard", 0444, NULL, &fg_stats_ops) ||
	    !proc_create("sched_tier", 0444, NULL, &fg_tier_ops) ||
	    !proc_create("fork_guard_ctl", 0200, NULL, &fg_ctl_ops))
		pr_err("fork_guard: /proc registration failed\n");

	pr_info("fork_guard: initialised (threshold=%u/%ums, gold nice=%d)\n",
		fg_threshold, fg_window_ms, fg_nice_of(FG_GOLD));
	return 0;
}
late_initcall(fork_guard_init);
