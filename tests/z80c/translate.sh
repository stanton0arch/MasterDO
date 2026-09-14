#!/bin/sh
#
# The cartridge read once, on the PC, its code written as C, proved, and
# chosen under a budget.
#
# Builds the translator (tests/z80c/translate.c) on the host compiler and
# runs it on the image given, translating everything it reaches; links
# that whole table with the core and judges it by its pictures -- the ROM
# run free twice, translated code armed then the interpreter alone, every
# frame of the first held to the frame of the same rank of the second or
# to the one before or after it, a frame that is none of them redrawn in
# colour on both sides and compared byte for byte (tests/z80c/play.sh,
# tests/z80c/sidebyside.c) -- while a free translated run counts how often
# every block ran; runs the translator again with those counts and the
# budget, so that the blocks that ran the most T-states are kept while the
# bytes they cover fit; judges that chosen table the same way; compiles it
# on the host compiler with the same strictness the console chain applies,
# so that a file the console would refuse is refused here first; and only
# then writes src/rom_code.c -- a generated file the repository ignores.
# A red judgement writes nothing: whatever src/rom_code.c held stays as
# it was.
#
# Prints the translator's reports, the runners' lines for both judgements
# and, after them, the size of the host object's code as the estimate of
# what the chosen blocks will weigh in the boot binary.
#
# Without this step, `make` links the empty table (tests/z80c/
# rom_code_none.c) and the console runs the interpreter alone, saying so
# at boot. With it, `make clean && make` links the blocks.
#
#   sh tests/z80c/translate.sh takeme/roms/rom.sms
#   OUT=some/file.c sh tests/z80c/translate.sh <rom>   writes elsewhere
#   Z80C_BUDGET=<bytes> sh tests/z80c/translate.sh <rom>
#                                     the bytes of Z80 code the chosen
#                                     table may cover; the default below
#                                     is what keeps the boot binary under
#                                     its ceiling on the reference ROM
#   FRAMES=<n>                        the judgement's length
#   Z80C_PICREF=<file>                where the interpreter's picture of
#                                     the ROM is taken, or read when the
#                                     file exists (tests/z80c/run_z80c.sh
#                                     keeps it for its mutations); by
#                                     default a file of the work directory
#   Z80C_REDRAW_MAX=<n>               frames of the same screen under other
#                                     palette numbers allowed per judgement
#   Z80C_KEEP=<dir>                   where the two screens of a mismatch
#                                     are copied; by default nothing of a
#                                     run is kept
#
set -e

