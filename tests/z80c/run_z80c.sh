#!/bin/sh
#
# The translated code held against the interpreter, side by side, on the
# PC: every ROM of takeme/roms/ translated into a work directory, played
# twice on one binary -- translated code armed, then the interpreter
# alone on the same per-line quotas -- and the two runs compared line by
# line on the registers and frame by frame on the memory, the video part
# and the counters (tests/z80c/sidebyside.c says how). The first
# difference names the frame, the line, the block that started at the
# line's entry PC when one does ("none" otherwise), how many blocks the
# line ran, and both states.
#
# A green run in which no translated block ran is not green: the runner
# says so in its own line and this script refuses it -- the interpreter
# judged against itself proves nothing.
#
# The repository carries no ROM: without one this script says "skipped"
# unmistakably and exits 0, so that the target is runnable and green on
# every checkout. With several ROMs every one is translated into its own
# directory and judged; the exit status is 1 if any fails.
#
# Nothing derived from a ROM lands in the tree: the C the translator
# writes goes to the work directory, never to src/rom_code.c, and the
# work directory is removed at the end. The trace of a ROM weighs about
# 10.5 kilobytes per frame (31.5 megabytes at 3000 frames); it is removed
# as soon as its replay has passed and kept, in the work directory, while
# the script runs, when it has not.
#
#   sh tests/z80c/run_z80c.sh               every ROM, 3000 frames each
#   FRAMES=600 sh tests/z80c/run_z80c.sh    fewer frames
#   MUTATE=1 sh tests/z80c/run_z80c.sh      after the green: the emitted C
#                                           broken three ways, each seen
#                                           red, then the frame digest
#                                           seen red on its own
#
set -e

cd "$(dirname "$0")/../.."

ROMS=takeme/roms
FRAMES=${FRAMES:-3000}
EVERY=${EVERY:-300}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
case "$EVERY"  in ''|*[!0-9]*|0) echo "EVERY must be a positive integer, not '$EVERY'"; exit 2;; esac
CC=${CC:-gcc}
S=src
H=tests/cel8/3do
B=tests/z80c

