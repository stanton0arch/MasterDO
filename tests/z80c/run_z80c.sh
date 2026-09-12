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
# The proof is the translator's own (tests/z80c/translate.sh): the whole
# table first, then the table chosen under the budget from the counts of
# that first run -- two PASS lines per ROM, or a FAIL and nothing
# written. This script runs it with the output in the work directory,
# reads its verdict, and then, on request, breaks the chosen table.
#
# A green run in which no translated block ran is not green: the runner
# says so in its own line and the translator refuses it -- the
# interpreter judged against itself proves nothing.
#
# The repository carries no ROM, but it does carry one cartridge it
# writes itself (tests/z80c/rom_bank.c): a handful of instructions that
# move the very window they run in, which is the one case a real ROM
# never produces and the only thing the epoch of the mapper guards
# against. That image is generated into the work directory and judged
# first, on every checkout, so the target is never green on nothing.
# Without a real ROM beside it the script says so unmistakably and judges
# the written cartridge alone. With several ROMs every one is translated
# into its own directory and judged; the exit status is 1 if any fails.
#
# Nothing derived from a ROM lands in the tree: the C the translator
# writes goes to the work directory, never to src/rom_code.c, and the
# work directory is removed at the end. The trace of a ROM weighs about
# 10.5 kilobytes per frame (31.5 megabytes at 3000 frames); it is removed
# as soon as its replay has passed, and the work directory takes the rest
# with it on the way out -- a red proof leaves no trace behind to open,
# only the lines it printed. Replay the failing ROM by hand to get one.
#
#   sh tests/z80c/run_z80c.sh               every ROM, 3000 frames each
#   FRAMES=600 sh tests/z80c/run_z80c.sh    fewer frames
#   MUTATE=1 sh tests/z80c/run_z80c.sh      after the green: the chosen
#                                           table's C broken six ways,
#                                           each seen red, then the frame
#                                           digest seen red on its own;
#                                           and, on the written cartridge,
#                                           the epoch of the mapper taken
#                                           out of the core and seen red
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
export FRAMES EVERY CC

