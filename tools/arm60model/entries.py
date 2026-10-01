#!/usr/bin/env python3
"""Block entries of the generated code, from the code.bin and codecnt.bin
dumps of the model (executions per code word since the -from frame).

Usage: entries.py code.bin codecnt.bin [frames] [count]

An entry of a block is an execution of its first body word (the word the
same-key links and the entry check lead to); loops inside a block do not
count. With frames, the entries per frame are given too.
"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
cnt_raw = open(sys.argv[2], 'rb').read()
frames = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 20
code, cur, blocks, nb = struct.unpack('<4I', d[:16])
n = (cur - code) // 4
off = 16 + 4 * n
blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
cnt = struct.unpack('<%dI' % n, cnt_raw[:4 * n])
res = []
total = 0
entries_chk = 0
for pc, key, entry, body, nxt in blk:
    if entry == 0 or body >= cur:
        continue
    e = cnt[(body - code) // 4]
    entries_chk += cnt[(entry - code) // 4]
    total += e
    res.append((e, pc, key))
res.sort(reverse=True)
print('blocks %d, body entries %d, entry-check entries %d' % (len(res), total, entries_chk))
if frames:
    print('per frame: %.0f body entries, %.0f through the entry check' % (total / frames, entries_chk / frames))
for e, pc, key in res[:count]:
    print('%9d  pc %04X key %06X' % (e, pc, key))
