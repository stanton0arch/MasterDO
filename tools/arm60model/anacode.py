#!/usr/bin/env python3
"""Composition of the generated ARM code, from the code.bin dump of the model.

Usage: anacode.py code.bin [sim.sym]

Counts the words of the code buffer by kind: data processing, loads and
stores, branches (named when they go to a symbol of the image), and the
out-of-line stubs of the translator with their data words (segment time-out,
link, special write, RAM block check). Also counts the blocks whose code ran.
"""
import collections
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
sym_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'sim.sym')
syms = {}
for line in open(sym_path):
    p = line.split()
    if len(p) == 2:
        try:
            syms[int(p[1], 16)] = p[0]
        except ValueError:
            pass

d = open(sys.argv[1], 'rb').read()
code, cur, blocks, nb = struct.unpack('<4I', d[:16])
n = (cur - code) // 4
words = struct.unpack('>%dI' % n, d[16:16 + 4 * n])
off = 16 + 4 * n
blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
executed = d[off + 20 * nb: off + 20 * nb + n]

stub_data = {'z80j_glue_seg_timeout': 3, 'z80j_glue_link': 3, 'z80j_glue_write': 3}


def target(i, w):
    o = w & 0xFFFFFF
    if o & 0x800000:
        o -= 0x1000000
    return code + 4 * i + 8 + 4 * o


cat = collections.Counter()
i = 0
while i < n:
    w = words[i]
    kind = (w >> 25) & 7
    if kind == 5:
        name = syms.get(target(i, w))
        link = (w >> 24) & 1
        if link and name in stub_data:
            cat['stub ' + name] += 1 + stub_data[name]
            i += 1 + stub_data[name]
            continue
        if link and name == 'z80j_glue_verify':
            k = 3 + (words[i + 2] + 3) // 4
            cat['stub z80j_glue_verify'] += k
            i += k
            continue
        # A link stub already resolved: a plain branch followed by its
        # data words (Z80 target, jump site or 0, block key).
        if not link and name is None and i + 3 < n and words[i + 1] < 0x10000 and \
                (words[i + 2] == 0 or code <= words[i + 2] < cur) and words[i + 3] < 0x90000:
            cat['stub z80j_glue_link (resolved)'] += 4
            i += 4
            continue
        cat['branch' + (' to ' + name if name else '')] += 1
    elif kind in (2, 3):
        cat['load/store'] += 1
    elif kind == 4:
        cat['ldm/stm'] += 1
    elif (w & 0x0FBF0FFF) == 0x010F0000:
        cat['mrs'] += 1
    else:
        cat['data processing'] += 1
    i += 1

tot = sum(cat.values()) or 1
print('code words %d (%d bytes), blocks %d, %.1f bytes per block'
      % (n, 4 * n, nb, 4.0 * n / max(nb, 1)))
for k, v in cat.most_common():
    print('  %-40s %7d words %5.1f%%' % (k, v, 100.0 * v / tot))

ent = sorted(b[2] for b in blk)
ran = 0
for idx, e in enumerate(ent):
    a = (e - code) // 4
    b = ((ent[idx + 1] if idx + 1 < len(ent) else cur) - code) // 4
    if any(executed[a:b]):
        ran += 1
print('blocks whose code ran: %d of %d' % (ran, len(ent)))
