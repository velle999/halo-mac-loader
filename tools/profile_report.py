#!/usr/bin/env python3
# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Summarizes a profile written by HLE_PROFILE.

    tools/profile_report.py PROFILE LISTING [ROWS]

LISTING is a disassembly of the game's code, as otool -tV or llvm-objdump -d
print it. An address in the game counts toward the function containing it,
a function starting at "pushl %ebp" followed by "movl %esp, %ebp". The
report gives the CPU time by thread and by mapping, the functions and
library symbols that took the most, and, for time spent outside the game,
the game's functions that called out.
"""

import bisect
import collections
import os
import re
import sys


def read_profile(path):
    header = {}
    maps = []
    samples = []
    for line in open(path):
        if line.startswith("# map "):
            m = re.match(r"# map ([0-9a-f]+)-([0-9a-f]+) \S+ \S+ \S+ \S+\s*(.*)",
                         line)
            if m:
                maps.append((int(m.group(1), 16), int(m.group(2), 16),
                             m.group(3).strip()))
        elif line.startswith("# game code "):
            m = re.match(r"# game code (\S+)-(\S+), main thread (\d+)", line)
            header["game"] = (int(m.group(1), 0), int(m.group(2), 0))
            header["main"] = int(m.group(3))
        elif line.startswith("# hle profile:"):
            header["title"] = line[2:].strip()
        elif line.strip() and not line.startswith("#"):
            count, thread, address, caller, symbol = line.split(None, 4)
            samples.append((int(count), int(thread), int(address, 0),
                            int(caller, 0), symbol.strip()))
    return header, sorted(maps), samples


def function_starts(path):
    starts = []
    previous = None
    for line in open(path, errors="replace"):
        m = re.match(r"\s*([0-9a-f]+):\s*(?:[0-9a-f]{2} )*\s*(.*)", line)
        if not m:
            continue
        text = " ".join(m.group(2).split())
        if previous == "pushl %ebp" and text == "movl %esp, %ebp":
            starts.append(address)
        address = int(m.group(1), 16)
        previous = text
    return sorted(starts)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    header, maps, samples = read_profile(sys.argv[1])
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    starts = function_starts(sys.argv[2])
    game_lo, game_hi = header["game"]
    map_starts = [m[0] for m in maps]
    total = sum(s[0] for s in samples) or 1

    def mapping(address):
        if game_lo <= address < game_hi:
            return "game", game_lo
        i = bisect.bisect_right(map_starts, address) - 1
        if i >= 0 and maps[i][0] <= address < maps[i][1]:
            name = os.path.basename(maps[i][2]) if maps[i][2] else "[anonymous]"
            return name, maps[i][0]
        return "[unmapped]", 0

    def function(address):
        i = bisect.bisect_right(starts, address) - 1
        return starts[i] if i >= 0 else address

    def place(address, symbol):
        name, base = mapping(address)
        if name == "game":
            return f"game {function(address):#x}"
        m = re.match(r"(.+)\+(0x[0-9a-f]+|0)$", symbol)
        if m and m.group(1) != "?" and int(m.group(2), 0) < 0x4000:
            return f"{name} {m.group(1)}"
        return f"{name} +{(address - base) & ~0xfff:#x}"

    def thread_name(thread):
        return "main" if thread == header["main"] else f"thread {thread}"

    def table(title, counter):
        print(f"\n{title}")
        for key, n in counter.most_common(rows):
            print(f"  {100.0 * n / total:5.1f}% {n:8d}  {key}")

    threads = collections.Counter()
    mappings = collections.Counter()
    places = collections.Counter()
    callers = collections.Counter()
    for count, thread, address, caller, symbol in samples:
        who = thread_name(thread)
        name = mapping(address)[0]
        threads[who] += count
        mappings[f"{who:14s} {name}"] += count
        places[f"{who:14s} {place(address, symbol)}"] += count
        if name != "game":
            origin = f"game {function(caller):#x}" if caller else "(no game caller)"
            callers[f"{who:14s} {origin} -> {name}"] += count

    print(header.get("title", ""))
    table("By thread", threads)
    table("By thread and mapping", mappings)
    table("Functions and symbols", places)
    table("Outside the game, by the game function that called out", callers)


if __name__ == "__main__":
    main()
