#!/bin/sh
#
# The translated code held against the interpreter by its pictures, on
# the PC: every ROM of takeme/roms/ translated into a work directory and
# judged by the translator's own two judgements (tests/z80c/translate.sh)
# -- the whole table, then the table chosen under the budget -- each of
# them the ROM run free twice, translated code armed then the interpreter
# alone, and every frame of the first held to the frame of the same rank
# of the second, or to the one before or after it; a frame that is none of
# the three by its palette numbers is redrawn in colour on both sides and
# the two screens compared byte for byte (tests/z80c/play.sh,
# tests/z80c/sidebyside.c). The same screen is counted apart, colour_only;
# another screen names the frame and its rows. Per ROM, two verdict lines,
# or a FAIL and nothing written.
#
# What this does not prove, and sidebyside.c says why: the internal
# memory of the program, the code that never runs without a pad, and that
# a translation without an account of time will draw the same pictures.
#
# A green run in which no translated block ran is not green: the runners
# refuse it before comparing anything -- the interpreter judged against
# itself proves nothing.
#
# The repository carries no ROM, but it does carry one cartridge it
# writes itself (tests/z80c/rom_bank.c): a handful of instructions that
# move the very window they run in, which is the one case a real ROM
# never produces and the only thing the epoch of the mapper guards
# against, its result shown as the backdrop. That image is generated into
# the work directory and judged first, on every checkout, so the target is
# never green on nothing. Without a real ROM beside it the script says so
# unmistakably and judges the written cartridge alone. With several ROMs
# every one is translated into its own directory and judged; the exit
# status is 1 if any fails.
#
# Nothing derived from a ROM lands in the tree: the C the translator
# writes goes to the work directory, never to src/rom_code.c, and the
# work directory is removed at the end, a red run's with it -- only the
# lines it printed remain, unless Z80C_KEEP names where the two screens of
# a mismatch are copied.
#
#   sh tests/z80c/run_z80c.sh               every ROM, 3000 frames each
#   FRAMES=600 sh tests/z80c/run_z80c.sh    fewer frames
#   ROMS=some/dir sh tests/z80c/run_z80c.sh the ROMs of another directory
#   Z80C_KEEP=some/dir sh tests/z80c/run_z80c.sh
#                                           the screens of a mismatch kept
#   Z80C_REDRAW_MAX=<n> sh tests/z80c/run_z80c.sh
#                                           frames of the same screen under
#                                           other palette numbers allowed
#                                           per judgement (64)
#   MUTATE=1 sh tests/z80c/run_z80c.sh      after the green: the chosen
#                                           table's C broken four ways a
#                                           wrong translation would be,
#                                           one visible byte of the video
#                                           memory flipped, and every
#                                           emitted write to the colour
#                                           memory altered, each demanded
#                                           red on at least one ROM; the
#                                           C broken two ways that
#                                           change the time alone, each
#                                           verdict printed as it is; and,
#                                           on the written cartridge, the
#                                           epoch of the mapper taken out
#                                           of the core and demanded red
#
set -e

