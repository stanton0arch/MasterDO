#!/usr/bin/env python3
"""Code buffer evictions of a model run: what is evicted, what comes back.

Usage: evictions.py DIR [DIR48]

DIR holds a run made with SIMBLOCKLOG=DIR/blocks.txt and -framelog
(run.txt, code.bin); DIR48 the same play with -zones 48 -dramfree 3000000
(nothing evicted), from which the code size of each block is taken (the
order of its code buffer is the translation order). The block log lines
(see sim.c, block_log):
  T frame slot pc key entry           a block translated
  E frame slot pc key entry last runs a block evicted
  R frame slot slot ...               the blocks entered in the frame

Reported: the evictions (frame, zone, blocks, bytes, how long ago their
blocks last ran, how many ever ran), the evicted blocks translated again
later (how soon, in a burst or not: a frame with 4 ms or more of
translations at once), the retranslations' share of the translations at
once (each frame's time shared out by its blocks translated at once).
"""
import collections
import os
import re
import struct
import sys

ZONE_SHIFT = 15


def frames_of(run):
    fr = {}
    for m in re.finditer(r'^Frame: (\d+) emu (\d+) upd (\d+) draw (\d+) spare (\d+) sync (\d+) sync_us (\d+) '
                         r'forced (\d+) refused (\d+) spare_blocks (\d+).*?evict (\d+) evict_ahead (\d+) evict_us (\d+)',
                         open(run).read(), re.M):
        g = [int(x) for x in m.groups()]
        fr[g[0]] = dict(work=g[1] + g[2] + g[3], sync=g[5], sync_us=g[6], pre=g[9], ev=g[10], ahead=g[11], ev_us=g[12])
    return fr


def code_base(d):
    return struct.unpack('<4I', open(os.path.join(d, 'code.bin'), 'rb').read(16))[0]


def read_log(d):
    ev = []
    for line in open(os.path.join(d, 'blocks.txt')):
        p = line.split()
        if p[0] == 'T':
            ev.append(('T', int(p[1]), int(p[2]), int(p[3], 16), int(p[4], 16), int(p[5], 16)))
        elif p[0] == 'E':
            ev.append(('E', int(p[1]), int(p[2]), int(p[3], 16), int(p[4], 16), int(p[5], 16), int(p[6]), int(p[7])))
        elif p[0] == 'R':
            ev.append(('R', int(p[1]), [int(x) for x in p[2:]]))
    return ev


def sizes_from_log(ev, base):
    """Code bytes of each translation (index in ev), from the next block
    translated in the same zone fill."""
    last_in_zone = {}
    size = {}
    for i, e in enumerate(ev):
        if e[0] == 'T':
            z = (e[5] - base) >> ZONE_SHIFT
            if z in last_in_zone:
                j = last_in_zone[z]
                s = e[5] - ev[j][5]
                if 0 < s < 16384:
                    size[j] = s
            last_in_zone[z] = i
        elif e[0] == 'E':
            z = (e[5] - base) >> ZONE_SHIFT
            last_in_zone.pop(z, None)
    return size


