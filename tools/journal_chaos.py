#!/usr/bin/env python3
# M11: the journal chaos sweep — liferaft's discipline applied to the
# machine journal, against the REAL pipeline (QEMU, the real kernel,
# the real store).
#
# Each seeded cycle:
#   1. boots the machine (fresh QEMU, the shared scratch disk),
#   2. drives a fixed 8-unit REPL session (machine readings, dependent
#      arithmetic, a multi-line define, history queries, an fs write),
#   3. KILLS the machine at a seed-derived moment — after unit k, with
#      seed-jittered extra delay, sometimes mid-typing — nothing is
#      saved by hand,
#   4. reboots and runs `journal_replay of "lastboot"`,
#   5. applies the TRANSCRIPT ORACLE: the replayed block must equal,
#      line for line (echoes AND results, including the recorded
#      machine readings), the prefix of the recorded transcript
#      covering exactly the units whose results echoed before death.
#
# The crash model is already reduced by construction: regionfs's
# double-buffered commit means a recovered tape is a COMPLETE earlier
# ring snapshot (old-or-new, never torn bytes), so the adversary's
# remaining axis is TIMING — which units committed before the kill.
#
# Usage: python3 tools/journal_chaos.py [nseeds] [first_seed]
# Needs build/EigenOS.iso + build/tidelog.img (formatted). ~100s/seed.

import socket, subprocess, sys, time, os, re

ISO  = "build/EigenOS.iso"
DISK = "build/tidelog.img"
SOCK = "/tmp/eigenos_chaos_mon.sock"
LOG  = "/tmp/eigenos_chaos_serial.log"

# full char -> sendkey map (a missing char silently drops from the
# typed line — copy the WHOLE map, never a subset)
KMAP = {
    ' ': 'spc', '.': 'dot', ',': 'comma', '-': 'minus', '=': 'equal',
    '[': 'bracket_left', ']': 'bracket_right', '*': 'shift-8',
    '+': 'shift-equal', '!': 'shift-1', '"': 'shift-apostrophe',
    ':': 'shift-semicolon', '_': 'shift-minus', '(': 'shift-9',
    ')': 'shift-0', '<': 'shift-comma', '>': 'shift-dot', '/': 'slash',
    ';': 'semicolon', "'": 'apostrophe', '%': 'shift-5', '&': 'shift-7',
    '#': 'shift-3', '$': 'shift-4', '@': 'shift-2', '^': 'shift-6',
    '?': 'shift-slash', '\\': 'backslash', '|': 'shift-backslash',
    '{': 'shift-bracket_left', '}': 'shift-bracket_right',
    '`': 'grave_accent', '~': 'shift-grave_accent',
}

# the session: (unit lines, expected-result-lines-count is always 1)
UNITS = [
    ["a is ticks of []"],
    ["b is heap_used of []"],
    ["c is a * 2 + 1"],
    ["d is vital_series of [\"heap_used\", 3]"],
    ["buf_len of d"],
    ["define f(n) as:", "    return n + a", ""],
    ["write_text of [\"chaos.txt\", \"cc\"]"],
    ["f of 10"],
]

def mon(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    time.sleep(0.2)
    s.sendall((cmd + "\n").encode())
    time.sleep(0.15)
    s.close()

def type_line(line):
    for ch in line:
        if ch in KMAP:
            mon("sendkey " + KMAP[ch])
        elif ch.isupper():
            mon("sendkey shift-" + ch.lower())
        else:
            mon("sendkey " + ch)
    mon("sendkey ret")

def boot(logpath):
    if os.path.exists(logpath):
        os.unlink(logpath)
    p = subprocess.Popen(
        ["qemu-system-x86_64", "-m", "1024", "-cdrom", ISO,
         "-drive", "file=%s,format=raw,index=0,media=disk" % DISK,
         "-boot", "d", "-serial", "file:" + logpath,
         "-display", "none", "-monitor",
         "unix:%s,server,nowait" % SOCK, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(1)
        if os.path.exists(logpath) and "auto-journal: on" in open(logpath, errors="replace").read():
            time.sleep(1)
            return p
    p.kill()
    raise RuntimeError("boot did not reach the REPL")

def kill(p):
    try:
        mon("quit")
    except Exception:
        pass
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()

def session_units(text):
    """Parse a serial transcript region into completed units:
    each unit = its echo lines (eigs>/....> prefixed, blank-continuation
    lines dropped) + exactly one result line. Incomplete tail is cut."""
    lines = text.splitlines()
    units, cur, have_echo = [], [], False
    for ln in lines:
        if ln.startswith("eigs> ") or ln.startswith("....> "):
            if ln.strip() in ("eigs>", "....>"):
                continue                      # blank submit echo
            if ln.startswith("eigs> ") and have_echo and cur:
                cur = [ln]                    # unit had no result: replace
                continue
            cur.append(ln)
            have_echo = True
        elif have_echo and ln.strip():
            cur.append(ln)                    # the result line
            units.append(cur)
            cur, have_echo = [], False
    return units

def run_seed(seed, last_nonempty):
    # --- record ---
    p = boot(LOG)
    kill_after = seed % len(UNITS)            # units completed before death
    jitter_ms = (seed * 137) % 1400
    for i, unit in enumerate(UNITS):
        if i == kill_after:
            # adversarial timing: die after the jitter — possibly while
            # the next unit is mid-typing
            time.sleep(jitter_ms / 1000.0)
            if seed % 3 == 0 and i < len(UNITS):
                for ch in UNITS[i][0][: (seed % 7) + 1]:
                    if ch in KMAP:
                        mon("sendkey " + KMAP[ch])
                    else:
                        mon("sendkey " + ch)
            break
        for ln in unit:
            type_line(ln)
        time.sleep(2.2)
    recorded = open(LOG, errors="replace").read()
    kill(p)
    rec_region = recorded.split("auto-journal: on", 1)[-1]
    expected = session_units(rec_region)      # completed units only
    # an input-less session never writes the ring: the recorder must NOT
    # overwrite the previous evidence with silence — replay then yields
    # the last non-empty session
    if not expected:
        expected = last_nonempty

    # --- recover ---
    time.sleep(1)
    p = boot(LOG + ".2")
    type_line('journal_replay of "lastboot"')
    time.sleep(6 + len(expected) * 3)
    replayed = open(LOG + ".2", errors="replace").read()
    kill(p)

    m = re.search(r"\[journal\] session replay:\n(.*?)\n\[journal\] session replay complete",
                  replayed, re.S)
    driven = session_units(m.group(1)) if m else []

    ok = driven == expected
    tag = "PASS" if ok else "FAIL"
    print("[seed %2d] kill after unit %d (+%dms)%s: recorded %d units, replayed %d — %s"
          % (seed, kill_after, jitter_ms,
             " mid-typing" if seed % 3 == 0 else "",
             len(expected), len(driven), tag))
    if not ok:
        print("  expected: %r" % expected)
        print("  driven:   %r" % driven)
    return ok, (expected if expected else last_nonempty)

def main():
    nseeds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    fails = 0
    last_nonempty = []
    for seed in range(first, first + nseeds):
        try:
            ok, last_nonempty = run_seed(seed, last_nonempty)
            if not ok:
                fails += 1
        except Exception as e:
            print("[seed %2d] ERROR: %s" % (seed, e))
            fails += 1
    print("chaos sweep: %d/%d seeds clean" % (nseeds - fails, nseeds))
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
