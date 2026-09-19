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
#     picture can tell;
#   - it walks the direct path to the work RAM on every family the
#     translator proves (the stack, an absolute read of the ROM, a pair
#     the tool knows, the seam of the mirror, the write to the mapper
#     that closes its block), so that the mutations "direct", "stack"
#     and "bankend" below have a form to break without a real ROM;
#   - it runs a loop at two levels over three blocks of one region, so
#     that the translator's line of regions is held to exact figures,
#     its C to carrying gotos, the runs to a frontier above zero, and
#     the mutations "edge" and "regexit" below have a form to break;
#   - it carries, where no run goes, code dense enough in memory
#     accesses that the translator must keep it apart and cut it, and
#     the mutations "accjoin" and "acccut" below break that;
#   - it is translated again as a table over the boot binary's ceiling:
#     under a named budget the translator must refuse it with status 5
#     and nothing written (z80c_over_ceiling below); under the default
#     budget it must lower the budget once and write the second table,
#     and under the same budget named, refuse it (z80c_lowered);
#   - its table is paired with it by the console's own z80c_init
#     (z80c_pairing): the image arms it, a copy with one byte changed
#     or of another size does not, and the mutation "pairing" breaks
#     that.
#
# Every table written, of every ROM, must carry the console compiler's
# weighing, "z80c: arm_bytes= est_launchme= ceiling=155648 base= ok"
# (CEILING below), before "written:"; its lines of regions must stay
# under the three sizes that compiler lives by, and its choice must
# charge no more bytes than the budget. The weighing needs the console's
# compiler, armcc and decaof under bin/compiler/linux of the devkit,
# found as the Makefile finds it (TDO_DEVKIT_PATH, else .devkit-path,
# else this directory): without it every translation fails.
#
# Two VARIANTS of it are each held to one refusal of the runners, and to
# nothing else; they are judged after the cartridge and played under no
# mutation. bankswitch_indirect.sms writes the mapper through a pointer
# loaded in another block, which the translator cannot prove: the block
# goes on after the write on the bytes of the bank that left, and the
# runners must refuse it ("bank switch inside block").
# bankswitch_underflow.sms sets its stack at $C001, in the work RAM, so
# that the stack is proved and its first push lands under the RAM: the
# runners must refuse it ("stack outside ram"), the check the console
# does not make.
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
#                                           out of the core, the
#                                           core's push swapped in its
#                                           header, the write of
#                                           its byte at $C000 moved to
#                                           $C003, and its waits marked
#                                           the same two wrong ways, each
#                                           demanded red or refused; and,
#                                           on the cartridge and the ROMs
#                                           alike, the direct path broken
#                                           three ways: every absolute
#                                           read of the ROM turned into
#                                           the RAM form, the two bytes
#                                           of the direct pop swapped in
#                                           the core's header, and the
#                                           flag of the blocks closed on
#                                           a mapper write cleared --
#                                           the written cartridge carries
#                                           all three forms, a real ROM
#                                           may carry none; and the
#                                           regions broken three ways:
#                                           every goto sent to the head
#                                           of its region, every case of
#                                           a dispatch sent to the head,
#                                           and the accumulator no
#                                           longer stored at the exits.
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
# The boot binary's ceiling, as tests/z80c/translate.sh holds it.
CEILING=155648
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
INDIRECT=$WORK/bankswitch_indirect.sms
UNDERFLOW=$WORK/bankswitch_underflow.sms
$CC -O1 -std=c89 -Wall -Wextra -Werror -o "$WORK/rom_bank" "$B/rom_bank.c"
"$WORK/rom_bank" "$FIXTURE"
"$WORK/rom_bank" "$INDIRECT" indirect
"$WORK/rom_bank" "$UNDERFLOW" underflow
set -- "$FIXTURE" "$INDIRECT" "$UNDERFLOW" "$@"

# The written cartridge's direct path and regions, held to figures: the
# translator's report of what it proved on the seeded table -- the whole
# one and the chosen one; the whole one also proves the dense code at
# the non-maskable vector, which no run enters and the chosen table
# leaves out -- its line of regions on each, and, in the C it
# wrote, at least one of every form the image was written to exercise,
# direct and full alike, and as many gotos as the line says edges and as
# many functions as it says regions. A proof gone silent draws the very
# same frames (the full path and the direct one agree to the byte), and
# so does a region cut back into blocks, so only the figures can tell;
# the runner's own count of bytes moved directly must be above zero on
# both judgements, and its counts of registers loaded and stored at the
# frontiers and of edges taken inside the regions are held to the EXACT
# figure of the 3000 frames -- the image is deterministic, and an edge
# that leaves by its exit instead of its goto (a window test written
# the wrong way, a guard that fires) draws the same frames and is seen
# by that count alone. The three sizes the console's compiler lives by
# are held under their caps (translate.c, MAX_INSNS, MAX_REGION_INSNS
# and MAX_REGION_ACCESSES). Counter and floor, the way a speed bench is
# held. The chosen table's densest function makes 34 accesses; the whole
# table's makes exactly the cap, 96: the run of ex (sp),hl at the
# non-maskable vector, cut after its 24th (tests/z80c/rom_bank.c,
# lay_nmi). That code, which no run enters, is also why the whole
# table has more blocks, more regions and more accesses proved than the
# chosen one: 60 ldi on the full path, 26 exchanges and a call, a retn
# and a ret on the proved stack.
#
# The direct figures, chosen table: 22 of 26 reads and 30 of 37 writes
# proved; whole table: 24 of 28 and 57 of 124 (the code above).
FIXTURE_DIRECT_FULL='z80c: direct rd=24/28 wr=57/124 stack=proven rom_fixed=0'
FIXTURE_DIRECT='z80c: direct rd=22/26 wr=30/37 stack=proven rom_fixed=0'
FIXTURE_REGIONS_FULL='z80c: regions=17 entries=23 edges=12 exits=26 loads=53 stores=119 longest_block=32 longest_region=55 longest_accesses=96'
FIXTURE_REGIONS_CHOSEN='z80c: regions=12 entries=18 edges=12 exits=21 loads=29 stores=98 longest_block=32 longest_region=55 longest_accesses=34'
FIXTURE_GOTOS=12
FIXTURE_FUNCTIONS=12
FIXTURE_FRONTIER=1690554
FIXTURE_EDGES=348132
MAX_INSNS=32
MAX_REGION_INSNS=128
MAX_REGION_ACCESSES=96

