#!/bin/sh
#
# The cartridge read once, on the PC, its code written as C, proved, and
# chosen under a budget.
#
# Builds the translator (tests/z80c/translate.c) on the host compiler and
# runs it on the image given, translating everything it reaches; runs
# that whole table on the PC, which records the positions the
# interpreter still ran from -- the code the walk cannot see behind a
# dispatch table -- and translates again with them as seeds, until a run
# adds no position; judges the whole table against the interpreter led
# by the same clock (tests/z80c/play.sh, tests/z80c/sidebyside.c: every
# frame, rows, colours and memory, no shift) while the free run counts
# how often every block ran; runs the translator again with those counts
# and the budget, so that the blocks that ran the most instructions are
# kept while the bytes they cover fit, the waits always in; judges that
# chosen table the same way; compiles it on the host compiler with the
# same strictness the console chain applies, so that a file the console
# would refuse is refused here first; holds its text to naming nothing
# of the interpreter's clock (Z80_SPEND, Z80_R, TSTATES); and only then
# writes src/rom_code.c -- a generated file the repository ignores. A red
# judgement writes nothing: whatever src/rom_code.c held stays as it
# was. A program the runners refuse -- one that never waits, one that
# executes code from RAM -- writes nothing either, and the script exits
# 4 with the runner's line.
#
# Prints the translator's reports, the runners' lines for both judgements
# -- the verdict, and beside it, for information, how the pictures stand
# against the classic interpreter -- and, after them, the size of the
# host object's code as the estimate of what the chosen blocks will weigh
# in the boot binary.
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
#   Z80C_PICREF=<file>                where the classic interpreter's
#                                     picture of the ROM is taken, or
#                                     read when the file exists
#                                     (tests/z80c/run_z80c.sh keeps it);
#                                     by default a file of the work
#                                     directory
#   Z80C_EVREF=<file>                 where the chosen table's reference
#                                     -- the same table, blocks not
#                                     executed -- is kept (run_z80c.sh
#                                     holds its mutations to it); by
#                                     default a file of the work directory
#   Z80C_KEEP=<dir>                   where the two screens of a mismatch
#                                     are copied; by default nothing of a
#                                     run is kept
#   Z80C_ROUNDS=<n>                   the most rounds of seeding (below);
#                                     8 by default. A run that still adds
#                                     positions after the last round is
#                                     translated once more with every
#                                     seed found and judged as it stands
#
set -e

