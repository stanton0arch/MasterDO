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
# of the interpreter's clock (Z80_SPEND, Z80_R, TSTATES); compiles it
# with the console's compiler and holds the boot binary it would make to
# its ceiling; and only then writes src/rom_code.c -- a generated file
# the repository ignores. A red judgement writes nothing: whatever
# src/rom_code.c held stays as it was. A program the runners refuse --
# one that never waits, one that executes code from RAM -- writes
# nothing either, and the script exits 4 with the runner's line. A table
# that would take the boot binary over its ceiling writes nothing, and
# the script exits 5 with both figures, the ceiling and the budget that
# would have held -- except under the default budget, which is lowered
# once to that budget, the table chosen, judged and weighed again, and
# only a second table over the ceiling exits 5. A budget the caller
# names is never lowered.
#
# Prints the translator's reports, the runners' lines for both judgements
# -- the verdict, and beside it, for information, how the pictures stand
# against the classic interpreter -- and, before "written:", the line
#   z80c: arm_bytes=<n> est_launchme=<n> ceiling=155648 base=<n> ok
# the chosen table's weight as the console's compiler makes it, and the
# boot binary estimated from it.
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
#   Z80C_BASE=<bytes>                 the boot binary with the empty
#                                     table, what the estimate adds the
#                                     chosen table to (below)
#   Z80C_MARGIN=<bytes>               the room kept under the ceiling by
#                                     the budget said to hold (below)
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

# The budget, in bytes of Z80 code the chosen blocks may cover, each
# byte of the image counted once. The default comes from the measure
# taken on the reference ROM with the console's compiler on 2026-09-19
# (takeme/roms/rom.sms, 3000 frames), the object's areas as decaof
# lists them with the words the link relocates, regions capped at 96
# memory accesses (translate.c, MAX_REGION_ACCESSES), against the boot
# binary with the empty table (Z80C_BASE, 91552):
#   budget 1500: 44100 +  93 words = 44472   (estimate 136024)
#          1650: 53032 +  99 words = 53428   (estimate 144980)
#          1675: 54928 +  95 words = 55308   (estimate 146860)
#          1680: 55360 +  97 words = 55748   (estimate 147300, 8348 under)
#          1685: 55712 + 101 words = 56116   (estimate 147668, 7980 under)
#          1690: 55672 +  98 words = 56064   (estimate 147616, 8032 under)
#          1710: 56532 +  99 words = 56928   (estimate 148480)
# about 33 bytes of ARM per byte of Z80 covered. The default is the
# largest of them that leaves at least 8 kilobytes under the ceiling
# (below): 1680 bytes, 57 blocks, 75.3% of the instructions the run
# recorded -- the greedy choice is not monotonic, 1685 bytes take 59
# blocks and 75.8%. The ratio is the chosen blocks' and not the whole
# program's: every block that ran, 12327 bytes, weighed 315732 bytes of
# areas before the cap on accesses (25.6 a byte), and the reachable
# code, 18682 bytes, would weigh some 480 kilobytes. Another ROM has its
# own ratio: the ceiling is held on every table below whatever the
# budget.
#
# Whether the caller named the budget: a named budget is held as it is,
# the default is lowered once when its table is over the ceiling (below).
Z80C_BUDGET_NAMED=${Z80C_BUDGET:+1}
Z80C_BUDGET=${Z80C_BUDGET:-1680}
# Zero is refused as a word is: it writes an empty table, and the proof
# then fails with "no block ran", which accuses the proof instead of the
# setting.
case "$Z80C_BUDGET" in ''|*[!0-9]*|0) echo "Z80C_BUDGET must be a positive integer, not '$Z80C_BUDGET'"; exit 2;; esac

# The boot binary's ceiling, a measured rule and not a setting: LaunchMe
# at most 155648 bytes (152 kilobytes), which leaves at least 32768
# bytes of DRAM free at boot (src/sys.c warns below them). The estimate
# is the boot binary with the empty table (Z80C_BASE: takeme/LaunchMe
# as `make` with no arguments builds it -- the development build, the
# one the console runs -- src/rom_code.c the empty table: 91552 bytes on
# 2026-09-19; a reader who grows the rest of the program sets it again
# the same way)
# plus the chosen table's object as the console's compiler makes it.
# Z80C_MARGIN is the room the budget said to have held keeps under the
# ceiling when a table is over: 8192 bytes; tests/z80c/run_z80c.sh sets
# it lower on its written cartridge, whose whole table weighs less than
# that, to walk the lowering of the budget, and nothing else should.
Z80C_CEILING=155648
Z80C_MARGIN=${Z80C_MARGIN:-8192}
case "$Z80C_MARGIN" in ''|*[!0-9]*) echo "Z80C_MARGIN must be a non-negative integer, not '$Z80C_MARGIN'"; exit 2;; esac
Z80C_BASE=${Z80C_BASE:-91552}
case "$Z80C_BASE" in ''|*[!0-9]*) echo "Z80C_BASE must be a non-negative integer, not '$Z80C_BASE'"; exit 2;; esac

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