# The three sizes the console's compiler lives by, held on every line of
# regions a translation printed -- every ROM's, not only the written
# cartridge's.
#
#   z80c_caps <file> <name>
z80c_caps() {
  zc_n=$(grep -c '^z80c: regions=' "$1" || true)
  zc_ok=$(grep -c '^z80c: regions=.* longest_block=[0-9]* longest_region=[0-9]* longest_accesses=[0-9]*$' "$1" || true)
  if [ "${zc_n:-0}" -eq 0 ] || [ "$zc_n" != "$zc_ok" ]; then
    echo "FAIL: $2: the translator's lines of regions are missing or unread ($zc_ok of $zc_n)"
    return 1
  fi
  zc_lb=$(sed -n 's/^z80c: regions=.* longest_block=\([0-9]*\) longest_region=\([0-9]*\) longest_accesses=\([0-9]*\)$/\1/p' "$1" | sort -n | tail -n 1)
  zc_lr=$(sed -n 's/^z80c: regions=.* longest_block=\([0-9]*\) longest_region=\([0-9]*\) longest_accesses=\([0-9]*\)$/\2/p' "$1" | sort -n | tail -n 1)
  zc_la=$(sed -n 's/^z80c: regions=.* longest_block=\([0-9]*\) longest_region=\([0-9]*\) longest_accesses=\([0-9]*\)$/\3/p' "$1" | sort -n | tail -n 1)
  if [ "$zc_lb" -gt "$MAX_INSNS" ] || [ "$zc_lr" -gt "$MAX_REGION_INSNS" ] || [ "$zc_la" -gt "$MAX_REGION_ACCESSES" ]; then
    echo "FAIL: $2: the longest block ($zc_lb), region ($zc_lr) or accesses ($zc_la) is over the sizes the console's compiler lives by ($MAX_INSNS, $MAX_REGION_INSNS, $MAX_REGION_ACCESSES)"
    return 1
  fi
  return 0
}

z80c_fixture_floors() {
  ff_rc=0
  if ! grep -q "^$FIXTURE_DIRECT_FULL\$" "$1/translate.out" || ! grep -q "^$FIXTURE_DIRECT\$" "$1/translate.out"; then
    echo "FAIL: the written cartridge's proof does not report '$FIXTURE_DIRECT_FULL' then '$FIXTURE_DIRECT': $(grep '^z80c: direct rd=' "$1/translate.out" | tr '\n' ' ')"
    ff_rc=1
  fi
  if ! grep -q "^$FIXTURE_REGIONS_FULL\$" "$1/translate.out" || ! grep -q "^$FIXTURE_REGIONS_CHOSEN\$" "$1/translate.out"; then
    echo "FAIL: the written cartridge's regions are not '$FIXTURE_REGIONS_FULL' then '$FIXTURE_REGIONS_CHOSEN': $(grep '^z80c: regions=' "$1/translate.out" | tr '\n' ' ')"
    ff_rc=1
  fi
  if [ "$(grep -c 'goto L_' "$1/rom_code.c")" -ne "$FIXTURE_GOTOS" ] || [ "$(grep -c '^r_' "$1/rom_code.c")" -ne "$FIXTURE_FUNCTIONS" ]; then
    echo "FAIL: the written cartridge's C does not carry the $FIXTURE_GOTOS gotos and $FIXTURE_FUNCTIONS regions its line says ($(grep -c 'goto L_' "$1/rom_code.c") gotos, $(grep -c '^r_' "$1/rom_code.c") regions)"
    ff_rc=1
  fi
  if [ "$(grep -c '^z80c: direct=[1-9][0-9]* full=[1-9][0-9]*$' "$1/translate.out")" -ne 2 ]; then
    echo "FAIL: the written cartridge's runs do not both count bytes on the direct path and the full one: $(grep '^z80c: direct=' "$1/translate.out" | tr '\n' ' ')"
    ff_rc=1
  fi
  if [ "$(grep -c "^z80c: frontier=$FIXTURE_FRONTIER\$" "$1/translate.out")" -ne 2 ]; then
    echo "FAIL: the written cartridge's runs do not both move $FIXTURE_FRONTIER registers at the frontiers: $(grep '^z80c: frontier=' "$1/translate.out" | tr '\n' ' ')"
    ff_rc=1
  fi
  if [ "$(grep -c "^z80c: edges=$FIXTURE_EDGES\$" "$1/translate.out")" -ne 2 ]; then
    echo "FAIL: the written cartridge's runs do not both take $FIXTURE_EDGES edges inside the regions: $(grep '^z80c: edges=' "$1/translate.out" | tr '\n' ' ')"
    ff_rc=1
  fi
  for ff_form in \
    'Z80C_RAM_RD8(0x' 'Z80C_RAM_WR8(0x' 'Z80C_RAM_WR8(Z80_HL,Z80_D)' 'Z80C_RAM_WR8(ixaddr,' \
    'Z80C_RAM_RD16(0x' 'Z80C_RAM_WR16(0x' \
    'Z80C_STK_PUSH(' 'Z80C_STK_POP(' 'Z80C_STK_EXSP(' 'Z80C_STK_RET()' 'Z80C_STK_RETI()' \
    'Z80_WR8(Z80_HL,Z80_D)' 'Z80_WR8(0xFFFFU,Z80_A)' 'Z80C_RD16(0xDFFFU)' 'Z80C_RD8(0x8'; do
    if ! grep -qF "$ff_form" "$1/rom_code.c"; then
      echo "FAIL: the written cartridge's C carries no '$ff_form'"
      ff_rc=1
    fi
  done
  return $ff_rc
}

