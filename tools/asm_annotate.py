#!/usr/bin/env python3
"""Prints part of an llvm-objdump --macho disassembly with its constants named.

usage: asm_annotate.py BINARY DISASSEMBLY LO HI [--all]
       asm_annotate.py BINARY --strings ADDR...

Constants that point into the i386 slice's C strings or CFStrings get the
text they name appended, and printable four-character codes are shown as
text. Without --all only calls, returns and annotated lines are printed.
"""

import re
import struct
import sys

CPU_TYPE_X86 = 7
LC_SEGMENT = 1


class Binary:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        base = 0
        if struct.unpack_from(">I", data, 0)[0] == 0xCAFEBABE:
            count = struct.unpack_from(">I", data, 4)[0]
            for i in range(count):
                cputype, _, offset, _, _ = struct.unpack_from(
                    ">iiIII", data, 8 + i * 20)
                if cputype == CPU_TYPE_X86:
                    base = offset
                    break
            else:
                sys.exit("no i386 slice")
        self.data = data
        self.base = base
        magic, _, _, _, ncmds, _, _ = struct.unpack_from("<7I", data, base)
        if magic != 0xFEEDFACE:
            sys.exit("not a 32-bit little-endian Mach-O")
        self.sections = []
        at = base + 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, at)
            if cmd == LC_SEGMENT:
                nsects = struct.unpack_from("<I", data, at + 48)[0]
                for s in range(nsects):
                    sect = at + 56 + s * 68
                    name = data[sect:sect + 16].rstrip(b"\0").decode()
                    seg = data[sect + 16:sect + 32].rstrip(b"\0").decode()
                    addr, length, offset = struct.unpack_from(
                        "<III", data, sect + 32)
                    self.sections.append((addr, length, offset, seg, name))
            at += size

    def section(self, addr):
        for start, length, offset, seg, name in self.sections:
            if start <= addr < start + length:
                return start, offset, seg, name
        return None

    def read(self, addr, n):
        found = self.section(addr)
        if not found or found[1] == 0:
            return None
        start, offset, _, _ = found
        at = self.base + offset + addr - start
        return self.data[at:at + n]

    def c_string(self, addr, limit=120):
        raw = self.read(addr, 4096)
        if raw is None:
            return None
        raw = raw.split(b"\0", 1)[0]
        text = raw[:limit].decode("mac_roman")
        text = text.replace("\\", "\\\\").replace("\n", "\\n")
        text = text.replace("\r", "\\r").replace("\t", "\\t")
        return '"' + text + ('..."' if len(raw) > limit else '"')

    def describe(self, addr):
        found = self.section(addr)
        if not found:
            return None
        _, _, seg, name = found
        if name in ("__cstring", "__literal4", "__literal8") and \
                name == "__cstring":
            return self.c_string(addr)
        if name == "__cfstring":
            raw = self.read(addr, 16)
            if raw and len(raw) == 16:
                _, _, pointer, _ = struct.unpack("<4I", raw)
                text = self.c_string(pointer)
                if text:
                    return "@" + text
        if seg == "__TEXT" and name in ("__text", "__StaticInit",
                                        "__textcoal_nt"):
            return None
        raw = self.read(addr, 5)
        if raw and len(raw) == 5 and all(32 <= b < 127 for b in raw[:4]):
            text = self.c_string(addr)
            if text and len(text) > 5:
                return text + " (%s,%s)" % (seg, name)
        return "(%s,%s)" % (seg, name)


def four_char(value):
    raw = struct.pack(">I", value)
    if all(32 <= b < 127 for b in raw) and any(65 <= b <= 122 for b in raw):
        return "'" + raw.decode("ascii") + "'"
    return None


def main():
    binary = Binary(sys.argv[1])
    if sys.argv[2] == "--strings":
        for arg in sys.argv[3:]:
            addr = int(arg, 16)
            print("%8x %s" % (addr, binary.describe(addr)))
        return
    asm_path = sys.argv[2]
    lo = int(sys.argv[3], 16)
    hi = int(sys.argv[4], 16)
    show_all = "--all" in sys.argv[5:]
    line_re = re.compile(r"^\s+([0-9a-f]+):\t")
    const_re = re.compile(r"(\$?)0x([0-9a-f]+)")
    with open(asm_path) as asm:
        for line in asm:
            m = line_re.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            if addr < lo:
                continue
            if addr >= hi:
                break
            line = line.rstrip()
            notes = []
            if "##" not in line:
                body = line[m.end():]
                for c in const_re.finditer(body):
                    value = int(c.group(2), 16)
                    mnemonic = body.split("\t", 1)[0]
                    if mnemonic.startswith("j") or mnemonic == "calll":
                        continue
                    note = binary.describe(value) if value >= 0x1000 else None
                    if note is None and c.group(1) and value > 0xFFFFFF:
                        note = four_char(value)
                    if note:
                        notes.append(note)
            interesting = notes or re.search(r"\tcall|\tret|\tjmp\t.*##", line)
            if show_all or interesting:
                print(line + ("  ; " + "; ".join(notes) if notes else ""))


if __name__ == "__main__":
    main()