case "${ROMS:-}" in ''|/*) ;; *) ROMS="$PWD/$ROMS";; esac
case "${Z80C_KEEP:-}" in ''|/*) ;; *) Z80C_KEEP="$PWD/$Z80C_KEEP";; esac

cd "$(dirname "$0")/../.."

ROMS=${ROMS:-takeme/roms}
FRAMES=${FRAMES:-3000}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
# A frame is redrawn by a run with one picture every 100000 frames
# (tests/z80c/play.sh): a longer run would take a second picture inside it.
if [ "$FRAMES" -gt 100000 ]; then
  echo "FRAMES must be at most 100000, not $FRAMES"
  exit 2
fi
# The flip of the video memory lands on frame FRAMES/3, which must be a
# frame of the run with a frame before it.
if [ "${MUTATE:-0}" = 1 ] && [ "$FRAMES" -lt 3 ]; then
  echo "MUTATE=1 needs FRAMES of at least 3, not $FRAMES"
  exit 2
fi
CC=${CC:-gcc}
S=src
H=tests/cel8/3do
B=tests/z80c
export FRAMES CC Z80C_KEEP Z80C_REDRAW_MAX

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

# One broken copy played through the whole judgement. Prints nothing;
# leaves the status in mrc (0 green, 1 red, 2 or 3 nothing proved, 8 does
# not compile, 9 the pattern matches nothing) and the lines in
# <dir>/<name>/verdict. A pattern that no longer matches is refused rather
# than played intact; a copy that does not compile is reported, not died
# on.
#
#   broken <name> <sed> <file to break> <rom_code.c> <rom> <dir> [core]
broken() {
  mrc=
  mkdir -p "$6/$1"
  if [ -n "${7:-}" ]; then
    copy=$6/$1/$(basename "$3")
  else
    copy=$6/$1/rom_code.c
  fi
  sed "$2" "$3" > "$copy"
  if cmp -s "$3" "$copy"; then
    mrc=9
    return 0
  fi
  set +e
  if [ -n "${7:-}" ]; then
    z80c_build "$4" "$6/$1/bin" "$WORK/obj" "$copy" >"$6/$1/build.log" 2>&1
  else
    z80c_build "$copy" "$6/$1/bin" "$WORK/obj" >"$6/$1/build.log" 2>&1
  fi
  brc=$?
  set -e
  if [ "$brc" -ne 0 ]; then
    mrc=8
    return 0
  fi
  set +e
  z80c_play "$6/$1/bin" "$5" "$6/$1" "$6/picture.fnv" "$WORK/obj" >"$6/$1/verdict" 2>&1
  mrc=$?
  set -e
  return 0
}

# The first line that says why a judgement went red.
red_line() {
  grep -m1 'MISMATCH' "$1" || grep -m1 '^FAIL' "$1" || true
}

# A mutation of a core file lays its code on one line of that file: an
# anchor that matches no line, or several, is refused rather than broken
# in more places than meant.
#
#   one_site <name> <regex> <file>
one_site() {
  os_n=$(grep -c "$2" "$3" || true)
  if [ "$os_n" -ne 1 ]; then
    echo "  [FAIL] mutation $1 refused: its anchor matches $os_n lines of $3, not one"
    return 1
  fi
  return 0
}

# The semantic mutations seen red, by name, over every ROM: each must be
# in this list at the end, or the check has not been seen to bite on it.
RED_SEEN=" "
PLAYED=" "

# A mutation of the semantics, on one ROM: red is what it is for; green on
# this ROM is printed and counted against the ROMs still to come; a copy
# that does not compile or that proves nothing is a failure of THIS
# script; a pattern with no form to break in this ROM's table is said.
#
#   mutate <name> <sed> <why> <file to break> <rom_code.c> <rom> <dir> [core]
mutate() {
  m_name=$1
  m_why=$3
  m_dir=$7
  broken "$1" "$2" "$4" "$5" "$6" "$7" ${8:+"$8"}
  case "$mrc" in
    9) echo "  [INFO] mutation $m_name ($m_why) has no form to break here: not played"
       return 0;;
    8) echo "  [FAIL] mutation $m_name ($m_why) does not compile"
       cat "$m_dir/$m_name/build.log"
       return 1;;
  esac
  case "$mrc" in
    0|1) PLAYED="$PLAYED$m_name ";;
  esac
  case "$mrc" in
    0) echo "  [INFO] mutation $m_name ($m_why) stays green on this rom: $(grep -m1 'pictures PASS' "$m_dir/$m_name/verdict")"
       return 0;;
    1) echo "  [OK] mutation $m_name turns the check red: $(red_line "$m_dir/$m_name/verdict")"
       RED_SEEN="$RED_SEEN$m_name "
       return 0;;
    *) echo "  [FAIL] mutation $m_name ($m_why) proved nothing (status $mrc)"
       cat "$m_dir/$m_name/verdict"
       return 1;;
  esac
}

# A mutation of the time alone: played, and its verdict printed as it
# is. Once the account of time is gone, nothing the program shows depends
# on it; while it stands, a shift of the interrupts may or may not move a
# picture by more than a frame.
#
#   timing <name> <sed> <why> <rom_code.c> <rom> <dir>
timing() {
  broken "$1" "$2" "$4" "$4" "$5" "$6"
  case "$mrc" in
    9) echo "  [FAIL] timing mutation $1 changed nothing, its pattern no longer matches"
       return 1;;
    8) echo "  [FAIL] timing mutation $1 ($3) does not compile"
       cat "$6/$1/build.log"
       return 1;;
    0) echo "  [INFO] timing mutation $1: green ($3): $(grep -m1 'pictures PASS' "$6/$1/verdict")";;
    1) echo "  [INFO] timing mutation $1: red ($3): $(red_line "$6/$1/verdict")";;
    *) echo "  [INFO] timing mutation $1: proved nothing ($3, status $mrc): $(red_line "$6/$1/verdict")";;
  esac
  return 0
}

if [ "${MUTATE:-0}" = 1 ]; then
  z80c_runner "$WORK/obj"
fi

fail=0
for rom in "$@"; do
  [ -f "$rom" ] || continue
  # rom.sms and rom.gg each get their own directory and their own name.
  name=$(basename "$rom" | sed 's/\./_/g')
  dir=$WORK/$name
  mkdir -p "$dir"
  echo "== $name: translating, judging the whole table, choosing, judging the chosen table =="
  start=$(date +%s)
  # The translator's file goes to the work directory and nowhere else:
  # src/rom_code.c stays whatever the tree holds. The interpreter's
  # picture is kept there for the mutations.
  set +e
  OUT="$dir/rom_code.c" Z80C_PICREF="$dir/picture.fnv" sh "$B/translate.sh" "$rom" >"$dir/translate.out" 2>&1
  trc=$?
  set -e
  cat "$dir/translate.out"
  echo "  ($(( $(date +%s) - start )) s)"
  if [ "$trc" -ne 0 ] \
     || [ "$(grep -c "^z80c: pictures PASS $FRAMES/$FRAMES same=[0-9]* shifted=[0-9]* colour_only=[0-9]*\$" "$dir/translate.out")" -ne 2 ] \
     || ! grep -q "^written: " "$dir/translate.out"; then
    echo "FAIL: $name: the two judgements did not both pass on the pictures, or nothing was written"
    fail=1
    continue
  fi

  [ "${MUTATE:-0}" = 1 ] || continue

  # The written cartridge answers for the core's own guard and for
  # nothing else: its handful of instructions match none of the patterns
  # below, which are forms only a real program carries, and its video
  # memory holds nothing to flip.
  if [ "$rom" = "$FIXTURE" ]; then
    echo "== $name: the epoch of the mapper taken out of the core =="
    # Without it the chain trusts the successor a block rendered while
    # the bank behind that address was being turned: the block of the
    # bank that has just left runs in place of the one now there, and the
    # backdrop turns white.
    mutate epoch 's/^\( *\)z80c_map_epoch++;$/\1;/' \
         "the mapper no longer steps the epoch" "$S/cart.c" \
         "$dir/rom_code.c" "$rom" "$dir" core || fail=1
    continue
  fi

  echo "== $name: the chosen table's C broken, four ways that change what the program does =="
  # Every compare of an immediate leaves the carry flag inverted: the
  # next conditional branch goes the other way.
  mutate cp 's/^\(  Z80_OP_CP(0x[0-9A-F]*U);\)\( \/\* cp n \*\/\)$/\1 Z80_F ^= 0x01U;\2/' \
         "the carry flag inverted after every cp n" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every relative jump lands one byte past its target.
  mutate jr 's/^\(  *Z80_PC = (uint16)(z80_pc0 + 0x[0-9A-F]*U\)); \/\* jr /\1 + 1U); \/* jr /' \
         "every jr one byte past its target" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every block that loads B loads it from the field of C.
  mutate load 's/^\(  uint8 z80c_b\) = Z80_B_STATE;$/\1 = Z80_C_STATE;/' \
         "register b loaded from the field of c" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every rendered successor is the entry after the right one.
  mutate succ 's/&z80c_table\[\([0-9]*\)\]/\&z80c_table[(\1 + 1UL) % z80c_block_count]/g' \
         "every successor shifted by one entry" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  echo "== $name: one visible byte of the video memory flipped =="
  # On the translated side only, once, at the end of frame FLIP-1, FLIP
  # being a third of the run: bit 0 of the name table entry of row 12,
  # column 16 -- the middle of the screen -- flipped through the same notes
  # a write of the data port makes, so that the list redraws the tile.
  # The name table is where register 2 puts it at that moment. The tile
  # beside it in the pattern table shows from frame FLIP until the program
  # writes that entry again.
  flip=$(( FRAMES / 3 ))
  if one_site vram '^      sms.vdp.vcount = 0;$' "$S/vdp.c"; then
    mutate vram "s/^      sms.vdp.vcount = 0;\$/      { static unsigned long flips; if(++flips == ${flip}UL) { uint32 a = (((((uint32)sms.vdp.reg[2]) \& 0x0EUL) << 10) + 0x320UL) \& VDP_VRAM_MASK; uint8 v = (uint8)(sms.vdp.vram[a] ^ 1U); VDP_DECOR_NOTE(a,v); sms.vdp.vram[a] = v; sms.vdp.tc_valid[VDP_TC_KEY(a)] = 0; } }\n&/" \
           "bit 0 of the name table entry in the middle of the screen flipped on frame $(( flip - 1 ))" "$S/vdp.c" \
           "$dir/rom_code.c" "$rom" "$dir" core || fail=1
  else
    fail=1
  fi
  echo "== $name: every emitted write to the colour memory altered =="
  # The port writes the emitted C makes go through z80_io_write
  # (src/z80.c); a write to the data port of the video part (an even port
  # from $80 to $BF) while the video part is set to write the colour
  # memory stores the value with bit 0 flipped: red one step off, every
  # colour the translated code writes, and nothing else the program can
  # read back.
  if one_site colour '^  CART_IO_WRITE(port,value);$' "$S/z80.c"; then
    mutate colour 's/^  CART_IO_WRITE(port,value);$/  CART_IO_WRITE(port,(uint8)((((port \& 0xC1U) == 0x80U) \&\& (sms.vdp.code == VDP_CODE_CRAM_WRITE)) ? (value ^ 0x01U) : value));/' \
           "every emitted colour write with bit 0 flipped" "$S/z80.c" \
           "$dir/rom_code.c" "$rom" "$dir" core || fail=1
  else
    fail=1
  fi
  echo "== $name: the chosen table's C broken, two ways that change the time alone =="
  # Every exit of every block charges one T-state too few.
  timing spend 's/^\(  *Z80_SPEND(\)\([0-9]*\)\();\)$/\1\2 - 1\3/' \
         "every block one T-state cheaper" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every taken branch forgets its surcharge.
  timing cc 's/^\(      Z80_SPEND([0-9]*\) + [0-9]*);$/\1);/' \
         "the surcharge of every taken branch dropped" "$dir/rom_code.c" "$rom" "$dir" || fail=1
done

# The semantic mutations, over every ROM played: each seen red at least
# once, or the check has not been seen to bite on it. Without a real ROM,
# only the epoch was played.
if [ "${MUTATE:-0}" = 1 ]; then
  echo "== the mutations over every rom =="
  if [ "$found" -eq 1 ]; then
    wanted="epoch cp jr load succ vram colour"
  else
    wanted="epoch"
  fi
  for m in $wanted; do
    case "$RED_SEEN" in
      *" $m "*) echo "  [OK] mutation $m seen red";;
      *) case "$PLAYED" in
           *" $m "*) echo "  [FAIL] mutation $m left the check green on every rom it was played on";;
           *) echo "  [FAIL] mutation $m was played on no rom";;
         esac
         fail=1;;
    esac
  done
fi

echo "failed=$fail"
exit $fail