# The ROMs, .sms and .gg, whatever their names: none means nothing to
# compare, and that is said rather than counted as a pass.
set -- "$ROMS"/*.sms "$ROMS"/*.gg
found=0
for r in "$@"; do
  [ -f "$r" ] && found=1
done
if [ "$found" -eq 0 ]; then
  echo "z80c: skipped: no rom in $ROMS (nothing compared, nothing proved)"
  exit 0
fi

# No substitute may shadow a real header of src/: the -I of the stubs comes
# first, so a file named like one of the core's own would replace it in
# silence (tests/z80/run_z80.sh, step 1).
for h in "$H"/*.h; do
  if [ -e "$S/$(basename "$h")" ]; then
    echo "FAIL: $h shadows $S/$(basename "$h")"
    exit 1
  fi
done

# The same pins as the picture check: the host has no assembler for the
# recompiled cores, the interrupt test source is off, and the counters
# the video part and the translated code keep are on -- the runner
# records them.
PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The runner is the only host file here, and this is the only place a
# second compiler reads it: its warnings are kept, the core's are not
# (they are the SDK headers' and are counted elsewhere). The dialect is
# gnu89 and not overridable: the runner formats its states with snprintf,
# which C89 does not declare (tests/z80/run_z80.sh says the same).
echo "== building the runner =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -I"$H" -I"$S" -c -o "$WORK/sidebyside.o" "$B/sidebyside.c"

# The core linked as it stands with a named table of translated code: the
# one the translator wrote for the ROM, or one of the broken copies below.
#
#   build <rom_code.c> <binary>
build() {
  $CC -O1 -std=gnu89 -w $PINS -I"$H" -I"$S" -o "$2" "$WORK/sidebyside.o" \
      "$S/cart.c" "$S/sms.c" "$S/vdp.c" "$S/z80.c" "$S/z80c.c" "$1"
}

# One binary played twice: recorded with the translated code armed, then
# replayed by the interpreter on the recorded quotas. Prints the runner's
# lines; returns its status -- 0 the same, 1 a difference, 2 nothing
# proved, 3 the table is not the ROM's. The replay mode is the caller's:
# "replay" judges, "replay-poke" is the self-check of the frame path.
#
#   play <binary> <rom> <dir> <replay mode>
play() {
  rc=0; t_rec=; t_rep=; start=
  set +e
  start=$(date +%s)
  "$1" "$2" "$FRAMES" "$EVERY" record "$3/trace.bin" >"$3/record.out" 2>"$3/record.log"
  rc=$?
  t_rec=$(( $(date +%s) - start ))
  cat "$3/record.out"
  if [ "$rc" -ne 0 ]; then
    set -e
    grep -E 'ERR|WARN' "$3/record.log" || true
    return "$rc"
  fi
  start=$(date +%s)
  "$1" "$2" "$FRAMES" "$EVERY" "$4" "$3/trace.bin" >"$3/replay.out" 2>"$3/replay.log"
  rc=$?
  t_rep=$(( $(date +%s) - start ))
  set -e
  cat "$3/replay.out"
  [ "$rc" -eq 0 ] || grep -E 'ERR|WARN' "$3/replay.log" || true
  return "$rc"
}

# The check seen to bite: copies of the emitted C, each broken one way a
# wrong translation would be, each played through the same two runs,
# each expected red. Global -- every occurrence of the form -- because a
# block nobody runs proves nothing, and the first executed block of the
# form is what bites. A copy that stays green is a failure of THIS script;
# a pattern that no longer matches is refused rather than played intact;
# a copy that does not compile is reported, not died on. Each is the
# ROM's verdict: the next ROM is still judged.
#
#   mutate <name> <sed> <why> <rom_code.c> <rom> <dir>
mutate() {
  mkdir -p "$6/$1"
  sed "$2" "$4" > "$6/$1/rom_code.c"
  if cmp -s "$4" "$6/$1/rom_code.c"; then
    echo "FAIL: mutation $1 changed nothing, its pattern no longer matches"
    return 1
  fi
  if ! build "$6/$1/rom_code.c" "$6/$1/sidebyside" 2>"$6/$1/build.log"; then
    echo "  [FAIL] mutation $1 ($3) does not compile"
    cat "$6/$1/build.log"
    return 1
  fi
  set +e
  play "$6/$1/sidebyside" "$5" "$6/$1" replay >"$6/$1/verdict" 2>&1
  mrc=$?
  set -e
  if [ "$mrc" -eq 0 ]; then
    echo "  [FAIL] mutation $1 ($3) left the check green"
    cat "$6/$1/verdict"
    return 1
  fi
  if [ "$mrc" -ne 1 ]; then
    echo "  [FAIL] mutation $1 ($3) proved nothing (status $mrc)"
    cat "$6/$1/verdict"
    return 1
  fi
  echo "  [OK] mutation $1 ($3) turns the check red: $(grep -m1 'MISMATCH' "$6/$1/verdict")"
  return 0
}

# The frame path seen to bite on the intact binary: the emitted C of
# 12.1's instruction set stores nothing but the push of a call, so no
# mutation above reaches the digest; the runner flips one byte of the
# work RAM on frame 0 instead, and the digest must say so.
#
#   poke <binary> <rom> <dir>
poke() {
  mkdir -p "$3/poke"
  set +e
  play "$1" "$2" "$3/poke" replay-poke >"$3/poke/verdict" 2>&1
  prc=$?
  set -e
  if [ "$prc" -eq 0 ]; then
    echo "  [FAIL] one byte of the work RAM flipped left the check green"
    cat "$3/poke/verdict"
    return 1
  fi
  if [ "$prc" -ne 1 ]; then
    echo "  [FAIL] the flipped byte proved nothing (status $prc)"
    cat "$3/poke/verdict"
    return 1
  fi
  echo "  [OK] the frame digest turns the check red: $(grep -m1 'MISMATCH' "$3/poke/verdict")"
  return 0
}

fail=0
for rom in "$@"; do
  [ -f "$rom" ] || continue
  # rom.sms and rom.gg each get their own directory and their own name.
  name=$(basename "$rom" | sed 's/\./_/g')
  dir=$WORK/$name
  mkdir -p "$dir"
  echo "== $name: translating =="
  # The translator's file goes to the work directory and nowhere else:
  # src/rom_code.c stays whatever the tree holds.
  if ! OUT="$dir/rom_code.c" sh "$B/translate.sh" "$rom" >"$dir/translate.out" 2>&1; then
    cat "$dir/translate.out"
    echo "FAIL: $name: the translator refused the rom"
    fail=1
    continue
  fi
  report=$(grep -m1 '^z80c: rom ' "$dir/translate.out" | sed 's/ insns=.*//')
  echo "== $name: building =="
  build "$dir/rom_code.c" "$dir/sidebyside"
  echo "== $name: translated code armed, then the interpreter on its quotas =="
  if play "$dir/sidebyside" "$rom" "$dir" replay; then
    echo "$report frames=$FRAMES translated=${t_rec}s interp=${t_rep}s"
  else
    echo "$report frames=$FRAMES translated=${t_rec:-?}s interp=${t_rep:-?}s"
    echo "FAIL: $name: the two runs differ, or nothing was proved"
    fail=1
    continue
  fi
  # What the core said of the table at boot, if it said anything: a
  # "none, interpreter only" here is what the next check refuses.
  grep 'WARN.*translated code' "$dir/record.log" || true
  # The same guard as the picture check: a run the translated code took
  # no part in judged the interpreter against itself.
  if ! grep -q '^z80c: recorded [0-9]* frames exec=[1-9]' "$dir/record.out"; then
    echo "FAIL: $name: no translated block ran, the interpreter was judged against itself"
    fail=1
    continue
  fi
  rm -f "$dir/trace.bin"

  [ "${MUTATE:-0}" = 1 ] || continue

  echo "== $name: the emitted C broken, three ways =="
  # Every compare of an immediate leaves the carry flag inverted: the
  # next conditional branch of the interpreter goes the other way.
  mutate cp 's/^\(  Z80_OP_CP(0x[0-9A-F]*U);\)\( \/\* cp n \*\/\)$/\1 Z80_F ^= 0x01U;\2/' \
         "the carry flag inverted after every cp n" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every block charges one T-state too many: the interpreter given the
  # recorded quota stops one instruction short of the block's end.
  mutate spend 's/^  Z80_SPEND(\([0-9]*\));$/  Z80_SPEND(\1 + 1);/' \
         "every block one T-state dearer" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every relative jump lands one byte past its target.
  mutate jr 's/^\(  Z80_PC = (uint16)(z80_pc0 + 0x[0-9A-F]*U\)); \/\* jr /\1 + 1U); \/* jr /' \
         "every jr one byte past its target" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  echo "== $name: the frame digest, one byte of the work ram flipped =="
  poke "$dir/sidebyside" "$rom" "$dir" || fail=1
done

echo "failed=$fail"
exit $fail