# A variant held to one refusal: the translator's status is 4 and its
# output carries the line; anything else -- a pass, another refusal, a
# failure -- is the core's failure.
#
#   expected_refusal <name> <status> <output> <refusal line prefix> <why>
expected_refusal() {
  if [ "$2" -eq 4 ] && grep -q "^z80c: REFUSED $4 at " "$3"; then
    echo "z80c: $1 refused as expected: $(grep -m1 '^z80c: REFUSED ' "$3")"
    return 0
  fi
  echo "FAIL: $1: $5 was not refused as expected (status $2: $(grep -m1 '^z80c: REFUSED \|^FAIL' "$3" || echo 'no refusal'))"
  return 1
}

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
  # The runner's own check that chains are followed stops a broken copy
  # before any judgement (status 2): on a mutated table that is the
  # check biting, not a failure of this script, so it counts as refused.
  # Any other status 2 still proves nothing.
  if [ "$mrc" = 2 ] &&
     grep -q '^FAIL: no chain ran a second region' "$m_dir/$m_name/verdict"; then
    mrc=4
  fi
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

# The three breakings of the direct path, played on the written
# cartridge and on every ROM alike, each demanded red or refused:
#
#   direct   every absolute read of the ROM (the reads the tool did NOT
#            prove, since the proved ones already carry the RAM form)
#            turned into the RAM form: the byte comes out of the work
#            RAM at the masked offset instead of the image;
#   stack    the two bytes of the direct pop swapped, in the core's
#            header: a value popped comes back with its halves crossed,
#            and so does every return address -- proved in the core,
#            not in the emitted C, since the emitted C only names the
#            form;
#   bankend  the flag of every block closed on a mapper write cleared:
#            the core sees the epoch move under a block not marked for
#            it and refuses the program.
#
#   z80c_direct_mutations <dir> <rom>
#
# The last two break a form the table may not carry -- a program whose
# stack is not proved names no Z80C_STK_ form, one that never writes the
# mapper at a proved address closes no block -- and the sed of "stack"
# always changes the header it edits: the form is looked for in the
# table first, and a table without it is said not played, the way
# "broken" says it of a pattern that matches nothing.
z80c_direct_mutations() {
  z80c_dm_rc=0
  mutate direct 's/Z80C_RD8(0x\([0-9A-F]*\)U)/Z80C_RAM_RD8(0x\1U)/' \
         "every absolute read of the rom taken from the ram" "$1/rom_code.c" "$1/rom_code.c" "$2" "$1" || z80c_dm_rc=1
  if grep -q 'Z80C_STK_' "$1/rom_code.c"; then
    mutate stack 's/(lo) = (ram)\[Z80_SP/(XX) = (ram)[Z80_SP/; s/(hi) = (ram)\[Z80_SP/(lo) = (ram)[Z80_SP/; s/(XX) = (ram)\[Z80_SP/(hi) = (ram)[Z80_SP/' \
           "the two bytes of the direct pop swapped" "$S/z80_ops.h" "$1/rom_code.c" "$2" "$1" core || z80c_dm_rc=1
  else
    echo "  [INFO] mutation stack (the two bytes of the direct pop swapped) has no form to break here: not played"
  fi
  if grep -q ',[23]UL) },$' "$1/rom_code.c"; then
    mutate bankend 's/,2UL) },$/,0UL) },/; s/,3UL) },$/,1UL) },/' \
           "the flag of the blocks closed on a mapper write cleared" "$1/rom_code.c" "$1/rom_code.c" "$2" "$1" || z80c_dm_rc=1
  else
    echo "  [INFO] mutation bankend (the flag of the blocks closed on a mapper write cleared) has no form to break here: not played"
  fi
  return $z80c_dm_rc
}

