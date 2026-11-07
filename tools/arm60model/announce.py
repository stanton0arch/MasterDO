#!/usr/bin/env python3
"""Translation bursts announced ahead: how much of a burst static analysis of
the code translated before it could name, and at what price.

Usage: announce.py DIR [rom] [depth]

DIR holds the run.txt (stdout of sim, run with -framelog, whose Frame:
lines end with the slot banks) and the blocks.txt (SIMBLOCKLOG) of a run
made with -zones 48 -dramfree 3000000, so that nothing is evicted. The ROM
image defaults to takeme/roms/<name of DIR>.sms or takeme/roms/gg/<name>.gg.

A burst is a run of frames whose translations at once took 4 ms or more
(frames at most 10 apart merged, 10 blocks or more), as in bursts.py. The
blocks are decoded again from the ROM with the translator's rules (a block
runs through conditional jumps and ends at an unconditional transfer; its
static exits are the targets of its jumps and calls, the return points of
its calls and the address after it when it stops early), under the slot
banks of the frame of their translation. Two ways of announcing a burst
are measured:

- Pointer tables: a block ending with JP (HL), JP (IX), JP (IY) or a PUSH
  and RET of a word read from memory is run abstractly to find the table
  its code reads the word from (a constant base plus an unknown index, the
  base split between H and an addition to A included, two tables of low
  and high bytes 256 bytes apart, a table of JP instructions entered by a
  computed jump). The entries of the tables of the blocks translated before
  the burst (up to 64, while they point below $C000) are the announced
  roots. Reported: the burst blocks among them, the burst blocks reachable
  from them through static exits of burst blocks (a walk that would only
  translate burst code: the bound), and a real walk of the given depth
  (default 4) from them: the blocks it translates and how many are burst
  blocks.
- Static walk: the exits of the blocks run in the 120 frames before the
  burst not translated yet, walked to the depth: blocks translated, burst
  blocks among them.

The blocks a walk translates cost about 1 ms and 0.9 KiB of code buffer
each: catching a share of a burst is only worth it with a precision (burst
blocks among the blocks walked) far above that share's cost.
"""
import collections
import os
import re
import sys

# --- Z80 decoding: length, kind, static target, 16-bit constant loaded -----

R8 = ['B', 'C', 'D', 'E', 'H', 'L', '(HL)', 'A']
R16 = ['BC', 'DE', 'HL', 'SP']
R16Q = ['BC', 'DE', 'HL', 'AF']
ALU = ['ADD', 'ADC', 'SUB', 'SBC', 'AND', 'XOR', 'OR', 'CP']
ENDS = ('jp', 'jr', 'ret', 'jpind', 'reti', 'halt', 'call', 'rst')


