#!/usr/bin/env python3
"""Disassembly of the generated code of a Z80 block, from the code.bin
dump of the model, with the cycles spent on each ARM instruction.

Usage: disblock.py code.bin pc [key] [sim.sym]
       disblock.py code.bin rank N [sim.sym]     (N-th hottest block, from 1)

Only the instruction forms the translator emits are decoded (data
processing, single loads and stores, branches, MRS); anything else is
shown as a data word.
"""
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
d = open(sys.argv[1], 'rb').read()
sym_path = os.path.join(here, 'sim.sym')
args = sys.argv[2:]
if args and args[-1].endswith('.sym'):
    sym_path = args.pop()
syms = {}
for line in open(sym_path):
    p = line.split()
    if len(p) == 2:
        try:
            syms[int(p[1], 16)] = p[0]
        except ValueError:
            pass

code, cur, blocks, nb = struct.unpack('<4I', d[:16])
n = (cur - code) // 4
words = struct.unpack('>%dI' % n, d[16:16 + 4 * n])
off = 16 + 4 * n
blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
off2 = off + 20 * nb + n
cyc = struct.unpack('<%df' % n, d[off2: off2 + 4 * n])
ent = sorted([(b[2], b[0], b[1]) for b in blk])

COND = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']
DP = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']
SH = ['lsl', 'lsr', 'asr', 'ror']


def reg(r):
    return {10: 'r10', 12: 'r12', 13: 'sp', 14: 'lr', 15: 'pc'}.get(r, 'r%d' % r)


def shifter(w):
    if w & (1 << 25):
        imm = w & 0xFF
        rot = (w >> 8) & 15
        v = ((imm >> (2 * rot)) | (imm << (32 - 2 * rot))) & 0xFFFFFFFF if rot else imm
        return '#0x%x' % v
    rm = reg(w & 15)
    typ = (w >> 5) & 3
    if w & 0x10:
        return '%s,%s %s' % (rm, SH[typ], reg((w >> 8) & 15))
    amt = (w >> 7) & 31
    if amt == 0 and typ == 0:
        return rm
    if amt == 0 and typ in (1, 2):
        amt = 32
    if amt == 0 and typ == 3:
        return '%s,rrx' % rm
    return '%s,%s #%d' % (rm, SH[typ], amt)


def dis(addr, w):
    c = COND[w >> 28]
    if (w & 0x0E000000) == 0x0A000000:
        o = w & 0xFFFFFF
        if o & 0x800000:
            o -= 0x1000000
        t = addr + 8 + 4 * o
        name = syms.get(t)
        return 'b%s%s %s' % ('l' if w & (1 << 24) else '', c, name if name else '0x%x' % t)
    if (w & 0x0FBF0FFF) == 0x010F0000:
        return 'mrs%s %s,cpsr' % (c, reg((w >> 12) & 15))
    if (w & 0x0C000000) == 0x04000000:
        ld = 'ldr' if w & (1 << 20) else 'str'
        b = 'b' if w & (1 << 22) else ''
        rd = reg((w >> 12) & 15)
        rn = reg((w >> 16) & 15)
        u = '' if w & (1 << 23) else '-'
        pre = w & (1 << 24)
        wb = '!' if (w & (1 << 21)) and pre else ''
        if w & (1 << 25):
            o = shifter(w & ~(1 << 25))
            o = u + o
        else:
            o = '#%s%d' % (u, w & 0xFFF)
        if pre:
            return '%s%s%s %s,[%s,%s]%s' % (ld, c, b, rd, rn, o, wb)
        return '%s%s%s %s,[%s],%s' % (ld, c, b, rd, rn, o)
    if (w & 0x0C000000) == 0:
        op = (w >> 21) & 15
        s = 's' if w & (1 << 20) else ''
        rd = reg((w >> 12) & 15)
        rn = reg((w >> 16) & 15)
        if op in (8, 9, 10, 11):
            return '%s%s %s,%s' % (DP[op], c, rn, shifter(w))
        if op in (13, 15):
            return '%s%s%s %s,%s' % (DP[op], c, s, rd, shifter(w))
        return '%s%s%s %s,%s,%s' % (DP[op], c, s, rd, rn, shifter(w))
    if (w & 0x0E000000) == 0x08000000:
        return '%s%s %s' % ('ldm' if w & (1 << 20) else 'stm', c, reg((w >> 16) & 15))
    return '.word 0x%08x' % w


if args[0] == 'rank':
    rank = int(args[1])
    res = []
    for i, (e, pc, key) in enumerate(ent):
        a = (e - code) // 4
        b = ((ent[i + 1][0] if i + 1 < len(ent) else cur) - code) // 4
        res.append((sum(cyc[a:b]), i))
    res.sort(reverse=True)
    sel = [res[rank - 1][1]]
else:
    pc = int(args[0], 16)
    key = int(args[1], 16) if len(args) > 1 else None
    sel = [i for i, (e, p, k) in enumerate(ent) if p == pc and (key is None or k == key)]

for i in sel:
    e, pc, key = ent[i]
    a = (e - code) // 4
    b = ((ent[i + 1][0] if i + 1 < len(ent) else cur) - code) // 4
    tot = sum(cyc[a:b])
    print('block pc %04X key %06X: %d words, %.0f cycles' % (pc, key, b - a, tot))
    for k in range(a, b):
        addr = code + 4 * k
        print('  %08x %08x %10.0f  %s' % (addr, words[k], cyc[k], dis(addr, words[k])))
