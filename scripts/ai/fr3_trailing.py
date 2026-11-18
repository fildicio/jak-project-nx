#!/usr/bin/env python3
"""Dump the trailing bytes after the first complete zstd frame of an fr3 file."""
import sys

path = sys.argv[1]
d = open(path, 'rb').read()
print('file size', len(d))
if len(d) < 8:
    sys.exit(1)

# find frame end by scanning the frame header, then let zstd CLI tell us:
# we pass the size via argv from zrepro instead. Simple approach here:
# use the zrepro-computed value if given.
if len(sys.argv) > 2:
    frame_end = 8 + int(sys.argv[2], 0)
else:
    frame_end = None

if frame_end is None:
    print('need frame compressed size as arg 2')
    sys.exit(1)

trail = d[frame_end:]
print('trailing len', len(trail))
print(trail[:64].hex())
print(repr(trail[:120]))