# The three breakings of the regions, played on the written cartridge
# and on every ROM alike, each demanded red or refused:
#
#   edge      every goto inside a region sent to the region's head: a
#             loop turns on the wrong block, and either the program
#             never waits on the line (the guard refuses it) or what it
#             writes differs;
#   dispatch  every case of a region's dispatch sent to its head: a
#             region entered at a block that is not its first -- the
#             return of a call landing on it -- runs from its first
#             instead (on the written cartridge the return of sub lands
#             on skip, and from the head the call is made again, for
#             ever);
#   regexit   the accumulator no longer stored at the exits of the
#             regions: what a region computed in A is lost when it
#             leaves.
#
#   z80c_region_mutations <dir> <rom>
#
# A table with no goto -- no two of its blocks joined -- has no edge to
# send, and one with no region of two blocks no case to send, and each
# is said not played, the way "broken" says it of a pattern that
# matches nothing.
z80c_region_mutations() {
  z80c_rm_rc=0
  mutate edge 's/^\( *\)goto L_[0-9a-f]*;/\1goto head;/' \
         "every goto sent to the head of its region" "$1/rom_code.c" "$1/rom_code.c" "$2" "$1" || z80c_rm_rc=1
  mutate dispatch 's/^\( *case [0-9]*UL: \)goto E_[0-9a-f]*;/\1goto head;/' \
         "every case of the dispatch sent to the head of its region" "$1/rom_code.c" "$1/rom_code.c" "$2" "$1" || z80c_rm_rc=1
  mutate regexit 's/^\( *\)Z80_A_STATE = z80c_a;$/\1;/' \
         "the accumulator no longer stored at the exits" "$1/rom_code.c" "$1/rom_code.c" "$2" "$1" || z80c_rm_rc=1
  return $z80c_rm_rc
}

# A table over the boot binary's ceiling, on the written cartridge: every
# block taken (a budget far over its code) and the boot binary said to
# weigh the ceiling already without it (Z80C_BASE=$CEILING), so that any
# object is over -- the cartridge's few blocks (some 5 kilobytes of ARM)
# never are on the true base, and no base leaves them over while room
# stays under the margin, so the report here is the one that says no
# budget holds. The translator must exit 5, name both figures and the
# ceiling, leave the file it would have written as it was, and -- the
# budget being named -- never lower it and try again.
z80c_over_ceiling() {
  z80c_oc_out=$2/over.c
  echo "/* left as it was */" > "$z80c_oc_out"
  cp "$z80c_oc_out" "$2/over.before"
  set +e
  OUT="$z80c_oc_out" Z80C_BUDGET=100000 Z80C_BASE=$CEILING Z80C_PICREF="$2/picture.fnv" \
    sh "$B/translate.sh" "$1" >"$2/over.out" 2>&1
  z80c_oc_rc=$?
  set -e
  if [ "$z80c_oc_rc" -ne 5 ] \
     || ! grep -q "^z80c: arm_bytes=[0-9]* est_launchme=[0-9]* ceiling=$CEILING base=$CEILING over\$" "$2/over.out" \
     || ! grep -q '^z80c: [0-9]* bytes of ARM for [0-9]* bytes of Z80 covered; no budget holds' "$2/over.out" \
     || grep -q '^written: ' "$2/over.out" \
     || grep -q '^z80c: over the ceiling at the default budget' "$2/over.out" \
     || ! cmp -s "$z80c_oc_out" "$2/over.before"; then
    echo "FAIL: $name: a table over the ceiling was not refused with status 5 and nothing written (status $z80c_oc_rc: $(grep -m1 '^z80c: arm_bytes=\|^FAIL' "$2/over.out" || echo 'no line'))"
    return 1
  fi
  echo "z80c: $name over the ceiling refused: $(grep -m1 '^FAIL' "$2/over.out")"
  return 0
}

