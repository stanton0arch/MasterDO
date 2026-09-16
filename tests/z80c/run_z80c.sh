#!/bin/sh
#
# The translated code held against the interpreter led by the same
# clock, on the PC: every ROM of takeme/roms/ translated into a work
# directory and judged by the translator's own two judgements
# (tests/z80c/translate.sh) -- the whole table, then the table chosen
# under the budget -- each of them the ROM played twice by the picture
# runner with that table linked, the blocks executed then not executed,
# and every frame of the first held to the frame of the same rank of the
# second: rows, colours line by line and the memory the program keeps,
# no shift, no tolerance (tests/z80c/play.sh, tests/z80c/sidebyside.c).
# Beside the verdict, for information, how the pictures stand against
# the classic interpreter. Per ROM, two verdict lines, or a FAIL and
# nothing written, or a REFUSED: a program that never waits, or one
# that executes code from RAM, which no table can hold. A refusal is
# counted and named, never failed on and never passed: the summary says
# how many ROMs were refused, and the human reads which.
#
# What this does not prove, and sidebyside.c says why: that the program
# behaves as the console did (the clock by events is not the part's),
# that the waits marked are the program's, and the code that never runs
# without a pad.
#
# A green run in which no translated block ran is not green: the runners
# refuse it before comparing anything -- the interpreter judged against
# itself proves nothing.
#
# The repository carries no ROM, but it does carry one cartridge it
# writes itself (tests/z80c/rom_bank.c), generated into the work
# directory and judged first, on every checkout, so the target is never
# green on nothing. It is the one program whose every wait and write is
# known, and it is held to more than a real ROM is:
#
#   - it moves the very window it runs in, mid-chain, which no real ROM
#     does and which only the epoch of the mapper guards against;
#   - it never halts: it waits by a loop on a byte of RAM, which the core
#     must end the line on -- a refusal ("no wait") is a FAILURE on this
#     image, never a refusal counted;
#   - its frames must also match the classic interpreter's frame for
#     frame (the line "classic same=FRAMES shifted=0 differ=0", demanded
#     twice, once per judgement): every write it makes to the video part
#     falls inside the vertical blank under either clock, so the two
#     clocks draw the same frames unless a wait is marked where the
#     program does not wait -- it carries a fill loop of 128 turns that
#     reads a fixed byte of RAM and writes memory, and marked as a wait
#     that loop would push the write of the backdrop into the picture;
#   - it keeps a byte at $C000 the picture never reads back, which the
#     mutation "memory" below moves: only the memory held beside the
#     picture can tell.
#
# Without a real ROM beside it the script says so unmistakably and judges
# the written cartridge alone. With several ROMs every one is translated
# into its own directory and judged; the exit status is 1 if any fails.
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
#   MUTATE=1 sh tests/z80c/run_z80c.sh      after the green: the chosen
#                                           table's C broken four ways a
#                                           wrong translation would be,
#                                           its waits marked two wrong
#                                           ways (one block in two a
#                                           wait, no block a wait), one visible
#                                           byte of the video memory
#                                           flipped, and every emitted
#                                           write to the colour memory
#                                           altered, each demanded red or
#                                           refused on at least one ROM;
#                                           and, on the written cartridge,
#                                           the epoch of the mapper taken
#                                           out of the core, the write of
#                                           its byte at $C000 moved to
#                                           $C003, and its waits marked
#                                           the same two wrong ways, each
#                                           demanded red or refused.
#                                           Every broken copy is held to
#                                           the reference of the intact
#                                           table.
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
export FRAMES CC Z80C_KEEP

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

# One broken copy played through the whole judgement, against the
# reference of the intact chosen table. Prints nothing; leaves the status
# in mrc (0 green, 1 red, 2 or 3 nothing proved, 4 refused, 8 does not
# compile, 9 the pattern matches nothing) and the lines in
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
  z80c_play "$6/$1/bin" "$5" "$6/$1" "$6/events.fnv" "$6/picture.fnv" "$WORK/obj" >"$6/$1/verdict" 2>&1
  mrc=$?
  set -e
  return 0
}

# The first line that says why a judgement went red, or was refused.
red_line() {
  grep -m1 'MISMATCH' "$1" || grep -m1 '^z80c: REFUSED' "$1" || grep -m1 '^FAIL' "$1" || true
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

# The mutations seen red or refused, by name, over every ROM: each must
# be in this list at the end, or the check has not been seen to bite on
# it.
RED_SEEN=" "
PLAYED=" "

# A mutation, on one ROM: red, or refused, is what it is for; green on
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
    0|1|4) PLAYED="$PLAYED$m_name ";;
  esac
  case "$mrc" in
    0) echo "  [INFO] mutation $m_name ($m_why) stays green on this rom: $(grep -m1 'events PASS' "$m_dir/$m_name/verdict")"
       return 0;;
    1) echo "  [OK] mutation $m_name turns the check red: $(red_line "$m_dir/$m_name/verdict")"
       RED_SEEN="$RED_SEEN$m_name "
       return 0;;
    4) echo "  [OK] mutation $m_name gets the program refused: $(red_line "$m_dir/$m_name/verdict")"
       RED_SEEN="$RED_SEEN$m_name "
       return 0;;
    *) echo "  [FAIL] mutation $m_name ($m_why) proved nothing (status $mrc)"
       cat "$m_dir/$m_name/verdict"
       return 1;;
  esac
}