# The console's compiler, found the way the Makefile finds the devkit:
# TDO_DEVKIT_PATH, else the .devkit-path file, else this directory. The
# weighing below needs it (bin/compiler/linux/armcc and decaof): without
# it nothing is written.
if [ -n "${TDO_DEVKIT_PATH:-}" ]; then
  DEVKIT=${TDO_DEVKIT_PATH%/}
elif [ -f .devkit-path ]; then
  DEVKIT=$(cat .devkit-path)
  DEVKIT=${DEVKIT%/}
else
  DEVKIT=$PWD
fi
ARMCC=$DEVKIT/bin/compiler/linux/armcc
DECAOF=$DEVKIT/bin/compiler/linux/decaof
if [ ! -x "$ARMCC" ] || [ ! -x "$DECAOF" ]; then
  echo "FAIL: the ceiling check needs the console's compiler: no armcc and decaof under $DEVKIT/bin/compiler/linux (the devkit found as the Makefile finds it: TDO_DEVKIT_PATH, else .devkit-path, else this directory; nothing written)"
  exit 1
fi
A_INCFLAGS="-I$DEVKIT/include/3do -I$DEVKIT/include/community -I$DEVKIT/include/ttl"
A_DEFFLAGS="-DNDEBUG=1"
A_CFLAGS='-O2 -zpno_check_stack -bigend -za1 -zi4 -fa -fh -fx -fpu none -arch 3 -apcs 3/32/nofp/swst/wide/softfp'