def decode(mem, pc):
    """Instruction at pc: (length, kind, mnemonic, operands, target, const16).
    kind: 'n', 'jr', 'jp', 'call', 'rst', 'jrcc', 'jpcc', 'callcc', 'djnz',
    'retcc', 'ret', 'reti', 'jpind', 'halt'. mem(addr) gives a byte."""
    b = [mem((pc + i) & 0xFFFF) for i in range(4)]
    op = b[0]
    ix = None
    p = 0
    if op in (0xDD, 0xFD):
        ix = 'IX' if op == 0xDD else 'IY'
        p = 1
        op = b[1]
        if op in (0xDD, 0xFD, 0xED):
            return 1, 'n', 'NOP', [], None, None

    def w(i):
        return b[i] | b[i + 1] << 8

    def r8(r):
        if r == 6 and ix:
            return '(%s+d)' % ix
        if ix and r in (4, 5):
            return ix + 'HL'[r - 4]
        return R8[r]

    def rp(r):
        return ix if (r == 2 and ix) else R16[r]

    if op == 0xCB:
        o = b[3] if ix else b[1]
        x, y = o >> 6, (o >> 3) & 7
        m = '(%s+d)' % ix if ix else R8[o & 7]
        mn = [['RLC', 'RRC', 'RL', 'RR', 'SLA', 'SRA', 'SLL', 'SRL'][y], 'BIT', 'RES', 'SET'][x]
        return (4 if ix else 2), 'n', mn, [m], None, None
    if op == 0xED and not ix:
        o = b[1]
        x, y, z = o >> 6, (o >> 3) & 7, o & 7
        if x == 1 and z == 3:
            return 4, 'n', 'LD', ([R16[y >> 1], '(nn)'] if y & 1 else ['(nn)', R16[y >> 1]]), None, None
        if x == 1 and z == 5:
            return 2, 'reti', 'RETI', [], None, None
        if x == 1 and z == 0:
            return 2, 'n', 'IN', [R8[y] if y != 6 else 'F', '(C)'], None, None
        return 2, 'n', 'ED', [], None, None
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    q = 1 + p
    if x == 0:
        if z == 0:
            if y < 2:
                return 1 + p, 'n', 'NOP' if y == 0 else 'EXAF', [], None, None
            e = b[q] - 256 if b[q] > 127 else b[q]
            tg = (pc + 2 + p + e) & 0xFFFF
            return 2 + p, ['djnz', 'jr', 'jrcc', 'jrcc', 'jrcc', 'jrcc'][y - 2], 'JR', [], tg, None
        if z == 1:
            if y & 1:
                return 1 + p, 'n', 'ADD', [rp(2), rp(y >> 1)], None, None
            return 3 + p, 'n', 'LD', [rp(y >> 1), 'nn'], None, w(q)
        if z == 2:
            if y < 4:
                return 1 + p, 'n', 'LD', [['(BC)', 'A'], ['A', '(BC)'], ['(DE)', 'A'], ['A', '(DE)']][y], None, None
            return 3 + p, 'n', 'LD', [['(nn)', rp(2)], [rp(2), '(nn)'], ['(nn)', 'A'], ['A', '(nn)']][y - 4], None, None
        if z == 3:
            return 1 + p, 'n', 'DEC' if y & 1 else 'INC', [rp(y >> 1)], None, None
        if z in (4, 5):
            return 1 + p + (1 if (y == 6 and ix) else 0), 'n', 'INC' if z == 4 else 'DEC', [r8(y)], None, None
        if z == 6:
            if y == 6 and ix:
                return 4, 'n', 'LD', [r8(6), 'n'], None, None
            return 2 + p, 'n', 'LD', [r8(y), '$%02X' % b[q]], None, None
        return 1 + p, 'n', ['RLCA', 'RRCA', 'RLA', 'RRA', 'DAA', 'CPL', 'SCF', 'CCF'][y], [], None, None
    if x == 1:
        if op == 0x76:
            return 1 + p, 'halt', 'HALT', [], None, None
        if (y == 6 or z == 6) and ix:
            return 2 + p, 'n', 'LD', [r8(y) if y == 6 else R8[y], r8(z) if z == 6 else R8[z]], None, None
        return 1 + p, 'n', 'LD', [r8(y), r8(z)], None, None
    if x == 2:
        return (2 if (z == 6 and ix) else 1) + p, 'n', ALU[y], [r8(z)], None, None
    if z == 0:
        return 1 + p, 'retcc', 'RET', [], None, None
    if z == 1:
        if y & 1 == 0:
            return 1 + p, 'n', 'POP', [ix if (y >> 1 == 2 and ix) else R16Q[y >> 1]], None, None
        if y == 1:
            return 1 + p, 'ret', 'RET', [], None, None
        if y == 3:
            return 1 + p, 'n', 'EXX', [], None, None
        if y == 5:
            return 1 + p, 'jpind', 'JP', [ix or 'HL'], None, None
        return 1 + p, 'n', 'LD', ['SP', ix or 'HL'], None, None
    if z == 2:
        return 3 + p, 'jpcc', 'JP', [], w(q), None
    if z == 3:
        if y == 0:
            return 3 + p, 'jp', 'JP', [], w(q), None
        if y in (2, 3):
            return 2 + p, 'n', 'OUT' if y == 2 else 'IN', (['(n)', 'A'] if y == 2 else ['A', '(n)']), None, None
        if y == 4:
            return 1 + p, 'n', 'EX', ['(SP)', ix or 'HL'], None, None
        if y == 5:
            return 1 + p, 'n', 'EX', ['DE', 'HL'], None, None
        return 1 + p, 'n', 'DI' if y == 6 else 'EI', [], None, None
    if z == 4:
        return 3 + p, 'callcc', 'CALL', [], w(q), None
    if z == 5:
        if y & 1 == 0:
            return 1 + p, 'n', 'PUSH', [ix if (y >> 1 == 2 and ix) else R16Q[y >> 1]], None, None
        return 3 + p, 'call', 'CALL', [], w(q), None
    if z == 6:
        return 2 + p, 'n', ALU[y], ['$%02X' % b[q]], None, None
    return 1 + p, 'rst', 'RST', [], y * 8, None


