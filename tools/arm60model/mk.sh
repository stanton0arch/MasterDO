#!/bin/sh
# Builds the ARM60 cycle model: the host interpreter (sim) and the image it
# runs (sim.bin, sim.sym), linked from the objects of the last
# "make clean && make" (build/*.o) with the harness and the host stubs.
set -e
cd "$(dirname "$0")"
M=$(cd ../.. && pwd)
B=$M/build
export PATH=$M/bin/compiler/linux:$PATH

gcc -O2 -o sim sim.c
armasm -bigend -fpu none -arch 3 -apcs "3/32/nofp/swst" hstub.s -o hstub.o
armcc -I$M/include/3do -I$M/include/community -I$M/include/ttl -I$M/src -DNDEBUG=1 -O2 \
      -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 -apcs "3/32/nofp/swst/wide/softfp" \
      -c harness.c -o harness.o 2>&1 | grep -v "Warning" | grep -v "^harness.c:" || true
armlink -bin -ro-base 0x8000 -o sim.bin -symbols sim.sym hstub.o harness.o \
        $B/game.c.o $B/sms.c.o $B/sms_io.s.o $B/vdp.c.o $B/smsmem.c.o $B/smsmem_a.s.o \
        $B/z80jit.c.o $B/z80jit_emit.c.o $B/z80jit_glue.s.o $B/dbgview.c.o $B/render.c.o $B/render_a.s.o \
        $B/bench_cpu.c.o $B/bench_z80.s.o $B/z80prog.c.o \
        $M/lib/community/libc.lib > link.txt 2>&1 || { cat link.txt; exit 1; }
echo "sim, sim.bin and sim.sym built"
