#!/usr/bin/env python3
"""Resolve addresses to (function, offset) via a sorted nm dump of the gk ELF."""
import bisect, sys

syms = []
for line in open('/tmp/nms.txt'):
    parts = line.split(' ', 2)
    if len(parts) >= 3 and parts[0].strip():
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        syms.append((a, parts[2].strip()))
syms.sort()

for a in [int(x, 16) for x in sys.argv[1:]]:
    i = bisect.bisect_right(syms, (a, '\xff')) - 1
    if i < 0:
        print(hex(a), '-> ?')
        continue
    off = a - syms[i][0]
    print(hex(a), '->', syms[i][1], '+', hex(off))
