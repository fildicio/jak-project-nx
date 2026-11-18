#!/usr/bin/env python3
"""Resolve gk crash-log stack walks against a build's symbol table.

Usage: walk_resolve.py <elf> <base> <addr> [<addr>...]
Base is derived from the decompress_zstd line-43 assert anchor:
the walk's third-from-top same-page value = decompress_zstd + 0x164.
"""

import bisect
import sys

elf, base, addrs = sys.argv[1], int(sys.argv[2], 16), [int(x, 16) for x in sys.argv[3:]]

syms = []
import subprocess
nm = subprocess.run(['xcrun', 'llvm-nm', '-n', '-C', elf],
                     capture_output=True, text=True).stdout
for line in nm.splitlines():
    parts = line.split(' ', 2)
    if len(parts) >= 3 and parts[0].strip():
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        syms.append((a, parts[2].strip()))
syms.sort()

print('base = 0x%x' % base)
for a in addrs:
    off = a - base
    if off < 0:
        print('%#x -> offset NEGATIVE (%#x)  [not in this binary]' % (a, off))
        continue
    i = bisect.bisect_right(syms, (off, '\xff')) - 1
    if i < 0:
        print('%#x -> offset %#x  [below text start]' % (a, off))
        continue
    print('%#x -> offset %#x  %s + %#x' % (a, off, syms[i][1], off - syms[i][0]))