if [ "${MUTATE:-0}" = 1 ]; then
  z80c_runner "$WORK/obj"
fi

fail=0
refused=0
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
  OUT="$dir/rom_code.c" Z80C_PICREF="$dir/picture.fnv" Z80C_EVREF="$dir/events.fnv" sh "$B/translate.sh" "$rom" >"$dir/translate.out" 2>&1
  trc=$?
  set -e
  cat "$dir/translate.out"
  echo "  ($(( $(date +%s) - start )) s)"
  if [ "$trc" -eq 4 ] && grep -q '^z80c: REFUSED ' "$dir/translate.out"; then
    if [ "$rom" = "$FIXTURE" ]; then
      # The written cartridge waits by a loop the core must honour: a
      # refusal of it is the core's failure, not the program's.
      echo "FAIL: $name: the written cartridge was refused: $(grep -m1 '^z80c: REFUSED ' "$dir/translate.out")"
      fail=1
      continue
    fi
    echo "z80c: $name REFUSED: $(grep -m1 '^z80c: REFUSED ' "$dir/translate.out")"
    refused=$(( refused + 1 ))
    continue
  fi
  if [ "$trc" -ne 0 ] \
     || [ "$(grep -c "^z80c: events PASS $FRAMES/$FRAMES memory=same insns/frame=[0-9]*\$" "$dir/translate.out")" -ne 2 ] \
     || ! grep -q "^written: " "$dir/translate.out"; then
    echo "FAIL: $name: the two judgements did not both pass, or nothing was written"
    fail=1
    continue
  fi
  # The written cartridge alone: the classic interpreter, which shares
  # nothing with the clock by events, must draw the very same frames,
  # frame for frame, under both judgements. On a real ROM the two clocks
  # legitimately drift apart and the line is information.
  if [ "$rom" = "$FIXTURE" ] \
     && [ "$(grep -c "^z80c: classic same=$FRAMES shifted=0 differ=0\$" "$dir/translate.out")" -ne 2 ]; then
    echo "FAIL: $name: the written cartridge does not draw the classic interpreter's frames frame for frame ($(grep -m1 '^z80c: classic ' "$dir/translate.out" || echo 'no classic line')): a wait is marked where the program does not wait, or one is missed"
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
    # backdrop shows the inverted pattern.
    mutate epoch 's/^\( *\)z80c_map_epoch++;$/\1;/' \
         "the mapper no longer steps the epoch" "$S/cart.c" \
         "$dir/rom_code.c" "$rom" "$dir" core || fail=1
    echo "== $name: the write of the byte at \$C000 moved to \$C003 =="
    # The picture never reads that byte back: the frames stand, and only
    # the memory held beside them at the end of every frame can tell.
    mutate memory 's/Z80_WR8(0xC000U,Z80_A)/Z80_WR8(0xC003U,Z80_A)/' \
         "the byte at \$C000 written at \$C003" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    echo "== $name: the written cartridge's waits marked two wrong ways =="
    # The same two breakings of the waits as on a real ROM (below): with
    # no block a wait the loop at wait never ends its line and the
    # program is refused; with one block in two a wait the fill is one,
    # and every turn of it ends a line.
    mutate nowait 's/, 1UL },$/, 0UL },/' \
         "no block marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    mutate wait 's/^\(  { 0x[0-9A-F]*[02468ACE]UL, b_[0-9a-f]*\), 0UL },$/\1, 1UL },/' \
         "one block in two marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
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
  echo "== $name: the chosen table's waits marked two wrong ways =="
  # No block a wait: the program spins in its loop and the line never
  # ends -- the guard refuses it. A table whose waits are all halts has
  # no mark to take out and says so.
  mutate nowait 's/, 1UL },$/, 0UL },/' \
         "no block marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # One block in two a wait (the ones whose position is even): a line
  # ends where the program does not wait, it falls behind its clock, and
  # the frames no longer match the reference taken with the intact
  # table's waits. Not every block: a table of nothing but waits never
  # chains two blocks, and the free run refuses it before anything is
  # judged.
  mutate wait 's/^\(  { 0x[0-9A-F]*[02468ACE]UL, b_[0-9a-f]*\), 0UL },$/\1, 1UL },/' \
         "one block in two marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
done

# The mutations, over every ROM played: each seen red or refused at
# least once, or the check has not been seen to bite on it. Without a
# real ROM, only the four of the written cartridge were played.
if [ "${MUTATE:-0}" = 1 ]; then
  echo "== the mutations over every rom =="
  if [ "$found" -eq 1 ]; then
    wanted="epoch memory cp jr load succ vram colour wait nowait"
  else
    wanted="epoch memory wait nowait"
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

echo "failed=$fail refused=$refused"
exit $fail