# --- The run ---------------------------------------------------------------

class Run:
    def __init__(self, d, rom_path):
        rom = open(rom_path, 'rb').read()
        if len(rom) > 512 and len(rom) % 0x4000 == 512:
            rom = rom[512:]             # copier header, skipped as the ISO does
        self.rom = rom
        txt = open(os.path.join(d, 'run.txt')).read()
        self.frames = {}
        for m in re.finditer(r'^Frame: (\d+) .*? sync_us (\d+) .*? banks (\d+) (\d+) (\d+)$', txt, re.M):
            g = [int(v) for v in m.groups()]
            self.frames[g[0]] = dict(sync_us=g[1], banks=g[2:5])
        if not self.frames:
            sys.exit('%s: no Frame: lines with the slot banks (run the model with -framelog)' % d)
        self.blocks = []
        cur = {}
        for line in open(os.path.join(d, 'blocks.txt')):
            p = line.split()
            if p[0] == 'T':
                blk = dict(frame=int(p[1]), pc=int(p[3], 16), key=int(p[4], 16), runs=[])
                self.blocks.append(blk)
                cur[int(p[2])] = blk
            elif p[0] == 'E':
                cur.pop(int(p[2]), None)
            elif p[0] == 'R':
                for s in p[2:]:
                    if int(s) in cur:
                        cur[int(s)]['runs'].append(int(p[1]))
        # Page kind of every 256-byte page, from the keys of the blocks met
        # there (pages without a block: the slot of the page).
        kinds = collections.defaultdict(collections.Counter)
        for blk in self.blocks:
            kinds[blk['pc'] >> 8][blk['key'] >> 16] += 1
        self.kind = [kinds[p].most_common(1)[0][0] if kinds[p] else (8 if p >= 0xC0 else (p >> 6) + 1)
                     for p in range(256)]

    def banks(self, f):
        return self.frames.get(max(f, 0), self.frames[min(self.frames)])['banks']

    def key(self, pc, banks):
        k = self.kind[pc >> 8]
        if k == 0:
            return 0
        if 1 <= k <= 3:
            return k << 16 | banks[k - 1]
        return 0x80000

    def byte(self, key, a):
        kind, bank = key >> 16, key & 0xFFFF
        a &= 0xFFFF
        if kind == 0:
            return self.rom[a % len(self.rom)]
        if 1 <= kind <= 3:
            return self.rom[(bank * 0x4000 + (a & 0x3FFF)) % len(self.rom)]
        return None

    def bursts(self, thr=4000, gap=10, minblocks=10):
        wins = []
        for f in sorted(self.frames):
            if self.frames[f]['sync_us'] >= thr:
                if wins and f - wins[-1][1] <= gap:
                    wins[-1][1] = f
                else:
                    wins.append([f, f])
        out = []
        for a, z in wins:
            ks = [blk for blk in self.blocks if a <= blk['frame'] <= z]
            if len(ks) >= minblocks:
                out.append((a, z, ks))
        return out


def walk_block(r, pc, key, banks):
    """Instructions and static exits [(pc, key)] of the block at pc."""
    ins = []
    exits = []
    p = pc
    for _ in range(64):
        kp = key if (p >> 8) == (pc >> 8) else r.key(p, banks)
        if (kp >> 16) != (key >> 16) or (kp >> 16) >= 8:
            exits.append((p, kp))
            return ins, exits
        it = decode(lambda a, kp=kp: r.byte(kp, a), p)
        ins.append((p,) + it)
        ln, kind, mn, args, tg, c16 = it
        nxt = (p + ln) & 0xFFFF
        if tg is not None and tg < 0xC000:
            same = (tg >> 14) == (pc >> 14) and r.kind[tg >> 8] == (key >> 16)
            exits.append((tg, key if same else r.key(tg, banks)))
        if kind in ('call', 'rst', 'callcc'):
            exits.append((nxt, key if (nxt >> 8) == (p >> 8) else r.key(nxt, banks)))
        if kind in ENDS:
            return ins, exits
        p = nxt
    exits.append((p, r.key(p, banks)))
    return ins, exits