# The lowering of the default budget, on the written cartridge, and its
# twin under a named budget. The boot binary is said to weigh, without
# the table, 50 bytes less than what the ceiling leaves the cartridge's
# table (its arm_bytes, read off its own translation): the default
# table is over by 50. With a margin of 300 bytes the room is its weight
# less 350, and the budget said to hold, at the table's ratio, some 20
# bytes under what it covers -- a table that still chains its regions
# and, measured on 2026-09-19, weighs 216 bytes less (5136 against
# 5352), under the ceiling. The default budget must be lowered once, to
# a smaller budget, the second table weighed ok and written; a named
# budget on the same base must exit 5 with the budget that would have
# held and no second choice.
#
#   z80c_lowered <rom> <dir>
z80c_lowered() {
  zl_arm=$(sed -n 's/^z80c: arm_bytes=\([0-9]*\) est_launchme=.* ok$/\1/p' "$2/translate.out" | tail -n 1)
  if [ -z "$zl_arm" ]; then
    echo "FAIL: $name: no weight read off the cartridge's translation"
    return 1
  fi
  zl_base=$(( CEILING - zl_arm + 50 ))
  set +e
  OUT="$2/lowered.c" Z80C_BASE=$zl_base Z80C_MARGIN=300 Z80C_PICREF="$2/picture.fnv" \
    sh "$B/translate.sh" "$1" >"$2/lowered.out" 2>&1
  zl_rc=$?
  set -e
  zl_from=$(sed -n 's/^z80c: over the ceiling at the default budget: Z80C_BUDGET \([0-9]*\) -> \([0-9]*\), .*$/\1/p' "$2/lowered.out")
  zl_to=$(sed -n 's/^z80c: over the ceiling at the default budget: Z80C_BUDGET \([0-9]*\) -> \([0-9]*\), .*$/\2/p' "$2/lowered.out")
  if [ "$zl_rc" -ne 0 ] || [ -z "$zl_from" ] || [ -z "$zl_to" ] || [ "$zl_to" -ge "$zl_from" ] \
     || ! sed -n "/^z80c: arm_bytes=[0-9]* est_launchme=[0-9]* ceiling=$CEILING base=$zl_base ok\$/,\$p" "$2/lowered.out" | grep -q '^written: '; then
    echo "FAIL: $name: the default budget over the ceiling was not lowered once and written (status $zl_rc, from ${zl_from:-?} to ${zl_to:-?}): $(grep -m1 '^FAIL' "$2/lowered.out" || true)"
    return 1
  fi
  echo "z80c: $name default budget lowered on base $zl_base: $zl_from -> $zl_to, $(grep '^z80c: arm_bytes=' "$2/lowered.out" | tail -n 1 | sed 's/^z80c: //')"
  echo "/* left as it was */" > "$2/named.c"
  cp "$2/named.c" "$2/named.before"
  set +e
  OUT="$2/named.c" Z80C_BUDGET="$zl_from" Z80C_BASE=$zl_base Z80C_MARGIN=300 Z80C_PICREF="$2/picture.fnv" \
    sh "$B/translate.sh" "$1" >"$2/named.out" 2>&1
  zl_rc=$?
  set -e
  if [ "$zl_rc" -ne 5 ] \
     || ! grep -q "^z80c: [0-9]* bytes of ARM for [0-9]* bytes of Z80 covered; Z80C_BUDGET=$zl_to would have held" "$2/named.out" \
     || grep -q '^z80c: over the ceiling at the default budget' "$2/named.out" \
     || [ "$(grep -c '^== choosing under ' "$2/named.out")" -ne 1 ] \
     || grep -q '^written: ' "$2/named.out" \
     || ! cmp -s "$2/named.c" "$2/named.before"; then
    echo "FAIL: $name: the named budget $zl_from on base $zl_base was not refused with status 5, Z80C_BUDGET=$zl_to named and no second choice (status $zl_rc)"
    return 1
  fi
  echo "z80c: $name named budget $zl_from on base $zl_base refused: $(grep -m1 '^z80c: [0-9]* bytes of ARM' "$2/named.out" | sed 's/^z80c: //')"
  return 0
}

# The console's pairing of the table with the image, played by the
# runner in its pairing mode (tests/z80c/sidebyside.c): the real
# z80c_init, none of the runner's own refusal. On the written cartridge:
# the image itself arms the table ("paired"); a copy with one byte
# changed, of the same size, does not ("rom mismatch", both digests);
# the image twice over, of another size, does not either ("rom
# mismatch", the digest never taken: dashes). With a fourth argument, a
# z80c.c to take the place of the core's: how the mutation "pairing"
# is played.
#
#   z80c_pairing <rom> <dir> [z80c.c]
z80c_pairing() {
  zp_d=$2/pairing${3:+.mutated}
  mkdir -p "$zp_d"
  [ -f "$WORK/obj/sidebyside.o" ] || z80c_runner "$WORK/obj" || return 2
  z80c_build "$2/rom_code.c" "$zp_d/run" "$WORK/obj" ${3:+"$3"} >"$zp_d/build.log" 2>&1 || { cat "$zp_d/build.log"; return 2; }
  cp "$1" "$zp_d/same.sms"
  # One byte of the first bank's filler, well past the code, turned.
  printf '\377' | dd of="$zp_d/same.sms" bs=1 seek=12288 conv=notrunc 2>/dev/null
  cat "$1" "$1" > "$zp_d/other.sms"
  zp_rc=0
  "$zp_d/run" "$1" pairing >"$zp_d/own.out" 2>"$zp_d/own.log" || zp_rc=1
  if [ "$zp_rc" -ne 0 ] || ! grep -q '^z80c: pairing armed=1$' "$zp_d/own.out" \
     || ! grep -q 'converted code: blocks=.* paired$' "$zp_d/own.log"; then
    echo "  the image itself: $(cat "$zp_d/own.out" "$zp_d/own.log" | grep -m2 'pairing\|converted')"
    zp_rc=1
  fi
  "$zp_d/run" "$zp_d/same.sms" pairing >"$zp_d/same.out" 2>"$zp_d/same.log" || zp_rc=1
  if ! grep -q '^z80c: pairing armed=0$' "$zp_d/same.out" \
     || ! grep -q '\[ERR\] converted code: rom mismatch ([0-9]*/[0-9a-f]\{8\} vs [0-9]*/[0-9a-f]\{8\}), interpreter only$' "$zp_d/same.log"; then
    echo "  one byte changed: $(cat "$zp_d/same.out" "$zp_d/same.log" | grep -m2 'pairing\|converted')"
    zp_rc=1
  fi
  "$zp_d/run" "$zp_d/other.sms" pairing >"$zp_d/other.out" 2>"$zp_d/other.log" || zp_rc=1
  if ! grep -q '^z80c: pairing armed=0$' "$zp_d/other.out" \
     || ! grep -q '\[ERR\] converted code: rom mismatch ([0-9]*/[0-9a-f]\{8\} vs [0-9]*/--------), interpreter only$' "$zp_d/other.log"; then
    echo "  another size: $(cat "$zp_d/other.out" "$zp_d/other.log" | grep -m2 'pairing\|converted')"
    zp_rc=1
  fi
  return $zp_rc
}

