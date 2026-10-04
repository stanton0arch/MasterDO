#!/usr/bin/env python3
"""Generated code time per Z80 block, from the code.bin dump of the model.

Usage: hotblocks.py code.bin [rom] [count]

code.bin is written by the model at the end of a game run (harness.c calls
sim_codedump): code buffer header, code, block descriptors (z80j_block: pc,
key, entry, body, next), one "executed" byte and one float of cycles per
code word. The cycles are those counted since the -from frame. Each block is
listed with its Z80 address, its key (page kind << 16 | bank) and the first
bytes of its code in the ROM.
"""
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
d = open(sys.argv[1], 'rb').read()
rom_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', '..', 'takeme', 'roms', 'rom.sms')
count = int(sys.argv[3]) if len(sys.argv) > 3 else 25
rom = open(rom_path, 'rb').read()
if len(rom) > 512 and len(rom) % 0x4000 == 512:
    rom = rom[512:]                     # copier header, skipped as the ISO does

code, cur, blocks, nb = struct.unpack('<4I', d[:16])
n = (cur - code) // 4
off = 16 + 4 * n
blk = [struct.unpack('>5I', d[off + 20 * k: off + 20 * k + 20]) for k in range(nb)]
off2 = off + 20 * nb + n
cyc = struct.unpack('<%df' % n, d[off2: off2 + 4 * n])
ent = sorted([(b[2], b[0], b[1]) for b in blk])
tot = sum(cyc) or 1.0
res = []
for i, (e, pc, key) in enumerate(ent):
    a = (e - code) // 4
    b = ((ent[i + 1][0] if i + 1 < len(ent) else cur) - code) // 4
    res.append((sum(cyc[a:b]), pc, key))
res.sort(reverse=True)
print('generated code cycles: %.0f' % tot)
for c, pc, key in res[:count]:
    kind = key >> 16
    bank = key & 0xFFFF
    if kind == 0:
        addr = pc
    elif 1 <= kind <= 3:
        addr = bank * 0x4000 + (pc - (kind - 1) * 0x4000)
    else:
        addr = None
    if addr is not None and addr < len(rom):
        text = ' '.join('%02X' % x for x in rom[addr:addr + 24])
    else:
        text = '(RAM)'
    print('%6.2f%%  pc %04X key %06X  %s' % (100 * c / tot, pc, key, text))
