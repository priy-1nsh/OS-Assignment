/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>

/*
 * Called at the very top of kernel_clone().
 * Returns 0 to allow the fork, or a negative errno (-EAGAIN) to refuse it.
 * Never consults the tier table.
 */
int fg_check_fork(void);

/*
 * Called from sched_fork() for a new fair-class task.
 * Returns true and fills *nice if current's user is GOLD or BRONZE.
 * SILVER (default) returns false: the task keeps its inherited nice value.
 */
bool fg_tier_lookup_nice(int *nice);

#endif /* _LINUX_FORK_GUARD_H */