# The translator broken, compiled on the side and run alone on the
# written cartridge (its whole table, no run): the line of regions must
# go over the cap on accesses. accjoin: regions joined whatever their
# accesses; acccut: no block cut on its accesses.
#
#   z80c_translator_mutation <name> <sed> <rom> <dir>
z80c_translator_mutation() {
  zt_d=$4/$1
  mkdir -p "$zt_d"
  sed "$2" "$B/translate.c" > "$zt_d/translate.c"
  if cmp -s "$B/translate.c" "$zt_d/translate.c"; then
    echo "  [FAIL] mutation $1 matches nothing in translate.c"
    return 1
  fi
  PLAYED="$PLAYED$1 "
  $CC -O1 -std=gnu89 -w -o "$zt_d/translate" "$zt_d/translate.c" || { echo "  [FAIL] mutation $1 does not compile"; return 1; }
  "$zt_d/translate" "$3" "$zt_d/out.c" >"$zt_d/out.txt" 2>&1 || true
  if z80c_caps "$zt_d/out.txt" "$1" >"$zt_d/caps.txt"; then
    echo "  [FAIL] mutation $1 left every function under the caps: $(grep '^z80c: regions=' "$zt_d/out.txt")"
    return 1
  fi
  echo "  [OK] mutation $1 turns the check red: $(sed 's/^FAIL: //' "$zt_d/caps.txt")"
  RED_SEEN="$RED_SEEN$1 "
  return 0
}

if [ "${MUTATE:-0}" = 1 ]; then
  z80c_runner "$WORK/obj"
fi