# --- Pointer tables of dispatch blocks --------------------------------------

PAIRS = {'BC': ('B', 'C'), 'DE': ('D', 'E'), 'HL': ('H', 'L'), 'IX': ('IXH', 'IXL'), 'IY': ('IYH', 'IYL'),
         'AF': ('A', 'F')}


def dispatch_table(ins):
    """Abstract run of a block ending with a dynamic transfer. A byte is a
    constant ('c', n), an unknown index plus a constant ('x', n), half of
    a constant base plus an index ('p', base, k), or a byte read from a
    table ('m', address). Returns ('words', base), ('split', low, high),
    ('jump', base) or None."""
    reg = collections.defaultdict(lambda: ('x', 0))
    stack = []

    def pair(n):
        return reg[PAIRS[n][0]], reg[PAIRS[n][1]]

    def setpair(n, h, l):
        reg[PAIRS[n][0]], reg[PAIRS[n][1]] = h, l

    def read(n):
        h, l = pair(n)
        if h[0] == 'c' and l[0] in ('c', 'x'):
            return ('m', h[1] << 8 | l[1])
        if h[0] == 'p' and l[0] == 'p':
            return ('m', (h[1] + l[2]) & 0xFFFF)
        return ('x', 0)

    def word_of(h, l):
        if l[0] == 'm' and h[0] == 'm':
            if h[1] == (l[1] + 1) & 0xFFFF:
                return ('words', l[1])
            if h[1] == (l[1] + 0x100) & 0xFFFF:
                return ('split', l[1], h[1])
        return None

    for p, ln, kind, mn, args, tg, c16 in ins:
        if c16 is not None:
            if args[0] in PAIRS:
                setpair(args[0], ('c', c16 >> 8), ('c', c16 & 0xFF))
        elif mn == 'LD' and len(args) == 2:
            a, s = args
            if a in PAIRS:
                setpair(a, ('x', 0), ('x', 0))
            elif a.startswith('(') or a == 'SP':
                pass
            elif s.startswith('$'):
                reg[a] = ('c', int(s[1:], 16))
            elif s in ('(HL)', '(BC)', '(DE)'):
                reg[a] = read(s[1:3])
            elif s.startswith('(') or s == 'n':
                reg[a] = ('x', 0)
            else:
                reg[a] = reg[s]
        elif mn in ('INC', 'DEC') and not args[0].startswith('('):
            a, dl = args[0], (1 if mn == 'INC' else -1)
            if a in PAIRS:
                h, l = pair(a)
                if h[0] == 'c' and l[0] == 'c':
                    v = ((h[1] << 8 | l[1]) + dl) & 0xFFFF
                    setpair(a, ('c', v >> 8), ('c', v & 0xFF))
                elif l[0] == 'p':
                    setpair(a, ('p', h[1], h[2] + dl), ('p', l[1], l[2] + dl))
                elif l[0] == 'x':
                    reg[PAIRS[a][1]] = ('x', (l[1] + dl) & 0xFF)
            else:
                v = reg[a]
                if a in ('H', 'IXH', 'IYH') and v[0] == 'c' and dl == 1:
                    pass        # carry of an 8-bit addition: the base stays
                elif v[0] in ('c', 'x'):
                    reg[a] = (v[0], (v[1] + dl) & 0xFF)
                elif v[0] == 'p':
                    reg[a] = ('p', v[1], v[2] + dl)
                else:
                    reg[a] = ('x', 0)
        elif mn in ('ADD', 'ADC') and args and args[0] in ('HL', 'IX', 'IY') and len(args) == 2:
            h1, l1 = pair(args[0])
            h2, l2 = pair(args[1])
            c1 = h1[0] == 'c' and l1[0] == 'c'
            c2 = h2[0] == 'c' and l2[0] == 'c'
            if c1 and c2:
                v = ((h1[1] << 8 | l1[1]) + (h2[1] << 8 | l2[1])) & 0xFFFF
                setpair(args[0], ('c', v >> 8), ('c', v & 0xFF))
            elif c1 or c2:
                base = (h1[1] << 8 | l1[1]) if c1 else (h2[1] << 8 | l2[1])
                setpair(args[0], ('p', base, 0), ('p', base, 0))
            elif h2[0] == 'p':
                setpair(args[0], h2, l2)
            elif h1[0] != 'p':
                setpair(args[0], ('x', 0), ('x', 0))
        elif mn in ALU and mn != 'CP':
            s = args[0]
            a = reg['A']
            v = ('c', int(s[1:], 16)) if s.startswith('$') else reg[s] if not s.startswith('(') else ('x', 0)
            if mn == 'XOR' and s == 'A':
                reg['A'] = ('c', 0)
            elif mn in ('ADD', 'ADC') and a[0] in ('c', 'x') and v[0] in ('c', 'x'):
                n = a[1] if s == 'A' else v[1]
                reg['A'] = ('c' if a[0] == v[0] == 'c' else 'x', (a[1] + n) & 0xFF)
            elif mn in ('SUB', 'SBC') and a[0] in ('c', 'x') and v[0] == 'c':
                reg['A'] = (a[0], (a[1] - v[1]) & 0xFF)
            else:
                reg['A'] = ('x', 0)
        elif mn in ('RLCA', 'RLA', 'SLA', 'RL', 'RLC', 'RRCA', 'RRA', 'SRL', 'SRA', 'RR', 'RRC', 'SLL', 'CPL'):
            a = args[0] if args else 'A'
            if not a.startswith('('):
                v = reg[a]
                reg[a] = ('x', (2 * v[1]) & 0xFF) if (mn in ('RLCA', 'RLA', 'SLA', 'RL', 'RLC') and v[0] == 'x') \
                    else ('x', 0)
        elif mn == 'EX' and args == ['DE', 'HL']:
            d, e = pair('DE')
            h, l = pair('HL')
            setpair('DE', h, l)
            setpair('HL', d, e)
        elif mn == 'EX':
            setpair(args[1], ('x', 0), ('x', 0))
        elif mn == 'EXX':
            for n in ('BC', 'DE', 'HL'):
                setpair(n, ('x', 0), ('x', 0))
        elif mn == 'PUSH':
            stack.append(pair(args[0]))
        elif mn == 'POP':
            h, l = stack.pop() if stack else (('x', 0), ('x', 0))
            if args[0] != 'AF':
                setpair(args[0], h, l)
        elif mn == 'IN' and args[0] != 'F':
            reg[args[0]] = ('x', 0)
        elif kind == 'jpind':
            h, l = pair(args[0])
            t = word_of(h, l)
            if t is None and h[0] == 'p':
                t = ('jump', h[1])
            return t
        elif kind == 'ret':
            return word_of(*stack.pop()) if stack else None
    return None