case "${1:-}" in
  '') echo "usage: sh tests/z80c/translate.sh <rom>"; exit 2;;
  /*) ROM=$1;;
  *)  ROM="$PWD/$1";;
esac
case "${OUT:-}" in ''|/*) ;; *) OUT="$PWD/$OUT";; esac
case "${Z80C_PICREF:-}" in ''|/*) ;; *) Z80C_PICREF="$PWD/$Z80C_PICREF";; esac
case "${Z80C_EVREF:-}" in ''|/*) ;; *) Z80C_EVREF="$PWD/$Z80C_EVREF";; esac
case "${Z80C_KEEP:-}" in ''|/*) ;; *) Z80C_KEEP="$PWD/$Z80C_KEEP";; esac

cd "$(dirname "$0")/../.."

CC=${CC:-gcc}
OUT=${OUT:-src/rom_code.c}
S=src
H=tests/cel8/3do
B=tests/z80c
FRAMES=${FRAMES:-3000}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
# A screen is redrawn by a run with one picture every 100000 frames
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
# above 32768 bytes. That measure was taken on blocks that kept an
# account of time; the figure is kept until the chain measures the new
# ones.
Z80C_BUDGET=${Z80C_BUDGET:-1500}
# Zero is refused with the two figures above: it writes an empty table,
# and the proof then fails with "no block ran", which accuses the proof
# instead of the setting.
case "$Z80C_BUDGET" in ''|*[!0-9]*|0) echo "Z80C_BUDGET must be a positive integer, not '$Z80C_BUDGET'"; exit 2;; esac

# The rounds of seeding: a run that adds no position ends them.
Z80C_ROUNDS=${Z80C_ROUNDS:-8}
case "$Z80C_ROUNDS" in ''|*[!0-9]*) echo "Z80C_ROUNDS must be a non-negative integer, not '$Z80C_ROUNDS'"; exit 2;; esac

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

$CC -O1 -std=gnu89 -Wall -Wextra -o "$WORK/translate" tests/z80c/translate.c

echo "== building the runners =="
z80c_runner "$WORK/obj"

# The classic interpreter's picture of every frame: what both tables are
# held beside, for information.
PICREF=${Z80C_PICREF:-$WORK/picture.fnv}
echo "== the classic interpreter's picture of every frame =="
if ! z80c_reference "$WORK/obj" "$ROM" "$PICREF"; then
  echo "FAIL: the classic interpreter's picture was not taken (nothing written)"
  exit 1
fi

# Everything translated, into the work directory, then run and seeded
# until the walk reaches what the run reaches. The whole table is what
# the first judgement runs and what the counts are taken on. A refusal
# of the program stands once seeding adds nothing.
echo "== translating everything =="
"$WORK/translate" "$ROM" "$WORK/full.c"
mkdir -p "$WORK/full.d"
round=0
seeds=
while :; do
  z80c_build "$WORK/full.c" "$WORK/full" "$WORK/obj"
  # A file of the round before must not pass for this round's.
  rm -f "$WORK/seeds.new"
  set +e
  "$WORK/full" "$ROM" "$FRAMES" translated "$WORK/counts" "$WORK/seeds.new" >"$WORK/full.d/seed.out" 2>"$WORK/full.d/seed.log"
  rrc=$?
  set -e
  if [ ! -f "$WORK/seeds.new" ]; then
    cat "$WORK/full.d/seed.out"
    grep -E 'ERR|WARN' "$WORK/full.d/seed.log" || true
    echo "FAIL: the free run of the whole table wrote no seeds (status $rrc, nothing written)"
    exit 1
  fi
  # The union of the seeds so far, under one header.
  if [ -n "$seeds" ]; then
    { head -n 1 "$WORK/seeds.new"; { tail -n +2 "$seeds"; tail -n +2 "$WORK/seeds.new"; } | LC_ALL=C sort -u; } >"$WORK/seeds.all"
  else
    cp "$WORK/seeds.new" "$WORK/seeds.all"
  fi
  seeds=$WORK/seeds
  if [ -f "$seeds" ] && cmp -s "$seeds" "$WORK/seeds.all"; then
    break
  fi
  mv -f "$WORK/seeds.all" "$seeds"
  round=$(( round + 1 ))
  if [ "$round" -gt "$Z80C_ROUNDS" ]; then
    # Translated once more with every seed found, so that the whole
    # table judged below and the table chosen from it start from the
    # same seeds.
    echo "z80c: seeding still adds positions after $Z80C_ROUNDS rounds: translated as it stands"
    "$WORK/translate" "$ROM" "$WORK/full.c" --seeds "$seeds"
    z80c_build "$WORK/full.c" "$WORK/full" "$WORK/obj"
    break
  fi
  echo "== translating again with the seeds of the run (round $round) =="
  "$WORK/translate" "$ROM" "$WORK/full.c" --seeds "$seeds"
done
echo "z80c: seeded in $round rounds"

echo "== the whole table: held to the interpreter led by the same clock =="
if z80c_play "$WORK/full" "$ROM" "$WORK/full.d" "$WORK/full.d/reference.fnv" "$PICREF" "$WORK/obj" "$WORK/counts"; then prc=0; else prc=$?; fi
if [ "$prc" -eq 4 ]; then
  echo "z80c: the program is refused (nothing written)"
  exit 4
fi
if [ "$prc" -ne 0 ]; then
  echo "FAIL: the whole table does not agree with the interpreter led by the same clock (nothing written)"
  exit 1
fi
echo "z80c: whole table proved translated=${t_tr}s picture=${t_pic}s"

# The table chosen under the budget, from the counts, and judged on its
# own: what it leaves out is interpreted, and a successor that pointed at
# a block left out is null. The waits are always in.
echo "== choosing under Z80C_BUDGET=$Z80C_BUDGET bytes =="
"$WORK/translate" "$ROM" "$WORK/rom_code.c" --seeds "$seeds" --counts "$WORK/counts" --budget "$Z80C_BUDGET"
z80c_build "$WORK/rom_code.c" "$WORK/chosen" "$WORK/obj"

EVREF=${Z80C_EVREF:-$WORK/chosen.d/reference.fnv}
echo "== the chosen table: held to the interpreter led by the same clock =="
mkdir -p "$WORK/chosen.d"
rm -f "$EVREF" "$EVREF.colours" "$EVREF.memory"
if z80c_play "$WORK/chosen" "$ROM" "$WORK/chosen.d" "$EVREF" "$PICREF" "$WORK/obj"; then prc=0; else prc=$?; fi
if [ "$prc" -eq 4 ]; then
  echo "z80c: the program is refused (nothing written)"
  exit 4
fi
if [ "$prc" -ne 0 ]; then
  echo "FAIL: the chosen table does not agree with the interpreter led by the same clock (nothing written)"
  exit 1
fi
echo "z80c: chosen table proved translated=${t_tr}s picture=${t_pic}s"

# The file compiled as the console chain will compile it: strict C89,
# optimised so that the code size read off it is an estimate and not a
# debug listing, every warning on and fatal, the SDK stubs standing in
# for the console's headers.
$CC -O2 -std=c89 -Wall -Wextra -Werror -I"$H" -I"$S" \
    -c "$WORK/rom_code.c" -o "$WORK/rom_code.o"

TEXT=$(size "$WORK/rom_code.o" | awk 'NR == 2 { print $1 }')

# The generated file keeps no account of time: no T-state spent, no
# refresh register, no quota. Held here, on the text, before anything is
# written -- a line that names one is a translator that counts again.
if grep -E 'Z80_SPEND|Z80_R|TSTATES' "$WORK/rom_code.c"; then
  echo "FAIL: the chosen table's C keeps an account of time (the lines above, nothing written)"
  exit 1
fi

mv -f "$WORK/rom_code.c" "$OUT"

echo "z80c: est_text=${TEXT:-?}"
echo "written: $OUT"
