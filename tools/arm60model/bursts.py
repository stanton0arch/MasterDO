#!/usr/bin/env python3
"""Translation bursts of a model run: what the blocks translated at once in
a burst are, and what could have been done about them.

Usage: bursts.py DIR [sim.sym] [rom]

DIR holds the run.txt (stdout of sim, run with -framelog) and code.bin /
codecnt.bin of a run made with -zones 48 -dramfree 3000000, so that
nothing is evicted: the blocks then sit in the code buffer in the order
they were translated (the cold area takes the zones one after the other),
and the frame of each block is found from the per-frame counts of the
Frame: lines (translations at once plus those of the spare time). The ROM
image defaults to takeme/roms/<name of DIR>.

A burst is a run of frames whose translations at once took 4 ms or more
(frames at most 10 apart merged). For each burst of 10 blocks or more:
- the blocks, the time of the translations at once, the worst frame, the
  free time (budget minus the frame's work) of the 120 frames before;
- what code translated before the burst could have announced: static
  exits (the targets of its link stubs), return points of its calls (the
  call's target is an exit of a block starting at most 256 bytes before
  the call), addresses found in pointer tables of the ROM (a little-endian
  word next to two others pointing into the same 16 KiB slot); and how
  many blocks of the burst are static successors (exits or call return
  points) of other blocks of the burst, the rest being its roots;
- the translations that cost more than the interpreter would have spent
  on the block's executions after it (the block's executed ARM
  instructions / ARM_PER_Z80 * INT_US per Z80 instruction), the burst's
  time shared out by block code size: the bound of a policy that would
  leave such code to the interpreter.
"""
import collections
import os
import re
import struct
import sys

ARM_PER_Z80 = 5.5       # executed ARM instructions per Z80 instruction, translated
INT_US = 6.25           # microseconds per interpreted Z80 instruction (HOT_INT_COST)
CALLS = {0xCD, 0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC}

here = os.path.dirname(os.path.abspath(__file__))
d_dir = sys.argv[1].rstrip('/')
name = os.path.basename(d_dir)
sym_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'sim.sym')
if len(sys.argv) > 3:
    rom_path = sys.argv[3]
else:
    roms = os.path.join(here, '..', '..', 'takeme', 'roms')
    rom_path = os.path.join(roms, name) if os.path.exists(os.path.join(roms, name)) else os.path.join(roms, 'gg', name)

syms = {}
for line in open(sym_path):
    p = line.split()
    if len(p) == 2:
        try:
            syms[p[0]] = int(p[1], 16)
        except ValueError:
            pass
LINK = syms['z80j_glue_link']
rom = open(rom_path, 'rb').read()

d = open(os.path.join(d_dir, 'code.bin'), 'rb').read()
code, cur, _, nb = struct.unpack('<4I', d[:16])
n = (cur - code) // 4
w = struct.unpack('>%dI' % n, d[16:16 + 4 * n])
off = 16 + 4 * n
blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
blk = sorted([b for b in blk if b[2]], key=lambda b: b[2])
ent = [b[2] for b in blk] + [cur]
cnt = struct.unpack('<%dI' % n, open(os.path.join(d_dir, 'codecnt.bin'), 'rb').read()[:4 * n])

txt = open(os.path.join(d_dir, 'run.txt')).read()
nrom = int(re.search(r'ROM translation: (\d+) blocks', txt).group(1))
frames = []
for m in re.finditer(r'^Frame: (\d+) emu (\d+) upd (\d+) draw (\d+) spare (\d+) sync (\d+) sync_us (\d+) '
                     r'forced (\d+) refused (\d+) spare_blocks (\d+)', txt, re.M):
    f = [int(x) for x in m.groups()]
    frames.append(dict(f=f[0], work=f[1] + f[2] + f[3], sync=f[5], sync_us=f[6], pre=f[9]))
byf = {fr['f']: fr for fr in frames}
order = []
for fr in frames:
    order += [fr['f']] * (fr['sync'] + fr['pre'])
frame_of = [-1] * nrom + [order[i] if i < len(order) else None for i in range(len(blk) - nrom)]
mismatch = len(blk) - nrom - len(order)


def target(i, x):
    o = x & 0xFFFFFF
    if o & 0x800000:
        o -= 0x1000000
    return code + 4 * i + 8 + 4 * o