def table_entries(r, tab, key_of, maxn=64):
    out = []
    if tab[0] in ('words', 'split'):
        for i in range(maxn):
            lo_a = (tab[1] + (2 * i if tab[0] == 'words' else i)) & 0xFFFF
            hi_a = (lo_a + 1) if tab[0] == 'words' else (tab[2] + i) & 0xFFFF
            if hi_a >= 0xC000:
                break
            lo, hi = r.byte(key_of(lo_a), lo_a), r.byte(key_of(hi_a), hi_a)
            if lo is None or hi is None or not 0x40 <= (lo | hi << 8) < 0xC000:
                break
            out.append(lo | hi << 8)
    elif tab[1] < 0xC000:
        out.append(tab[1])
        for i in range(1, maxn):
            a = (tab[1] + 3 * i) & 0xFFFF
            if a >= 0xC000 or r.byte(key_of(a), a) != 0xC3:
                break
            out.append(a)
    return out


def main():
    d = sys.argv[1].rstrip('/')
    name = os.path.basename(d)
    here = os.path.dirname(os.path.abspath(__file__))
    roms = os.path.join(here, '..', '..', 'takeme', 'roms')
    rom_path = sys.argv[2] if len(sys.argv) > 2 else next(
        (p for p in (os.path.join(roms, name + '.sms'), os.path.join(roms, 'gg', name + '.gg')) if os.path.exists(p)),
        None)
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    r = Run(d, rom_path)
    cache = {}

    def block(pc, key, banks):
        k = (pc, key, tuple(banks))
        if k not in cache:
            cache[k] = walk_block(r, pc, key, banks)
        return cache[k]

    def walk(roots, done, banks, S):
        seen = set(roots) | done
        level = list(roots)
        walked = set(roots)
        for _ in range(depth):
            nxt = []
            for pc, key in level:
                for e in block(pc, key, banks)[1]:
                    if e not in seen and e[0] < 0xC000:
                        seen.add(e)
                        nxt.append(e)
                        walked.add(e)
            level = nxt
        return len(walked), len(walked & S)

    dyn = found = 0
    for blk in r.blocks:
        banks = r.banks(blk['frame'])
        ins, ex = block(blk['pc'], blk['key'], banks)
        blk['exits'] = set(ex)
        blk['entries'] = []
        if ins and ins[-1][2] in ('jpind', 'ret'):
            dyn += ins[-1][2] == 'jpind'
            tab = dispatch_table(ins)
            if tab:
                found += 1

                def key_of(a, blk=blk, banks=banks):
                    same = (a >> 14) == (blk['pc'] >> 14) and r.kind[a >> 8] == blk['key'] >> 16
                    return blk['key'] if same else r.key(a, banks)

                blk['entries'] = [(e, key_of(e)) for e in table_entries(r, tab, key_of)]
    tot = collections.Counter()
    print('%s: %d blocks; %d end with JP (HL), JP (IX) or JP (IY); pointer tables found in %d dispatch blocks'
          % (name, len(r.blocks), dyn, found))
    for a, z, ks in r.bursts():
        S = {(blk['pc'], blk['key']): blk for blk in ks}
        Sk = set(S)
        done = {(blk['pc'], blk['key']) for blk in r.blocks if blk['frame'] < a}
        succ = set().union(*[blk['exits'] for blk in ks])
        roots = sum(1 for k in S if k not in succ)
        banks = r.banks(a - 1)
        ann = set()
        for blk in r.blocks:
            if blk['frame'] < a:
                ann |= set(blk['entries'])
        ann -= done
        reach = ann & Sk
        todo = list(reach)
        while todo:
            for e in S[todo.pop()]['exits']:
                if e in S and e not in reach:
                    reach.add(e)
                    todo.append(e)
        t_walked, t_hit = walk(ann, done, banks, Sk)
        front = set()
        for blk in r.blocks:
            if blk['frame'] < a and any(a - 120 <= f < a for f in blk['runs']):
                front |= blk['exits']
        front = {e for e in front - done if e[0] < 0xC000}
        f_walked, f_hit = walk(front, done, banks, Sk)
        us = sum(r.frames[f]['sync_us'] for f in range(a, z + 1) if f in r.frames)
        print('  frames %d-%d: %d blocks, %.0f ms at once, %d roots; table entries announced %d (%d burst blocks),'
              ' burst blocks reachable from them %d; walk of depth %d from them %d blocks (%d burst blocks); static'
              ' walk of the code run before %d blocks (%d burst blocks)'
              % (a, z, len(ks), us / 1000, roots, len(ann), len(ann & Sk), len(reach), depth, t_walked, t_hit,
                 f_walked, f_hit))
        for k, v in (('n', len(ks)), ('roots', roots), ('ann', len(ann)), ('annS', len(ann & Sk)),
                     ('reach', len(reach)), ('tw', t_walked), ('th', t_hit), ('fw', f_walked), ('fh', f_hit)):
            tot[k] += v
    n = max(1, tot['n'])
    print('  all bursts: %d blocks, %d roots; tables: %d burst blocks announced, %d reachable (%.0f%%), walk %d'
          ' blocks for %d burst blocks (%.0f%%, precision %.1f%%); static walk %d blocks for %d burst blocks'
          ' (%.0f%%, precision %.1f%%)'
          % (tot['n'], tot['roots'], tot['annS'], tot['reach'], 100 * tot['reach'] / n, tot['tw'], tot['th'],
             100 * tot['th'] / n, 100 * tot['th'] / max(1, tot['tw']), tot['fw'], tot['fh'], 100 * tot['fh'] / n,
             100 * tot['fh'] / max(1, tot['fw'])))


if __name__ == '__main__':
    main()