fail=0
refused=0
retried=0
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
  # The two variants of the written cartridge, each held to its one
  # refusal: the bank turned inside a block the translator did not
  # close, and the proved stack pushed under the work RAM.
  if [ "$rom" = "$INDIRECT" ]; then
    expected_refusal "$name" "$trc" "$dir/translate.out" "bank switch inside block" \
      "the mapper written through a pointer" || fail=1
    continue
  fi
  if [ "$rom" = "$UNDERFLOW" ]; then
    expected_refusal "$name" "$trc" "$dir/translate.out" "stack outside ram" \
      "the stack set at \$C001 and pushed" || fail=1
    continue
  fi
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
  # Two judgements, three when the default budget was lowered once and
  # the second chosen table judged again.
  judged=2
  if grep -q '^z80c: over the ceiling at the default budget: ' "$dir/translate.out"; then
    judged=3
  fi
  if [ "$trc" -ne 0 ] \
     || [ "$(grep -c "^z80c: events PASS $FRAMES/$FRAMES memory=same insns/frame=[0-9]*\$" "$dir/translate.out")" -ne "$judged" ] \
     || ! grep -q "^written: " "$dir/translate.out"; then
    echo "FAIL: $name: the $judged judgements did not all pass, or nothing was written"
    fail=1
    continue
  fi
  # Every table written was weighed by the console's compiler and held
  # under the boot binary's ceiling before it was written.
  if ! sed -n "/^z80c: arm_bytes=[0-9]* est_launchme=[0-9]* ceiling=$CEILING base=[0-9]* ok\$/,\$p" "$dir/translate.out" | grep -q "^written: "; then
    echo "FAIL: $name: no 'z80c: arm_bytes= est_launchme= ceiling=$CEILING base= ok' line before 'written:'"
    fail=1
    continue
  fi
  # The sizes the console's compiler lives by, on every line of regions
  # this ROM's translation printed.
  if ! z80c_caps "$dir/translate.out" "$name"; then
    fail=1
    continue
  fi
  # The budget holds on every choice: bytes charged at most the budget,
  # unless the waits alone were over it (said by the translator).
  if ! grep -q '^translate: the waits alone cover ' "$dir/translate.out"; then
    zb_bad=$(sed -n 's/^z80c: selected blocks=[0-9]*\/[0-9]* bytes=\([0-9]*\) budget=\([0-9]*\) .*$/\1 \2/p' "$dir/translate.out" \
             | awk '$1 > $2 { print } END { if (NR == 0) print "none" }')
    if [ -n "$zb_bad" ]; then
      echo "FAIL: $name: a choice charged more bytes than its budget, or no choice was read ($zb_bad)"
      fail=1
      continue
    fi
  fi
  # A default budget over the ceiling is lowered once: the line says from
  # what to what, the second budget is the smaller, and it is the second
  # table that is weighed ok and written. Counted, so the summary says
  # whether the path ran on this checkout's ROMs.
  if grep -q '^z80c: over the ceiling at the default budget: ' "$dir/translate.out"; then
    rt_from=$(sed -n 's/^z80c: over the ceiling at the default budget: Z80C_BUDGET \([0-9]*\) -> \([0-9]*\), .*$/\1/p' "$dir/translate.out")
    rt_to=$(sed -n 's/^z80c: over the ceiling at the default budget: Z80C_BUDGET \([0-9]*\) -> \([0-9]*\), .*$/\2/p' "$dir/translate.out")
    if [ -z "$rt_from" ] || [ -z "$rt_to" ] || [ "$rt_to" -ge "$rt_from" ] \
       || [ "$(grep -c '^z80c: arm_bytes=.* over$' "$dir/translate.out")" -ne 1 ] \
       || [ "$(grep -c "^== choosing under Z80C_BUDGET=$rt_to bytes ==\$" "$dir/translate.out")" -ne 1 ]; then
      echo "FAIL: $name: the default budget was lowered but not as said (from ${rt_from:-?} to ${rt_to:-?})"
      fail=1
      continue
    fi
    retried=$(( retried + 1 ))
    echo "z80c: $name default budget lowered $rt_from -> $rt_to"
  fi
  echo "z80c: $name $(grep '^z80c: arm_bytes=' "$dir/translate.out" | tail -n 1 | sed 's/^z80c: //')"
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
  if [ "$rom" = "$FIXTURE" ] && ! z80c_fixture_floors "$dir"; then
    fail=1
    continue
  fi
  if [ "$rom" = "$FIXTURE" ] && ! z80c_over_ceiling "$rom" "$dir"; then
    fail=1
    continue
  fi
  if [ "$rom" = "$FIXTURE" ] && ! z80c_lowered "$rom" "$dir"; then
    fail=1
    continue
  fi
  if [ "$rom" = "$FIXTURE" ]; then
    if z80c_pairing "$rom" "$dir"; then
      echo "z80c: $name pairing: the image paired, one byte changed and another size refused"
    else
      echo "FAIL: $name: the console's pairing of the table with the image does not hold (above)"
      fail=1
      continue
    fi
  fi

  [ "${MUTATE:-0}" = 1 ] || continue

  # The written cartridge answers for the core's own guard and for
  # nothing else: its handful of instructions match none of the patterns
  # below, which are forms only a real program carries, and its video
  # memory holds nothing to flip.
  if [ "$rom" = "$FIXTURE" ]; then
    echo "== $name: the digest no longer compared at boot =="
    # The size alone pairs the table: the copy with one byte changed is
    # armed, and the pairing check must say so.
    mkdir -p "$dir/pairmut"
    sed 's/^  if(z80c_rom_fnv != fnv)$/  if(0)/' "$S/z80c.c" > "$dir/pairmut/z80c.c"
    if cmp -s "$S/z80c.c" "$dir/pairmut/z80c.c"; then
      echo "  [FAIL] mutation pairing matches nothing in z80c.c"
      fail=1
    else
      PLAYED="${PLAYED}pairing "
      set +e
      z80c_pairing "$rom" "$dir" "$dir/pairmut/z80c.c" >"$dir/pairmut/verdict" 2>&1
      zpm_rc=$?
      set -e
      if [ "$zpm_rc" -eq 1 ]; then
        echo "  [OK] mutation pairing turns the check red: $(grep -m1 'one byte changed' "$dir/pairmut/verdict" || head -n 1 "$dir/pairmut/verdict")"
        RED_SEEN="${RED_SEEN}pairing "
      else
        echo "  [FAIL] mutation pairing (the digest no longer compared) proved nothing (status $zpm_rc)"
        cat "$dir/pairmut/verdict"
        fail=1
      fi
    fi
    echo "== $name: the translator's cap on accesses broken two ways =="
    z80c_translator_mutation accjoin 's/^  if(region_access\[ra\] + region_access\[rb\] > (unsigned long)MAX_REGION_ACCESSES)$/  if(0)/' \
      "$rom" "$dir" || fail=1
    z80c_translator_mutation acccut 's/^      if(b->n > 0 \&\& accesses + insn_accesses(in) > (unsigned long)MAX_REGION_ACCESSES)$/      if(0)/' \
      "$rom" "$dir" || fail=1
    echo "== $name: the epoch of the mapper taken out of the core =="
    # Without it the chain trusts the successor a block rendered while
    # the bank behind that address was being turned: the block of the
    # bank that has just left runs in place of the one now there, and the
    # backdrop shows the inverted pattern.
    mutate epoch 's/^\( *\)z80c_map_epoch++;$/\1;/' \
         "the mapper no longer steps the epoch" "$S/cart.c" \
         "$dir/rom_code.c" "$rom" "$dir" core || fail=1
    echo "== $name: the core's push swapped in its header =="
    # The one breaking of the core's HEADER: the core's own PUSH -- the
    # one the frame interrupt uses to stack the return address, every
    # push of the image itself being translated and direct -- stores
    # its two bytes crossed, and the handler's direct RETI returns to a
    # crossed address. Red here proves that a header handed to
    # z80c_build reaches the core's files, not only the table
    # (tests/z80c/play.sh says how).
    mutate corepush 's/Z80_WR8(Z80_SP,(uint8)(z80_pv >> 8));/Z80_WR8(Z80_SP,(uint8)(z80_pv XX 8));/; s/Z80_WR8(Z80_SP,(uint8)(z80_pv \& 0xFFU));/Z80_WR8(Z80_SP,(uint8)(z80_pv >> 8));/; s/Z80_WR8(Z80_SP,(uint8)(z80_pv XX 8));/Z80_WR8(Z80_SP,(uint8)(z80_pv \& 0xFFU));/' \
         "the core's push swapped in its header" "$S/z80_ops.h" \
         "$dir/rom_code.c" "$rom" "$dir" core || fail=1
    echo "== $name: the write of the byte at \$C000 moved to \$C003 =="
    # The picture never reads that byte back: the frames stand, and only
    # the memory held beside them at the end of every frame can tell.
    mutate memory 's/Z80C_RAM_WR8(0xC000U,Z80_A)/Z80C_RAM_WR8(0xC003U,Z80_A)/' \
         "the byte at \$C000 written at \$C003" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    echo "== $name: the written cartridge's waits marked two wrong ways =="
    # The same two breakings of the waits as on a real ROM (below): with
    # no block a wait the loop at wait never ends its line and the
    # program is refused; with one block in two a wait the fill is one,
    # and every turn of it ends a line. The flags share one word (bit 0
    # the wait, bit 1 the close on a mapper write, the entry's index
    # above), so each breaking touches bit 0 of both values.
    mutate nowait 's/,1UL) },$/,0UL) },/; s/,3UL) },$/,2UL) },/' \
         "no block marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    mutate wait 's/^\(  { 0x[0-9A-F]*[02468ACE]UL, r_[0-9a-f]*, Z80C_ENTRY([0-9]*UL\),0UL) },$/\1,1UL) },/; s/^\(  { 0x[0-9A-F]*[02468ACE]UL, r_[0-9a-f]*, Z80C_ENTRY([0-9]*UL\),2UL) },$/\1,3UL) },/' \
         "one block in two marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
    echo "== $name: the direct path broken three ways =="
    z80c_direct_mutations "$dir" "$rom" || fail=1
    echo "== $name: the regions broken three ways =="
    z80c_region_mutations "$dir" "$rom" || fail=1
    continue
  fi

  echo "== $name: the chosen table's C broken, four ways that change what the program does =="
  # Every compare of an immediate leaves the carry flag inverted: the
  # next conditional branch goes the other way.
  mutate cp 's/^\(  Z80_OP_CP(0x[0-9A-F]*U);\)\( \/\* cp n \*\/\)$/\1 Z80_F ^= 0x01U;\2/' \
         "the carry flag inverted after every cp n" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # Every relative jump that leaves its region lands one byte past its
  # target (the ones inside a region are gotos, broken by "edge" below);
  # a table whose every jr is inside a region has no such form, and is
  # said not played.
  if grep -q '^ *Z80_PC = (uint16)(z80_win + 0x[0-9A-F]*U); /\* jr ' "$dir/rom_code.c"; then
    mutate jr 's/^\(  *Z80_PC = (uint16)(z80_win + 0x[0-9A-F]*U\)); \/\* jr /\1 + 1U); \/* jr /' \
           "every jr one byte past its target" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  else
    echo "  [INFO] mutation jr (every jr one byte past its target) has no form to break here: not played"
  fi
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
  mutate nowait 's/,1UL) },$/,0UL) },/; s/,3UL) },$/,2UL) },/' \
         "no block marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  # One block in two a wait (the ones whose position is even): a line
  # ends where the program does not wait, it falls behind its clock, and
  # the frames no longer match the reference taken with the intact
  # table's waits. Not every block: a table of nothing but waits never
  # chains two blocks, and the free run refuses it before anything is
  # judged. Bit 1 of the flags, the close on a mapper write, is kept,
  # and so is the entry's index above them.
  mutate wait 's/^\(  { 0x[0-9A-F]*[02468ACE]UL, r_[0-9a-f]*, Z80C_ENTRY([0-9]*UL\),0UL) },$/\1,1UL) },/; s/^\(  { 0x[0-9A-F]*[02468ACE]UL, r_[0-9a-f]*, Z80C_ENTRY([0-9]*UL\),2UL) },$/\1,3UL) },/' \
         "one block in two marked as a wait" "$dir/rom_code.c" "$dir/rom_code.c" "$rom" "$dir" || fail=1
  echo "== $name: the direct path broken three ways =="
  z80c_direct_mutations "$dir" "$rom" || fail=1
  echo "== $name: the regions broken three ways =="
  z80c_region_mutations "$dir" "$rom" || fail=1
done

# The mutations, over every ROM played: each seen red or refused at
# least once, or the check has not been seen to bite on it. Without a
# real ROM, only the four of the written cartridge were played.
if [ "${MUTATE:-0}" = 1 ]; then
  echo "== the mutations over every rom =="
  if [ "$found" -eq 1 ]; then
    wanted="epoch corepush memory cp jr load succ vram colour wait nowait direct stack bankend edge dispatch regexit pairing accjoin acccut"
  else
    wanted="epoch corepush memory wait nowait direct stack bankend edge dispatch regexit pairing accjoin acccut"
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

echo "z80c: default budget lowered on $retried rom(s)"
echo "failed=$fail refused=$refused"
exit $fail