# The choice, its judgement and its weighing, once -- or twice when the
# default budget's table is over the ceiling (below).
retried=0
while :; do
  # The table chosen under the budget, from the counts, and judged on its
  # own: what it leaves out is interpreted, and a successor that pointed at
  # a block left out is null. The waits are always in.
  echo "== choosing under Z80C_BUDGET=$Z80C_BUDGET bytes =="
  "$WORK/translate" "$ROM" "$WORK/rom_code.c" --seeds "$seeds" --counts "$WORK/counts" --budget "$Z80C_BUDGET" >"$WORK/choose.out"
  cat "$WORK/choose.out"
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

  # The file compiled on the host as strictly as the console chain would
  # refuse it: strict C89, optimised, every warning on and fatal, the SDK
  # stubs standing in for the console's headers. Its size is not read: the
  # console's compiler weighs the table below.
  $CC -O2 -std=c89 -Wall -Wextra -Werror -I"$H" -I"$S" \
      -c "$WORK/rom_code.c" -o "$WORK/rom_code.o"

  # The generated file keeps no account of time: no T-state spent, no
  # refresh register, no quota. Held here, on the text, before anything is
  # written -- a line that names one is a translator that counts again.
  if grep -E 'Z80_SPEND|Z80_R|TSTATES' "$WORK/rom_code.c"; then
    echo "FAIL: the chosen table's C keeps an account of time (the lines above, nothing written)"
    exit 1
  fi

  # The weight of the chosen table in the boot binary, measured with the
  # console's own compiler and held to the ceiling before anything is
  # written. The options are the release build's, copied from the Makefile
  # as tests/z80c/arm_count.sh copies them (INCFLAGS, DEFFLAGS, CFLAGS),
  # with src/ for the headers; the object's size is the sum of its areas
  # as decaof lists them, the zero-initialised ones left out (they take
  # no byte of the file), and its relocated words (below). The estimate is
  # the boot binary with the empty table (Z80C_BASE) plus that sum -- the
  # empty table's own 28 bytes are counted twice, on the safe side.
  echo "== compiling the chosen table with the console's compiler =="
  T0=$(date +%s)
  # shellcheck disable=SC2086
  if ! "$ARMCC" $A_INCFLAGS -I"$S" $A_DEFFLAGS $A_CFLAGS -c "$WORK/rom_code.c" -o "$WORK/rom_code.aof" >"$WORK/armcc.log" 2>&1; then
    grep -v 'Warning' "$WORK/armcc.log" | tail -n 20
    echo "FAIL: the console's compiler refuses the chosen table (nothing written)"
    exit 1
  fi
  T1=$(date +%s)
  # The two listings go to files and their status is read: a decaof
  # that fails, or prints a format this reading does not know, must not
  # pass for a small table.
  if ! "$DECAOF" -b "$WORK/rom_code.aof" >"$WORK/areas.txt" 2>&1 \
     || ! grep -q '^\*\* Area [0-9]* .*, Size [0-9]* ' "$WORK/areas.txt"; then
    head -n 5 "$WORK/areas.txt"
    echo "FAIL: decaof -b did not list the console object's areas (nothing written)"
    exit 1
  fi
  if ! "$DECAOF" -r "$WORK/rom_code.aof" >"$WORK/relocs.txt" 2>&1; then
    head -n 5 "$WORK/relocs.txt"
    echo "FAIL: decaof -r did not list the console object's relocations (nothing written)"
    exit 1
  fi
  ARM_BYTES=$(awk '
    /^\*\* Area / { if (have && !zero) sum += size
                     size = $0; sub(/^.*, Size /, "", size); sub(/ .*$/, "", size)
                     have = 1; zero = 0; next }
    have && /Attributes:/ && /Zero/ { zero = 1 }
    END { if (have && !zero) sum += size; print sum + 0 }' "$WORK/areas.txt")
  case "$ARM_BYTES" in ''|*[!0-9]*) echo "FAIL: the size of the console object was not read (nothing written)"; exit 1;; esac
  if [ "$ARM_BYTES" -eq 0 ]; then
    echo "FAIL: the console object has no area (nothing written)"
    exit 1
  fi
  # Every word of the object that holds an absolute address -- the
  # table's pointers to the regions, the literals naming the core's
  # globals -- is one more word of the boot binary's relocation list (the
  # link is relocatable, -reloc): counted with the areas. Measured on
  # 2026-09-19 against `make` on the reference ROM, this leaves the
  # estimate 28 bytes over the real size, the empty table's own.
  # A table of converted code always holds absolute words (its entries
  # point at its functions): an object with areas and no relocation
  # line of the form read here is a listing this reading does not know.
  RELOC_WORDS=$(grep -c ': Word ' "$WORK/relocs.txt" || true)
  case "$RELOC_WORDS" in ''|*[!0-9]*) RELOC_WORDS=0;; esac
  if [ "$RELOC_WORDS" -eq 0 ]; then
    head -n 5 "$WORK/relocs.txt"
    echo "FAIL: decaof -r listed no relocated word in an object of $ARM_BYTES bytes: format not recognised (nothing written)"
    exit 1
  fi
  AREA_BYTES=$ARM_BYTES
  ARM_BYTES=$(( AREA_BYTES + 4 * RELOC_WORDS ))
  EST=$(( Z80C_BASE + ARM_BYTES ))
  echo "z80c: armcc ${AREA_BYTES} bytes of areas and ${RELOC_WORDS} words to relocate in $(( T1 - T0 )) s"
  if [ "$EST" -gt "$Z80C_CEILING" ]; then
    echo "z80c: arm_bytes=$ARM_BYTES est_launchme=$EST ceiling=$Z80C_CEILING base=$Z80C_BASE over"
    # The budget that would have held at the ratio just measured: the room
    # under the ceiling, less the margin, in Z80 bytes at this table's
    # ARM bytes per Z80 byte covered.
    COVERED=$(sed -n 's/^z80c: selected blocks=[0-9]*\/[0-9]* bytes=\([0-9]*\) .*$/\1/p' "$WORK/choose.out" | tail -n 1)
    ROOM=$(( Z80C_CEILING - Z80C_MARGIN - Z80C_BASE ))
    # The bytes the waits alone cover: no budget goes under them.
    WAITS=$(sed -n 's/^z80c: selected blocks=.* waits=\([0-9]*\) .*$/\1/p' "$WORK/choose.out" | tail -n 1)
    FITS=
    if [ -z "$COVERED" ] || [ -z "$WAITS" ]; then
      echo "z80c: $ARM_BYTES bytes of ARM; the bytes of Z80 covered were not read"
    elif [ "$ROOM" -le 0 ]; then
      echo "z80c: $ARM_BYTES bytes of ARM for $COVERED bytes of Z80 covered; no budget holds: the boot binary without the table ($Z80C_BASE) leaves less than the margin of $Z80C_MARGIN bytes"
    else
      FITS=$(( ROOM * COVERED / ARM_BYTES ))
      if [ "$FITS" -le "$WAITS" ]; then
        echo "z80c: $ARM_BYTES bytes of ARM for $COVERED bytes of Z80 covered; no budget holds: $FITS bytes at that ratio, not above the $WAITS the waits alone cover"
        FITS=
      else
        echo "z80c: $ARM_BYTES bytes of ARM for $COVERED bytes of Z80 covered; Z80C_BUDGET=$FITS would have held at that ratio with $Z80C_MARGIN bytes of margin"
      fi
    fi
    # The default budget is lowered once, to the budget just said to
    # hold, and the table chosen, judged and weighed again; a named
    # budget, a second time over, or no budget that holds: nothing
    # written, status 5.
    if [ -z "$Z80C_BUDGET_NAMED" ] && [ "$retried" -eq 0 ] && [ -n "$FITS" ] && [ "$FITS" -gt 0 ] && [ "$FITS" -lt "$Z80C_BUDGET" ]; then
      echo "z80c: over the ceiling at the default budget: Z80C_BUDGET $Z80C_BUDGET -> $FITS, chosen, judged and weighed again"
      Z80C_BUDGET=$FITS
      retried=1
      continue
    fi
    echo "FAIL: the boot binary would weigh $EST bytes, over the ceiling of $Z80C_CEILING (budget $Z80C_BUDGET, nothing written)"
    exit 5
  fi
  echo "z80c: arm_bytes=$ARM_BYTES est_launchme=$EST ceiling=$Z80C_CEILING base=$Z80C_BASE ok"
  break
done

mv -f "$WORK/rom_code.c" "$OUT"

echo "written: $OUT"