case "${1:-}" in
  '') echo "usage: sh tests/z80c/translate.sh <rom>"; exit 2;;
  /*) ROM=$1;;
  *)  ROM="$PWD/$1";;
esac
case "${OUT:-}" in ''|/*) ;; *) OUT="$PWD/$OUT";; esac
case "${Z80C_PICREF:-}" in ''|/*) ;; *) Z80C_PICREF="$PWD/$Z80C_PICREF";; esac
case "${Z80C_KEEP:-}" in ''|/*) ;; *) Z80C_KEEP="$PWD/$Z80C_KEEP";; esac

cd "$(dirname "$0")/../.."

CC=${CC:-gcc}
OUT=${OUT:-src/rom_code.c}
S=src
H=tests/cel8/3do
B=tests/z80c
FRAMES=${FRAMES:-3000}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
# A frame is redrawn by a run with one picture every 100000 frames
# (tests/z80c/play.sh): a longer run would take a second picture inside it.
if [ "$FRAMES" -gt 100000 ]; then
  echo "FRAMES must be at most 100000, not $FRAMES"
  exit 2
fi

# The budget, in bytes of Z80 code the chosen blocks may cover. The
# default comes from the measure taken on the reference ROM with the
# console chain: the boot binary grows by about 40 bytes of ARM code per
# byte of Z80 code covered (1500 bytes covered: LaunchMe 146664 bytes
# against 86256 with the empty table; every block: 594204), and it must
# stay under 152 kilobytes with the free memory the boot reports still
# above 32768 bytes. On that ROM 1500 bytes hold the 135 blocks that ran
# the most, three quarters of the T-states of a 3000-frame run.
Z80C_BUDGET=${Z80C_BUDGET:-1500}
# Zero is refused with the two figures above: it writes an empty table,
# and the proof then fails with "no block ran", which accuses the proof
# instead of the setting.
case "$Z80C_BUDGET" in ''|*[!0-9]*|0) echo "Z80C_BUDGET must be a positive integer, not '$Z80C_BUDGET'"; exit 2;; esac

if [ ! -f "$ROM" ]; then
  echo "translate: no rom at $ROM (nothing written)"
  exit 2
fi

# No substitute may shadow a real header of src/ (tests/z80/run_z80.sh).
for h in "$H"/*.h; do
  if [ -e "$S/$(basename "$h")" ]; then
    echo "FAIL: $h shadows $S/$(basename "$h")"
    exit 1
  fi
done

. "$B/play.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The two figures of the cap are the core's (src/z80c.h), read off the
# header and handed to the tool, so that the bound z80.h states for
# z80_run and the cap the tool applies are the same numbers.
CAP=$(sed -n 's/^#define[[:space:]]*Z80C_BLOCK_TSTATES[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$S/z80c.h")
CAPMAX=$(sed -n 's/^#define[[:space:]]*Z80C_BLOCK_TSTATES_MAX[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$S/z80c.h")
if [ -z "$CAP" ] || [ -z "$CAPMAX" ]; then
  echo "FAIL: Z80C_BLOCK_TSTATES or Z80C_BLOCK_TSTATES_MAX not found in $S/z80c.h"
  exit 1
fi

$CC -O1 -std=gnu89 -Wall -Wextra -DBLOCK_TSTATES="${CAP}UL" -DBLOCK_TSTATES_MAX="${CAPMAX}UL" \
    -o "$WORK/translate" tests/z80c/translate.c

# Everything translated, into the work directory: the whole table is what
# the first proof runs and what the counts are taken on. The translator's
# exit status stops the script through set -e.
echo "== translating everything =="
"$WORK/translate" "$ROM" "$WORK/full.c"

echo "== building the runners =="
z80c_runner "$WORK/obj"
z80c_build "$WORK/full.c" "$WORK/full" "$WORK/obj"

# The picture the interpreter draws of every frame: what both tables are
# held to, row for row.
PICREF=${Z80C_PICREF:-$WORK/picture.fnv}
echo "== the interpreter's picture of every frame =="
if ! z80c_reference "$WORK/obj" "$ROM" "$PICREF"; then
  echo "FAIL: nothing to hold the pictures to (nothing written)"
  exit 1
fi

echo "== the whole table: run free, its pictures held to the interpreter's =="
mkdir -p "$WORK/full.d"
if ! z80c_play "$WORK/full" "$ROM" "$WORK/full.d" "$PICREF" "$WORK/obj" "$WORK/counts"; then
  echo "FAIL: the whole table does not draw the interpreter's pictures (nothing written)"
  exit 1
fi
echo "z80c: whole table proved translated=${t_tr}s picture=${t_pic}s redraw=${t_redraw}s"

# The table chosen under the budget, from the counts, and judged on its
# own: what it leaves out is interpreted, and a successor that pointed at
# a block left out is null.
echo "== choosing under Z80C_BUDGET=$Z80C_BUDGET bytes =="
"$WORK/translate" "$ROM" "$WORK/rom_code.c" --counts "$WORK/counts" --budget "$Z80C_BUDGET"
z80c_build "$WORK/rom_code.c" "$WORK/chosen" "$WORK/obj"

echo "== the chosen table: run free, its pictures held to the interpreter's =="
mkdir -p "$WORK/chosen.d"
if ! z80c_play "$WORK/chosen" "$ROM" "$WORK/chosen.d" "$PICREF" "$WORK/obj"; then
  echo "FAIL: the chosen table does not draw the interpreter's pictures (nothing written)"
  exit 1
fi
echo "z80c: chosen table proved translated=${t_tr}s picture=${t_pic}s redraw=${t_redraw}s"

# The file compiled as the console chain will compile it: strict C89,
# optimised so that the code size read off it is an estimate and not a
# debug listing, every warning on and fatal, the SDK stubs standing in
# for the console's headers.
$CC -O2 -std=c89 -Wall -Wextra -Werror -I"$H" -I"$S" \
    -c "$WORK/rom_code.c" -o "$WORK/rom_code.o"

TEXT=$(size "$WORK/rom_code.o" | awk 'NR == 2 { print $1 }')

mv -f "$WORK/rom_code.c" "$OUT"

echo "z80c: est_text=${TEXT:-?}"
echo "written: $OUT"
