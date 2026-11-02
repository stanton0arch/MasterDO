#!/usr/bin/env python3
"""Compares the code buffers of two builds, from their code.bin dumps.

Usage: cmpcode.py A/code.bin A/sim.sym B/code.bin B/sim.sym

The generated code must be identical word for word, except for the
branches that leave the buffer (to the glue or the translator), which are
compared by the symbol they reach in each build, since the image's layout
moves between builds. The block descriptors are compared too (address,
key, entry and body as offsets into the buffer, in their order), so that
the exits are reported in the same order. Prints the first differences and
a summary line; exits with 1 when the buffers differ.
"""
import struct
import sys


def load_syms(path):
    syms = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2:
            try:
                syms[int(p[1], 16)] = p[0]
            except ValueError:
                pass
    return syms


def load(path):
    d = open(path, 'rb').read()
    code, cur, blocks, nb = struct.unpack('<4I', d[:16])
    n = (cur - code) // 4
    words = struct.unpack('>%dI' % n, d[16:16 + 4 * n])
    off = 16 + 4 * n
    blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
    return code, cur, words, blk


def target(code, k, w):
    o = w & 0xFFFFFF
    if o & 0x800000:
        o -= 0x1000000
    return code + 4 * k + 8 + 4 * o


def main():
    ca, cura, wa, ba = load(sys.argv[1])
    sa = load_syms(sys.argv[2])
    cb, curb, wb, bb = load(sys.argv[3])
    sb = load_syms(sys.argv[4])
    bad = 0
    shown = 0
    if len(wa) != len(wb):
        print('code: %d words against %d' % (len(wa), len(wb)))
        bad += 1
    n = min(len(wa), len(wb))
    for k in range(n):
        x, y = wa[k], wb[k]
        if x == y:
            continue
        # A branch out of the buffer in both: the same symbol is the same code.
        if (x & 0x0E000000) == 0x0A000000 and (y & 0x0E000000) == 0x0A000000 and \
                (x >> 24) == (y >> 24):
            tx, ty = target(ca, k, x), target(cb, k, y)
            outx = not (ca <= tx < cura)
            outy = not (cb <= ty < curb)
            if outx and outy and sa.get(tx) is not None and sa.get(tx) == sb.get(ty):
                continue
        bad += 1
        if shown < 10:
            print('word %d (offset %#x): %08x against %08x' % (k, 4 * k, x, y))
            shown += 1
    # The descriptors: the live ones, in order.
    da = [(p & 0xFFFF, key, e - ca if e else 0, b - ca if e else 0) for p, key, e, b, nx in ba if e]
    db = [(p & 0xFFFF, key, e - cb if e else 0, b - cb if e else 0) for p, key, e, b, nx in bb if e]
    if da != db:
        bad += 1
        print('descriptors: %d against %d' % (len(da), len(db)))
        for k in range(min(len(da), len(db))):
            if da[k] != db[k]:
                print('descriptor %d: %r against %r' % (k, da[k], db[k]))
                break
    print('%d words, %d descriptors, %d differences' % (n, len(da), bad))
    sys.exit(1 if bad else 0)


main()
