#!/usr/bin/env python3
"""Lists the imports a function reaches, in the order a linear walk meets them.

usage: import_walk.py disassembly.asm HEXADDR [depth] [implemented-names]

The disassembly is llvm-objdump --macho output, whose calls to jump-table
stubs read "symbol stub for: _name". The walk starts at HEXADDR, inside a
function, and runs to the end of that function, descending into direct
calls up to |depth| levels. A function ends where the next call target
begins. With a file of implemented names (one per line, or nm output),
imports missing from it are marked with "!".
"""

import bisect
import re
import sys


def main():
    asm_path = sys.argv[1]
    begin = int(sys.argv[2], 16)
    depth_limit = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    implemented = None
    if len(sys.argv) > 4:
        implemented = set()
        with open(sys.argv[4]) as names:
            for line in names:
                parts = line.split()
                if parts:
                    implemented.add(parts[-1].split("@")[0])

    line_re = re.compile(r"^\s+([0-9a-f]+):\t(\w+)\t?(.*)$")
    stub_re = re.compile(r"symbol stub for: _(\S+)")
    target_re = re.compile(r"^0x([0-9a-f]+)")

    addrs = []
    insns = []
    with open(asm_path) as asm:
        for line in asm:
            m = line_re.match(line)
            if m:
                addrs.append(int(m.group(1), 16))
                insns.append((m.group(2), m.group(3)))

    starts = set()
    for mnemonic, ops in insns:
        if mnemonic == "calll" and "symbol stub" not in ops:
            t = target_re.match(ops)
            if t:
                starts.add(int(t.group(1), 16))
    starts = sorted(starts)

    def function_end(addr):
        i = bisect.bisect_right(starts, addr)
        return starts[i] if i < len(starts) else float("inf")

    seen = set()
    visited = {begin}
    order = []

    def walk(addr, depth):
        end = function_end(addr)
        i = bisect.bisect_left(addrs, addr)
        while i < len(addrs) and addrs[i] < end:
            mnemonic, ops = insns[i]
            if mnemonic == "calll":
                stub = stub_re.search(ops)
                target = target_re.match(ops)
                if stub:
                    name = stub.group(1)
                    if name not in seen:
                        seen.add(name)
                        order.append((addrs[i], depth, name))
                elif target:
                    callee = int(target.group(1), 16)
                    if depth < depth_limit and callee not in visited:
                        visited.add(callee)
                        walk(callee, depth + 1)
            i += 1

    walk(begin, 0)
    for addr, depth, name in order:
        mark = ""
        if implemented is not None:
            base = name.split("$")[0]
            mark = "  " if base in implemented else "! "
        print(f"{addr:8x} {mark}{'  ' * depth}{name}")


if __name__ == "__main__":
    main()
