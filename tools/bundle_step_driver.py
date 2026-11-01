#!/usr/bin/env python3
"""Emit the hosted step() driver for one ROM-bundle task module (hq#58).

Used by layer 3 of tools/bundle_load_hosted.sh:

    bundle_step_driver.py STAGE_DIR STEM KERNEL_NAMES_FILE ROUNDS

prints an EigenScript program that, run from STAGE_DIR, imports STEM (and
every bundle module it imports, transitively), binds a stub for each kernel
builtin those modules use into that module's namespace, then calls
`STEM.step of []` ROUNDS times. On the machine, task_spawn/task_step run
exactly that call (src/hal.c, `step of []`), and a step error kills the task.

The stubs model an idle machine: every input queue is empty (win_key,
irq_read, kbd_take return -1; gfx_poll, net_recv return null), the task has
a 320x200 viewport, the vitals are fixed, and everything else returns 0. A
module binding shadows a hosted builtin of the same name (gfx_*), so the
stubs replace those too. A name the module binds itself (`define NAME(` or
`NAME is` at the top level) is never overwritten.
"""
import os
import re
import sys

IDLE = {
    "win_key": "0 - 1", "irq_read": "0 - 1", "kbd_take": "0 - 1",
    "gfx_poll": "null", "net_recv": "null",
    "win_w": "320", "win_h": "200",
    "heap_used": "512", "heap_mapped": "1024",
    "mem_free_pages": "512", "mem_total_pages": "1024",
    "ticks": "1000", "tick_hz": "100",
}


def main():
    st, stem, kfile, rounds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    kernel = sorted(set(open(kfile).read().split()))

    def src(m):
        with open(os.path.join(st, m + ".eigs")) as fh:
            return fh.read()

    mods, todo = [], [stem]
    while todo:  # the module and every bundle module it imports, transitively
        m = todo.pop(0)
        if m in mods:
            continue
        mods.append(m)
        for i in re.findall(r"^import ([A-Za-z_][A-Za-z0-9_]*)[ \t]*$", src(m), re.M):
            if os.path.exists(os.path.join(st, i + ".eigs")) and i not in mods:
                todo.append(i)

    out = ["import " + m for m in mods]
    for k in kernel:
        out += ["define _k_%s(a) as:" % k, "    return %s" % IDLE.get(k, "0")]
    for m in mods:
        code = "\n".join(line.split("#", 1)[0] for line in src(m).splitlines())
        for k in kernel:
            if not re.search(r"\b%s\b" % re.escape(k), code):
                continue
            if re.search(r"^define %s\(|^%s is " % (re.escape(k), re.escape(k)), code, re.M):
                continue  # the module's own binding: never overwritten
            out.append("%s.%s is _k_%s" % (m, k, k))
    out += [
        "_round is 0",
        "loop while _round < %d:" % rounds,
        "    %s.step of []" % stem,
        "    _round is _round + 1",
    ]
    print("\n".join(out))


if __name__ == "__main__":
    main()
