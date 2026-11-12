#!/bin/sh
# Builds the ARM60 cycle model: the host interpreter (sim) and the image it
# runs (sim.bin, sim.sym), linked from the objects of the last
# "make clean && make" (build/*.o) with the harness and the host stubs,
# and copies the AIF header of that make's console executable
# (takeme/LaunchMe) to sim.aif: sim derives from its sizes the DRAM the
# console leaves to the cartridge with this build.
#
# mk.sh -labels: the translator's assembly files (z80jit_dec.s, _ana.s,
# _blk.s, _emt.s, _glue.s) are assembled again from copies in which every
# label is exported, so that sim.sym names them all: the profiles split the
# routines at their labels, and the translation trace (SIMTRLOG) finds the
# loop heads it stamps. The image is the same, byte for byte.
set -e
cd "$(dirname "$0")"
M=$(cd ../.. && pwd)
B=$M/build
export PATH=$M/bin/compiler/linux:$PATH

JD=$B/z80jit_dec.s.o JA=$B/z80jit_ana.s.o JB=$B/z80jit_blk.s.o JE=$B/z80jit_emt.s.o JG=$B/z80jit_glue.s.o
if [ "$1" = "-labels" ]; then
    L=$(mktemp -d)
    trap 'rm -rf "$L"' EXIT
    for f in z80jit_dec z80jit_ana z80jit_blk z80jit_emt z80jit_glue; do
        # Labels: a name in the first column, not an EQU, RN or SET symbol;
        # their EXPORT lines go after the first AREA line.
        awk 'NR == FNR {
                 if ($0 ~ /^[ \t]+EXPORT[ \t]/) known[$2] = 1
                 else if ($0 ~ /^[A-Za-z_][A-Za-z0-9_]*([ \t]|$)/ && $2 !~ /^(EQU|RN|SETA|SETL|SETS)$/) lab[++n] = $1
                 next
             }
             { print }
             !done && /^[ \t]+AREA[ \t]/ {
                 for (i = 1; i <= n; i++) if (!(lab[i] in known)) { print "        EXPORT  " lab[i]; known[lab[i]] = 1 }
                 done = 1
             }' $M/src/$f.s $M/src/$f.s > $L/$f.s
        armasm -I$M/include/3do -I$M/include/community -I$M/include/ttl -bigend -fpu none -arch 3 \
               -apcs "3/32/nofp/swst" $L/$f.s -o $L/$f.o
    done
    JD=$L/z80jit_dec.o JA=$L/z80jit_ana.o JB=$L/z80jit_blk.o JE=$L/z80jit_emt.o JG=$L/z80jit_glue.o
fi

gcc -O2 -o sim sim.c
armasm -bigend -fpu none -arch 3 -apcs "3/32/nofp/swst" hstub.s -o hstub.o
armcc -I$M/include/3do -I$M/include/community -I$M/include/ttl -I$M/src -DNDEBUG=1 -O2 \
      -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 -apcs "3/32/nofp/swst/wide/softfp" \
      -c harness.c -o harness.o 2>&1 | grep -v "Warning" | grep -v "^harness.c:" || true
armcc -I$M/include/3do -I$M/include/community -I$M/include/ttl -I$M/src -DNDEBUG=1 -O2 \
      -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 -apcs "3/32/nofp/swst/wide/softfp" \
      -c dectest.c -o dectest.o 2>&1 | grep -v "Warning" | grep -v "^dectest.c:" || true
armcc -I$M/include/3do -I$M/include/community -I$M/include/ttl -I$M/src -DNDEBUG=1 -O2 \
      -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 -apcs "3/32/nofp/swst/wide/softfp" \
      -c anatest.c -o anatest.o 2>&1 | grep -v "Warning" | grep -v "^anatest.c:" || true
armcc -I$M/include/3do -I$M/include/community -I$M/include/ttl -I$M/src -DNDEBUG=1 -O2 \
      -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 -apcs "3/32/nofp/swst/wide/softfp" \
      -c emittest.c -o emittest.o 2>&1 | grep -v "Warning" | grep -v "^emittest.c:" || true
armlink -bin -ro-base 0x8000 -o sim.bin -symbols sim.sym hstub.o harness.o dectest.o anatest.o emittest.o \
        $B/game.c.o $B/sms.c.o $B/sms_io.s.o $B/vdp.c.o $B/vdp_a.s.o $B/smsmem.c.o $B/smsmem_a.s.o \
        $B/z80jit.c.o $B/z80jit_emit.c.o $JE $JB $JG $JD $JA $B/z80int_a.s.o $B/dbgview.c.o $B/render.c.o $B/render_a.s.o \
        $B/bench_cpu.c.o $B/bench_z80.s.o $B/z80prog.c.o $B/clock.c.o \
        $M/lib/community/libc.lib > link.txt 2>&1 || { cat link.txt; exit 1; }
head -c 128 $M/takeme/LaunchMe > sim.aif
echo "sim, sim.bin, sim.sym and sim.aif built"