# The ROMs, .sms and .gg, whatever their names: none means nothing to
# compare, and that is said rather than counted as a pass.
set -- "$ROMS"/*.sms "$ROMS"/*.gg
found=0
for r in "$@"; do
  [ -f "$r" ] && found=1
done
if [ "$found" -eq 0 ]; then
  echo "z80c: no rom in $ROMS: the written cartridge is judged alone"
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

. "$B/play.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The cartridge this repository writes itself, generated into the work
# directory and judged before the real ROMs. It is built with the
# strictness of the tools, not of the console: it never leaves the PC.
FIXTURE=$WORK/bankswitch.sms
$CC -O1 -std=c89 -Wall -Wextra -Werror -o "$WORK/rom_bank" "$B/rom_bank.c"
"$WORK/rom_bank" "$FIXTURE"
set -- "$FIXTURE" "$@"

# The check seen to bite: copies of the chosen table's C, each broken one
# way a wrong translation would be, each played through the same two
# runs, each expected red. Global -- every occurrence of the form --
# because a block nobody runs proves nothing, and the first executed
# block of the form is what bites. A copy that stays green is a failure
# of THIS script; a pattern that no longer matches is refused rather than
# played intact; a copy that does not compile is reported, not died on.
# Each is the ROM's verdict: the next ROM is still judged.
#
#   mutate <name> <sed> <why> <rom_code.c> <rom> <dir>
mutate() {
  mkdir -p "$6/$1"
  sed "$2" "$4" > "$6/$1/rom_code.c"
  if cmp -s "$4" "$6/$1/rom_code.c"; then
    echo "FAIL: mutation $1 changed nothing, its pattern no longer matches"
    return 1
  fi
  if ! z80c_build "$6/$1/rom_code.c" "$6/$1/sidebyside" "$WORK/sidebyside.o" 2>"$6/$1/build.log"; then
    echo "  [FAIL] mutation $1 ($3) does not compile"
    cat "$6/$1/build.log"
    return 1
  fi
  set +e
  z80c_play "$6/$1/sidebyside" "$5" "$6/$1" replay >"$6/$1/verdict" 2>&1
  mrc=$?
  set -e
  rm -f "$6/$1/trace.bin"
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

# The frame path seen to bite on the intact binary, on its own terms:
# the runner flips one byte of the work RAM on frame 0, and the digest
# must say so.
#
#   poke <binary> <rom> <dir>
poke() {
  mkdir -p "$3/poke"
  set +e
  z80c_play "$1" "$2" "$3/poke" replay-poke >"$3/poke/verdict" 2>&1
  prc=$?
  set -e
  rm -f "$3/poke/trace.bin"
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

# The same, on a file of the core instead of the emitted C: the copy
# takes the place of the original in the link, the table stays intact.
# A guard of the core that no ROM in the work list makes bite is a guard
# nobody has seen work.
#
#   mutate_core <name> <sed> <why> <core file> <rom_code.c> <rom> <dir>
mutate_core() {
  mkdir -p "$7/$1"
  copy=$7/$1/$(basename "$4")
  sed "$2" "$4" > "$copy"
  if cmp -s "$4" "$copy"; then
    echo "FAIL: mutation $1 changed nothing, its pattern no longer matches"
    return 1
  fi
  if ! z80c_build "$5" "$7/$1/sidebyside" "$WORK/sidebyside.o" "$copy" 2>"$7/$1/build.log"; then
    echo "  [FAIL] mutation $1 ($3) does not compile"
    cat "$7/$1/build.log"
    return 1
  fi
  set +e
  z80c_play "$7/$1/sidebyside" "$6" "$7/$1" replay >"$7/$1/verdict" 2>&1
  mrc=$?
  set -e
  rm -f "$7/$1/trace.bin"
  if [ "$mrc" -eq 0 ]; then
    echo "  [FAIL] mutation $1 ($3) left the check green"
    cat "$7/$1/verdict"
    return 1
  fi
  if [ "$mrc" -ne 1 ]; then
    echo "  [FAIL] mutation $1 ($3) proved nothing (status $mrc)"
    cat "$7/$1/verdict"
    return 1
  fi
  echo "  [OK] mutation $1 ($3) turns the check red: $(grep -m1 'MISMATCH' "$7/$1/verdict")"
  return 0
}

fail=0
for rom in "$@"; do
  [ -f "$rom" ] || continue
  # rom.sms and rom.gg each get their own directory and their own name.
  name=$(basename "$rom" | sed 's/\./_/g')
  dir=$WORK/$name
  mkdir -p "$dir"
  echo "== $name: translating, proving the whole table, choosing, proving the chosen table =="
  # The translator's file goes to the work directory and nowhere else:
  # src/rom_code.c stays whatever the tree holds.
  set +e
  OUT="$dir/rom_code.c" sh "$B/translate.sh" "$rom" >"$dir/translate.out" 2>&1
  trc=$?
  set -e
  cat "$dir/translate.out"
  if [ "$trc" -ne 0 ] || [ "$(grep -c '^z80c: PASS [0-9]*/[0-9]* frames$' "$dir/translate.out")" -ne 2 ] \
     || ! grep -q "^written: " "$dir/translate.out"; then
    echo "FAIL: $name: the two proofs did not both pass, or nothing was written"
    fail=1
    continue
  fi

  [ "${MUTATE:-0}" = 1 ] || continue

  z80c_runner "$WORK/sidebyside.o"

  # The written cartridge answers for the core's own guard and for
  # nothing else: its handful of instructions match none of the six
  # patterns below, which are forms only a real program carries.
  if [ "$rom" = "$FIXTURE" ]; then
    echo "== $name: the epoch of the mapper taken out of the core =="
    # Without it the chain trusts the successor a block rendered while
    # the bank behind that address was being turned: the block of the
    # bank that has just left runs in place of the one now there.
    mutate_core epoch 's/^\( *\)z80c_map_epoch++;$/\1;/' \
         "the mapper no longer steps the epoch" "$S/cart.c" \
         "$dir/rom_code.c" "$rom" "$dir" || fail=1
  else
    echo "== $name: the chosen table's C broken, six ways =="
    # Every compare of an immediate leaves the carry flag inverted: the
    # next conditional branch of the interpreter goes the other way.
    mutate cp 's/^\(  Z80_OP_CP(0x[0-9A-F]*U);\)\( \/\* cp n \*\/\)$/\1 Z80_F ^= 0x01U;\2/' \
           "the carry flag inverted after every cp n" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    # Every exit of every block charges one T-state too few: the
    # interpreter given the recorded quota stops short of the block's end.
    mutate spend 's/^\(  *Z80_SPEND(\)\([0-9]*\)\();\)$/\1\2 - 1\3/' \
           "every block one T-state cheaper" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    # Every relative jump lands one byte past its target.
    mutate jr 's/^\(  *Z80_PC = (uint16)(z80_pc0 + 0x[0-9A-F]*U\)); \/\* jr /\1 + 1U); \/* jr /' \
           "every jr one byte past its target" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    # Every block that loads B loads it from the field of C.
    mutate load 's/^\(  uint8 z80c_b\) = Z80_B_STATE;$/\1 = Z80_C_STATE;/' \
           "register b loaded from the field of c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    # Every rendered successor is the entry after the right one.
    mutate succ 's/&z80c_table\[\([0-9]*\)\]/\&z80c_table[(\1 + 1UL) % z80c_block_count]/g' \
           "every successor shifted by one entry" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    # Every taken branch forgets its surcharge.
    mutate cc 's/^\(      Z80_SPEND([0-9]*\) + [0-9]*);$/\1);/' \
           "the surcharge of every taken branch dropped" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  fi
  echo "== $name: the frame digest, one byte of the work ram flipped =="
  z80c_build "$dir/rom_code.c" "$dir/sidebyside" "$WORK/sidebyside.o"
  poke "$dir/sidebyside" "$rom" "$dir" || fail=1
done

echo "failed=$fail"
exit $fail