# Static exits of each block: its link stubs, resolved (a plain branch into
# the buffer followed by the Z80 target, the site or 0, the key) or not
# (BL z80j_glue_link followed by the same words).
exits = []
for k, b in enumerate(blk):
    t = set()
    for i in range((b[2] - code) // 4, min((ent[k + 1] - code) // 4, n) - 3):
        x = w[i]
        if (x >> 25) & 7 != 5:
            continue
        if (x >> 24) & 1:
            if target(i, x) == LINK:
                t.add(w[i + 1] & 0xFFFF)
        elif w[i + 1] < 0x10000 and (w[i + 2] == 0 or code <= w[i + 2] < cur) and w[i + 3] < 0x90000 \
                and code <= target(i, x) < cur:
            t.add(w[i + 1])
    exits.append(t)


def rom_off(key, pc):
    kind, bank = key >> 16, key & 0xFFFF
    if kind == 0:
        return pc
    if 1 <= kind <= 3:
        return (bank * 0x4000 + (pc & 0x3FFF)) % len(rom)
    return None


def call_return_point(b):
    pc, o = b[0] & 0xFFFF, rom_off(b[1], b[0] & 0xFFFF)
    return o is not None and pc >= 3 and o >= 3 and rom[o - 3] in CALLS


table_words = set()
for base in range(0, len(rom) - 0x3FFF, 0x4000):
    for start in (0, 1):
        ws = [rom[base + i] | rom[base + i + 1] << 8 for i in range(start, 0x4000 - 1, 2)]
        for i in range(1, len(ws) - 1):
            if ws[i - 1] >> 14 == ws[i] >> 14 == ws[i + 1] >> 14 and ws[i - 1] != ws[i] != ws[i + 1]:
                table_words.add(ws[i])

heavy = [fr['f'] for fr in frames if fr['sync_us'] >= 4000]
wins = []
for f in heavy:
    if wins and f - wins[-1][1] <= 10:
        wins[-1][1] = f
    else:
        wins.append([f, f])

print('%s: %d blocks (%d from the ROM translation, %d while running; frame count mismatch %d)'
      % (name, len(blk), nrom, len(blk) - nrom, mismatch))
tot = collections.Counter()
for a, z in wins:
    ks = [k for k in range(len(blk)) if frame_of[k] is not None and a <= frame_of[k] <= z]
    if len(ks) < 10:
        continue
    us = sum(byf[f]['sync_us'] for f in range(a, z + 1) if f in byf)
    worst = max(byf[f]['work'] for f in range(a, z + 1) if f in byf)
    free = sum(max(0, 16683 - byf[f]['work']) for f in range(max(0, a - 120), a) if f in byf)
    known = set()
    starts = collections.defaultdict(list)
    for k in range(len(blk)):
        if frame_of[k] is not None and frame_of[k] < a:
            known |= exits[k]
            for t in exits[k]:
                starts[t].append(blk[k][0] & 0xFFFF)
    succ = set()
    for k in range(len(blk)):
        if frame_of[k] is not None and frame_of[k] <= z:
            succ |= exits[k]
    c = collections.Counter()
    size = {k: ent[k + 1] - blk[k][2] for k in ks}
    total_size = sum(size.values())
    for k in ks:
        b = blk[k]
        pc, o = b[0] & 0xFFFF, rom_off(b[1], b[0] & 0xFFFF)
        if pc in known:
            c['exit'] += 1
        elif call_return_point(b) and any(0 <= pc - 3 - s < 256
                                          for s in starts.get(rom[o - 2] | rom[o - 1] << 8, [])):
            c['return'] += 1
        elif pc in table_words:
            c['table'] += 1
        if pc in succ or call_return_point(b):
            c['successor'] += 1
        t_b = us * size[k] / total_size
        i_b = sum(cnt[(b[2] - code) // 4:(ent[k + 1] - code) // 4]) / ARM_PER_Z80 * INT_US
        if i_b < t_b:
            c['cheaper'] += 1
            c['cheaper_us'] += t_b
            c['saving_us'] += t_b - i_b
    print('  frames %d-%d: %d blocks, %.1f ms at once, worst frame %.1f ms, %.0f ms free in the 120 frames before;'
          ' announced by earlier code: exits %d, call returns %d, pointer tables %d; successors of burst code %d;'
          ' cheaper interpreted %d (%.0f%% of the time, net %.0f%%)'
          % (a, z, len(ks), us / 1000, worst / 1000, free / 1000, c['exit'], c['return'], c['table'],
             c['successor'], c['cheaper'], 100 * c['cheaper_us'] / us, 100 * c['saving_us'] / us))
    tot.update(c)
    tot['n'] += len(ks)
    tot['us'] += us
if tot['n']:
    N = tot['n']
    print('  all bursts: %d blocks, %.0f ms at once; announced by earlier code: exits %d%%, call returns %d%%,'
          ' pointer tables %d%%; successors of burst code %d%%; cheaper interpreted %.0f%% of the time (net %.0f%%)'
          % (N, tot['us'] / 1000, 100 * tot['exit'] // N, 100 * tot['return'] // N, 100 * tot['table'] // N,
             100 * tot['successor'] // N, 100 * tot['cheaper_us'] / tot['us'], 100 * tot['saving_us'] / tot['us']))