def main():
    d = sys.argv[1].rstrip('/')
    d48 = sys.argv[2].rstrip('/') if len(sys.argv) > 2 else None
    fr = frames_of(os.path.join(d, 'run.txt'))
    base = code_base(d)
    ev = read_log(d)
    size_log = sizes_from_log(ev, base)
    size48 = {}
    if d48:
        ev48 = read_log(d48)
        s48 = sizes_from_log(ev48, code_base(d48))
        for i, e in enumerate(ev48):
            if e[0] == 'T' and i in s48:
                size48.setdefault((e[3], e[4]), s48[i])
    burst = {f for f, x in fr.items() if x['sync_us'] >= 4000}

    def bsize(i, key):
        return size48.get(key) or size_log.get(i) or 0

    # Translations: per (pc, key) the frames translated; evictions grouped by frame and zone.
    live = {}                       # slot -> (pc, key, index of T)
    evicted = collections.defaultdict(list)   # (pc, key) -> list of (frame, last, runs)
    groups = collections.OrderedDict()        # (frame, zone) -> list of (pc, key, last, runs, size)
    retrans = []                    # (frame, pc, key, frames since eviction, size)
    first = 0
    trans_frames = collections.Counter()
    ntrans = 0
    for i, e in enumerate(ev):
        if e[0] == 'T':
            _, f, slot, pc, key, entry = e
            live[slot] = (pc, key, i)
            if f >= 0:
                ntrans += 1
                trans_frames[f] += 1
            k = (pc, key)
            # A block translated again without an eviction in between (its first
            # copy dropped by a room check retry) is not a second return.
            if evicted.get(k) and len(evicted[k][-1]) == 3:
                ef, last, runs = evicted[k][-1]
                retrans.append((f, pc, key, f - ef, bsize(i, k), last, runs))
                evicted[k][-1] = (ef, last, runs, 'back')
        elif e[0] == 'E':
            _, f, slot, pc, key, entry, last, runs = e
            t = live.pop(slot, None)
            sz = bsize(t[2], (pc, key)) if t else 0
            z = (entry - base) >> ZONE_SHIFT
            groups.setdefault((f, z), []).append((pc, key, last, runs, sz))
            evicted[(pc, key)].append((f, last, runs))

    # Translation time per block: the frame's time at once over its blocks at once.
    def at_once_us(f):
        x = fr.get(f)
        if not x or x['sync'] == 0:
            return 0.0
        return x['sync_us'] / x['sync']

    print('%s: %d translations while running, %d evictions (%d blocks)' % (
        os.path.basename(d), ntrans, len(groups), sum(len(v) for v in groups.values())))
    print('  eviction       zone blocks  KiB  ran  never  last ran: <60 <600 older frames ago  back later')
    back_keys = {(r[1], r[2], r[0] - r[3]) for r in retrans}
    for (f, z), bl in groups.items():
        kib = sum(b[4] for b in bl) / 1024.0
        ran = sum(1 for b in bl if b[3] > 0)
        ages = [f - b[2] for b in bl if b[2] >= 0]
        a1 = sum(1 for a in ages if a < 60)
        a2 = sum(1 for a in ages if 60 <= a < 600)
        a3 = sum(1 for a in ages if a >= 600)
        back = sum(1 for b in bl if (b[0], b[1], f) in back_keys)
        print('  frame %5d    %4d %6d %4.0f %4d %6d            %3d %4d %5d             %4d' % (
            f, z, len(bl), kib, ran, len(bl) - ran, a1, a2, a3, back))
    nev = sum(len(v) for v in groups.values())
    nback = len(retrans)
    in_burst = [r for r in retrans if r[0] in burst]
    us_back = sum(at_once_us(r[0]) for r in retrans if r[0] in burst)
    us_burst = sum(fr[f]['sync_us'] for f in burst)
    us_all = sum(x['sync_us'] for x in fr.values())
    us_back_all = sum(at_once_us(r[0]) for r in retrans)
    print('  evicted blocks translated again: %d of %d (%.0f%%), %d of them in bursts' % (
        nback, nev, 100.0 * nback / max(1, nev), len(in_burst)))
    gaps = sorted(r[3] for r in retrans)
    if gaps:
        q = lambda p: gaps[min(len(gaps) - 1, int(p * len(gaps)))]
        print('  frames from the eviction to the retranslation: median %d, quartiles %d-%d, %d within 60 frames, %d within 600' % (
            q(0.5), q(0.25), q(0.75), sum(1 for g in gaps if g < 60), sum(1 for g in gaps if g < 600)))
    ages_back = sorted(r[0] - r[3] - r[5] for r in retrans if r[5] >= 0)
    if ages_back:
        q = lambda p: ages_back[min(len(ages_back) - 1, int(p * len(ages_back)))]
        print('  blocks that came back had last run before their eviction: median %d frames, quartiles %d-%d' % (
            q(0.5), q(0.25), q(0.75)))
    print('  retranslations: about %.0f of the %.0f ms of translations at once in bursts (%.0f%%), %.0f of %.0f ms over the run' % (
        us_back / 1000, us_burst / 1000, 100 * us_back / max(1, us_burst), us_back_all / 1000, us_all / 1000))
    kib_back = sum(r[4] for r in retrans) / 1024.0
    print('  code translated again: %.0f KiB' % kib_back)


if __name__ == '__main__':
    main()
