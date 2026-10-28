#!/bin/bash
# Usage: ./apply_fork_guard.sh /path/to/linux-6.8
# Copies fork_guard.{c,h} into the tree and patches kernel/fork.c,
# kernel/sched/core.c and kernel/Makefile. Idempotent; keeps *.fg.orig backups.
set -euo pipefail

T="${1:?usage: $0 /path/to/linux-6.8}"
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -f "$T/kernel/fork.c" ] && [ -f "$T/kernel/sched/core.c" ] \
    || { echo "error: $T is not a kernel source tree"; exit 1; }

cp "$HERE/fork_guard.c" "$T/kernel/fork_guard.c"
cp "$HERE/fork_guard.h" "$T/include/linux/fork_guard.h"

python3 - "$T" <<'PY'
import re, sys
tree = sys.argv[1]

def patch(path, fn):
    s = open(path).read()
    if 'fork_guard' in s:
        print('already patched:', path)
        return
    out = fn(s)
    open(path + '.fg.orig', 'w').write(s)
    open(path, 'w').write(out)
    print('patched:', path)

def add_include(s):
    m = re.search(r'^#include <linux/[^\n]*\n', s, re.M)
    assert m, 'no include anchor'
    return s[:m.end()] + '#include <linux/fork_guard.h>\n' + s[m.end():]

def fork_c(s):
    s = add_include(s)
    i = s.index('pid_t kernel_clone(struct kernel_clone_args *args)')
    anchor = '\tpid_t nr;\n'
    j = s.index(anchor, i)
    assert j - i < 600, 'kernel_clone layout differs from 6.8'
    k = j + len(anchor)
    ins = ('\n\t/* fork_guard: containment runs first, never consults tiers */\n'
           '\t{\n\t\tint fg_ret = fg_check_fork();\n\n'
           '\t\tif (fg_ret)\n\t\t\treturn fg_ret;\n\t}\n')
    return s[:k] + ins + s[k:]

def core_c(s):
    s = add_include(s)
    i = s.index('int sched_fork(unsigned long clone_flags, struct task_struct *p)')
    j = s.index('\tif (dl_prio(p->prio))', i)
    assert j - i < 3000, 'sched_fork layout differs from 6.8'
    ins = ('\t/* fork_guard: apply the user\'s service tier to the new task */\n'
           '\tif (p->policy == SCHED_NORMAL || p->policy == SCHED_BATCH) {\n'
           '\t\tint fg_nice;\n\n'
           '\t\tif (fg_tier_lookup_nice(&fg_nice)) {\n'
           '\t\t\tp->static_prio = NICE_TO_PRIO(fg_nice);\n'
           '\t\t\tp->prio = p->normal_prio = p->static_prio;\n'
           '\t\t\tset_load_weight(p, false);\n'
           '\t\t}\n\t}\n\n')
    return s[:j] + ins + s[j:]

def makefile(s):
    return s.rstrip('\n') + '\n\nobj-y += fork_guard.o\n'

patch(tree + '/kernel/fork.c', fork_c)
patch(tree + '/kernel/sched/core.c', core_c)
patch(tree + '/kernel/Makefile', makefile)
PY

echo
echo "Done. Next, from $T:"
echo "  make -j\$(nproc) 2>&1 | tee build.log"
echo "  sudo make modules_install install && sudo update-grub && sudo reboot"
