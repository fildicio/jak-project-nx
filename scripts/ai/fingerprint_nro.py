#!/usr/bin/env python3
"""Fingerprint the deployed NROs: locate compression::decompress_zstd via its
unique prologue+body instruction bytes and report the page offset of the
line-43 assert call site (bl private_assert_failed).

The instruction sequence (from the f119 ELF disassembly):
  f1001c3f  cmp   x1, #0x7          <- at function_start + 0x10
  aa0003f5  mov   x21, x0
  d1002034  sub   x20, x1, #0x8
  f9400016  ldr   x22, [x0]
  b7f80a16  tbnz  x22, #0x3f, ...
The line-43 assert bl is at function_start + 0x160, so the logged assert lr
is start + 0x164.

NRO layout: vaddr == file offset for the text segment, so a match at file
offset X corresponds to virtual address X directly.
"""
import sys

def le(instr_hex):
    return bytes.fromhex(instr_hex)[::-1]

PATTERN = (le('aa0003f5') + le('d1002034') + le('f9400016') +
           le('b7f80a16'))  # mov x21,x0; sub x20,x1,#8; ldr x22,[x0]; tbnz x22,#0x3f
# this 4-instruction run starts at function_start + 0x18

for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    hits = []
    off = data.find(PATTERN)
    while off != -1 and len(hits) < 4:
        hits.append(off)
        off = data.find(PATTERN, off + 4)
    print(path)
    if not hits:
        print('  no match!')
        continue
    for h in hits:
        start = h - 0x18
        assert_lr = start + 0x164
        print('  func@0x%x  assert lr offset = 0x%x (page offset 0x%03x)' %
              (start, assert_lr, assert_lr & 0xFFF))
