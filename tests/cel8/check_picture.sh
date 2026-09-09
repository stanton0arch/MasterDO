#!/bin/sh
#
# The picture the delivered render composes, held against the one it drew
# before the format moved to a byte a pixel -- and, since the background
# moved to the cel engine, against the older path on every frame.
#
# check_stroke.sh next door pins the two stroke loops of the older render.
# This one pins what the render DRAWS: the real core plays the real ROM on
# this host, and every row of every picture taken must digest to what the
# same row digested to on the build that drew the picture at six bits
# packed -- the reference beside this script, taken once from that build.
# That build's own runner played the same frames and wrote its pictures
# raw, unpacked to one index per byte; each row was digested as romrun.c
# digests a row now, and the derivation was replayed from the archived
# tree, row for row, before the reference was kept. A row that differs is
# a failure, whatever a cycle count says: a format that draws another
# picture is not cheaper, it is wrong.
#
# Since the picture moved to the cel engine (src/common.h,
# SMS_DECOR_CEL), it is no longer a buffer the render fills: it is what a
# list of cels -- windows of the background picture, sprites, priority
# tiles, backdrop -- makes the engine draw. The runner reconstitutes that
# from the blocks the render hands out -- every field held, every block
# painted as the engine paints it, cut to the picture's area -- and more
# figures come out: every window sound, no pixel left uncovered, no more
# pixels read than the picture plus the alignment columns allow, every
# small cel sound, the two sprite bits raised on the frames the older path
# raises them, and a set of synthetic scenes for the cases of the journal,
# the bands and the sprites the ROM does not reach. All are demanded
# below.
#
# Three runs, because the frozen reference holds one picture in sixty and
# the writes that cut a picture into bands land on a few frames a minute:
#
#   1. the OLDER path (SMS_DECOR_CEL 0, the processor composes) against
#      the frozen reference -- the oracle stays proved;
#   2. the older path writes a reference of EVERY frame into the work
#      directory;
#   3. the delivered path (SMS_DECOR_CEL 1) against the frozen reference,
#      and then against the every-frame one: 1200 pictures, 230400 rows,
#      and the two sprite bits of every frame (which only the every-frame
#      reference carries: the frozen one keeps its format).
#
# It needs the ROM the console runs, which is not distributed with the
# repository, and it needs it to be THE ROM the reference was taken from,
# which the reference names by size and by digest. Without the ROM, or
# with another one, the check is SKIPPED and says so, with an exit status
# that is neither a pass nor a failure. A pass is never reported for a run
# that compared nothing.
#
# Not hooked to a make target for that reason: a target that passes on one
# machine and skips on another is not a test of the repository.
#
#   sh tests/cel8/check_picture.sh                twenty-one pictures over 1200 frames, then all 1200
#   PPM=some/dir sh tests/cel8/check_picture.sh   and one PPM per picture of the frozen set
#   TOLERATED_MAX=8 ROM=some/rom.gg FROZEN=0 sh tests/cel8/check_picture.sh
#       rows that may differ inside the fold of a degraded picture (bands
#       or palette segments past their cap): counted apart, never as a
#       defect, and bounded so that the tolerance stays honest
#   MUTATE=1 sh tests/cel8/check_picture.sh
#       and then breaks the render thirty-three ways, on thirty-three copies
#       of src/vdp.c, and demands that each copy turn the every-frame check
#       red: a check that has not been seen to bite proves nothing
#       (tests/celprobe/check_list.sh).
#   WRITE=some/file TAKEN=<commit> sh tests/cel8/check_picture.sh
#       takes a NEW frozen reference from the delivered build as it stands
#       instead of comparing. Only ever after a change of picture that was
#       meant: name the commit in TAKEN=, and say in the story what changed
#       and why.
#   ROM=some/rom.sms FROZEN=0 sh tests/cel8/check_picture.sh
#       plays ANOTHER ROM than the reference's: the two runs against the
#       frozen reference (1 and 3a) are left out, since they would refuse
#       it, and the older path stays the oracle of every frame (2 and 3b),
#       with the scenes and the mutations. What this proves is the list
#       against the older path on that ROM; what it does not prove is the
#       older path against its own frozen picture, and the verdict says
#       so. For the day the reference ROM is not on the disc.
#
set -e

# A path the caller gives is read from where the caller stands, not from
# the repository root this script moves to.
case "${WRITE:-}" in ''|/*) ;; *) WRITE="$PWD/$WRITE";; esac
case "${PPM:-}"   in ''|/*) ;; *) PPM="$PWD/$PPM";; esac
case "${REF:-}"   in ''|/*) ;; *) REF="$PWD/$REF";; esac
case "${ROM:-}"   in ''|/*) ;; *) ROM="$PWD/$ROM";; esac

cd "$(dirname "$0")/../.."

ROM=${ROM:-takeme/roms/rom.sms}
REF=${REF:-tests/cel8/picture-106b64a.fnv}
FROZEN=${FROZEN:-1}
FRAMES=${FRAMES:-1200}
EVERY=${EVERY:-60}
# The rows a run may draw other than the older path inside the fold of a
# degraded picture -- bands or palette segments folded past their cap, a
# journal full: from the fold line on, the rows are drawn from the final
# state on purpose, and the runner counts the ones that differ there
# apart, tolerated. A bound keeps that tolerance from hiding a defect:
# none by default (the reference ROM folds one picture, frame 106, and
# differs on no row of it); a ROM that folds visibly gets its figure.
TOLERATED_MAX=${TOLERATED_MAX:-0}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
case "$EVERY"  in ''|*[!0-9]*|0) echo "EVERY must be a positive integer, not '$EVERY'"; exit 2;; esac
case "$TOLERATED_MAX" in ''|*[!0-9]*) echo "TOLERATED_MAX must be an integer, not '$TOLERATED_MAX'"; exit 2;; esac
CC=${CC:-gcc}
S=src
H=tests/vdp-profile/3do
B=tests/cel8

if [ ! -f "$ROM" ]; then
  echo "skipped: no rom at $ROM (nothing compared, nothing proved)"
  exit 3
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

# The same pins as the processor bench: the host has no assembler for the
# recompiled cores, the interrupt test source is off, and the counters the
# render keeps are on. The log level lets INFO through, so the boot line
# this script reads at its end -- the cel's depth, and the preamble
# arbiter's warning if it fires -- comes out. Which path draws the
# background is the one pin that varies, per build below.
PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"

# The pictures a run takes, and so the rows it must find identical: one
# every EVERY frames, and the last frame whether or not it falls on one.
# Demanded as an exact count, not as "some rows": a run that took fewer
# pictures than the reference holds is refused by the runner, but a count
# written here is what a reader checks against the story.
WANT_PICTURES=$(( (FRAMES + EVERY - 1) / EVERY ))
if [ $(( (FRAMES - 1) % EVERY )) -ne 0 ]; then
  WANT_PICTURES=$((WANT_PICTURES + 1))
fi
WANT_LINES=$((WANT_PICTURES * 192))
ALL_LINES=$((FRAMES * 192))

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The runner is the only host file here, and this is the only place a second
# compiler reads it: its warnings are kept, the core's are not (they are the
# SDK headers' and are counted elsewhere). One object per path: the runner
# reads the picture off the buffer on one and off the list on the other.
echo "== building the runner for both paths =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -DSMS_DECOR_CEL=0 -I"$H" -I"$S" -c -o "$WORK/romrun0.o" "$B/romrun.c"
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -DSMS_DECOR_CEL=1 -I"$H" -I"$S" -c -o "$WORK/romrun1.o" "$B/romrun.c"

# The core linked with a named copy of vdp.c on a named path: the render
# as it stands, or one of the broken copies below. The copy's own directory
# holds nothing but the copy, so its includes still read the headers of
# src/.
#
#   build <vdp.c> <binary> <0|1>
build() {
  $CC -O1 -std=gnu89 -w $PINS -DSMS_DECOR_CEL="$3" -I"$H" -I"$S" -o "$2" "$WORK/romrun$3.o" \
      "$S/cart.c" "$S/sms.c" "$1" "$S/z80.c"
}

echo "== building the older path (the processor composes) and the delivered one (cel windows) =="
build "$S/vdp.c" "$WORK/romrun_old" 0
build "$S/vdp.c" "$WORK/romrun" 1

if [ -n "${PPM:-}" ]; then
  mkdir -p "$PPM"
fi

# One run in write mode: a reference of the pictures the run takes. The
# runner writes to a temporary name and renames at the end, so a run that
# stopped half way leaves nothing. A reference is never minted from a run
# the compare path would refuse.
#
#   mint <binary> <reference> <every> <taken> <log>
mint() {
  set +e
  "$1" "$ROM" "$FRAMES" "$3" write "$2" "" "$4" >"$5.out" 2>"$5"
  rc=$?
  set -e
  if [ "$rc" != 0 ]; then
    cat "$5.out"
    echo "  [FAIL] the reference was not taken (log follows)"
    cat "$5"
    return 1
  fi
  if ! grep -q 'cel ok 256x192 bpp=8 coded' "$5" || grep -q 'preamble disagrees' "$5"; then
    cat "$5.out"
    echo "  [FAIL] the run does not draw through a sound cel of eight bits: reference discarded (log follows)"
    cat "$5"
    rm -f "$2"
    return 1
  fi
  return 0
}

if [ -n "${WRITE:-}" ]; then
  echo "== playing $FRAMES frames, one picture every $EVERY, WRITING a new reference to $WRITE =="
  if ! mint "$WORK/romrun" "$WRITE" "$EVERY" "${TAKEN:-unnamed}" "$WORK/mint.log"; then
    exit 2
  fi
  cat "$WORK/mint.log.out"
  echo "  reference written: $WRITE (this is not a comparison, nothing is proved)"
  exit 0
fi

# One run of one binary against one reference, and every figure it prints
# held. Prints the verdict lines; returns 0 when every one passed, 1 on
# the first that did not, 3 when the ROM on disc is not the reference's
# (the caller says so and stops: nothing was compared). Used on both
# paths, on the two references, and on every broken copy under MUTATE=1,
# where a return of 0 is the failure.
#
# The status is taken from the runner itself, never from a pipe: a pipe
# reports its last command, and a check that bit in its figures once went
# green that way. The log of a run that failed is shown before the trap
# removes it.
#
#   judge <binary> <out> <log> <ppm dir or empty> <reference> <every> <want lines> <0|1> <flags 0|1>
#
# The last argument says whether the reference carries the two sprite
# bits after each picture (the every-frame one the older path mints does,
# the frozen one does not), and so whether the list path is held to them.
judge() {
  start=$(date +%s)
  set +e
  "$1" "$ROM" "$FRAMES" "$6" compare "$5" "$4" >"$2" 2>"$3"
  rc=$?
  set -e
  cat "$2"
  echo "  ($(( $(date +%s) - start )) s)"

  if [ "$rc" = 3 ]; then
    grep 'skipped' "$3" || true
    echo "skipped: the rom on disc is not the one the reference names (nothing compared, nothing proved)"
    return 3
  fi
  if [ "$rc" = 1 ]; then
    echo "  [FAIL] the render draws another picture than the reference (log follows)"
    cat "$3"
    return 1
  fi
  if [ "$rc" != 0 ]; then
    echo "  [FAIL] the run did not compare anything (log follows)"
    cat "$3"
    return 1
  fi
  if ! grep -q " lines=$7 identical=[0-9]* different=0 tolerated=[0-9]* degraded=[0-9]*\$" "$2"; then
    echo "  [FAIL] the figures above are not $7 rows compared and found identical outside the degraded pictures"
    return 1
  fi
  deg_n=$(sed -n 's/^pictures=.* degraded=\([0-9]*\)$/\1/p' "$2")
  tol_n=$(sed -n 's/^pictures=.* tolerated=\([0-9]*\) .*$/\1/p' "$2")
  if [ "${tol_n:-0}" -gt "$TOLERATED_MAX" ]; then
    grep 'degraded from row' "$3" | head -8 || true
    echo "  [FAIL] $tol_n rows differ inside the fold of degraded pictures, more than TOLERATED_MAX=$TOLERATED_MAX allows"
    return 1
  fi
  echo "  [OK] every row the render composes is the row the reference holds ($7 rows; $deg_n pictures degraded, $tol_n rows tolerated inside their fold)"

  # The two tables between an index and a colour, checked on every picture
  # and reported as the fewest entries found right. The cel's palette must
  # be the identity, or the index a pixel carries is not the one the screen
  # asks its table for; and the screen table must be the colour memory
  # converted at the four documented levels plus the display's background
  # entry built from colour 0, computed by the runner from the colour
  # memory alone. Both are demanded in full: one entry wrong is one colour
  # wrong on the console. The per-frame figures below demand at least one
  # frame of their own.
  out_file="$2"
  tables() { grep -E '^(plut identity|clut entries|backdrop number|clut take|decor) ' "$out_file"; }
  if ! grep -q '^plut identity 32/32$' "$2"; then
    tables
    echo "  [FAIL] the cel palette is not the identity on every picture taken"
    return 1
  fi
  echo "  [OK] the cel palette is the identity on every picture taken"
  if ! grep -q '^clut entries 33/33$' "$2"; then
    tables
    echo "  [FAIL] the screen colour table is not the colour memory converted, background entry included"
    return 1
  fi
  echo "  [OK] the screen colour table is the colour memory at the four documented levels, every picture"
  # The border: the value the render hands the frame loop to fill the
  # ground with must be the backdrop NUMBER of register 7 on all three
  # components, on every picture taken -- a colour there would bypass the
  # screen table.
  if ! grep -qE '^backdrop number ([1-9][0-9]*)/\1$' "$2"; then
    tables
    echo "  [FAIL] the backdrop is not the number register 7 names on every picture"
    return 1
  fi
  echo "  [OK] the backdrop handed to the frame loop is the number of register 7, every picture"
  # The signal the frame loop rearms its table countdown on: the rebuild
  # must report a change on exactly the frames where a colour byte was
  # written, every frame played -- a silent rebuild leaves the console in
  # the boot table for the whole run, a chatty one sets the table on every
  # frame.
  if ! grep -qE '^clut take ([1-9][0-9]*)/\1$' "$2"; then
    tables
    echo "  [FAIL] the rebuild signal disagrees with the colour writes on some frame"
    return 1
  fi
  echo "  [OK] the rebuild signal fires on exactly the frames that wrote a colour"
  # A register written while the picture is being scanned -- the scroll
  # on two lines, on the last line, the masked column, the table base --
  # played on both paths and every row held against the runner's own
  # composition of the documented rule: the scroll of a line is the value
  # the register held at the end of the line before. The older path
  # proves the oracle, the list path is held to it; and the digest of the
  # scenes' pictures is printed by both, for the comparison below.
  if ! grep -qE '^scroll bands ([1-9][0-9]*)/\1$' "$2"; then
    tables
    grep 'raster scene' "$3" || true
    echo "  [FAIL] a raster scene draws a row other than the documented scroll or mask of that line"
    return 1
  fi
  echo "  [OK] $(grep '^scroll bands [0-9]' "$2") -- every row of the raster scenes shows its own line's registers"

  if [ "$8" = 0 ]; then
    # The older path: one cel of the whole picture, WITH the background
    # flag, and the hand-computed preamble agreeing with the stub's.
    if ! grep -q 'cel ok 256x192 bpp=8 coded.* bgnd=1 ' "$3"; then
      echo "  [FAIL] the boot trace does not name a coded cel of eight bits with the background flag (log follows)"
      cat "$3"
      return 1
    fi
    if grep -q 'preamble disagrees' "$3"; then
      echo "  [FAIL] the hand-computed preamble disagrees with the library's (log follows)"
      grep 'preamble' "$3"
      return 1
    fi
    echo "  [OK] the cel is coded at eight bits, background painted, and the hand-computed preamble is the stub's"
    return 0
  fi

  # The windows of the background picture, as the engine will read them
  # off the blocks: every one sound, on every frame played and in every
  # scene. One unsound window is a window the engine draws wrong on the
  # console with nothing to say so, and the runner names the first field
  # it found wrong.
  if ! grep -qE '^decor windows ([1-9][0-9]*)/\1$' "$2"; then
    tables
    grep 'decor window unsound' "$3" || true
    echo "  [FAIL] a window of the background picture is not what the engine must read"
    return 1
  fi
  echo "  [OK] every window of the background picture is sound, every frame"
  # No pixel of the picture's area left to whatever the screen held, and
  # no more pixels read than the picture plus the alignment columns: a
  # window read twice is the doubling the design refuses, and a hole is a
  # hole.
  if ! grep -qE '^decor read=[0-9]+ limit=[0-9]+ gaps=0$' "$2"; then
    tables
    echo "  [FAIL] some pixel of the picture's area is covered by no window"
    return 1
  fi
  read_px=$(sed -n 's/^decor read=\([0-9]*\) limit=\([0-9]*\) gaps=0$/\1/p' "$2")
  limit_px=$(sed -n 's/^decor read=\([0-9]*\) limit=\([0-9]*\) gaps=0$/\2/p' "$2")
  if [ -z "$read_px" ] || [ -z "$limit_px" ] || [ "$read_px" -gt "$limit_px" ]; then
    tables
    echo "  [FAIL] the windows read more pixels ($read_px) than the picture and its alignment columns allow ($limit_px)"
    return 1
  fi
  echo "  [OK] every pixel is covered by a window and the windows read $read_px pixels of the $limit_px allowed"
  # The bands really cut: the ROM's frames must have journaled some write
  # and drawn more bands than frames, or the every-frame comparison above
  # proved the one-band case alone and the band path rests on the scenes.
  journal_n=$(sed -n 's/^decor journal=\([0-9]*\) bands=\([0-9]*\)$/\1/p' "$2")
  bands_n=$(sed -n 's/^decor journal=\([0-9]*\) bands=\([0-9]*\)$/\2/p' "$2")
  if [ -z "$journal_n" ] || [ -z "$bands_n" ] || [ "$journal_n" -lt 1 ] || [ "$bands_n" -le "$FRAMES" ]; then
    tables
    echo "  [FAIL] the ROM's frames journaled no write or cut no band (journal=$journal_n bands=$bands_n over $FRAMES frames)"
    return 1
  fi
  echo "  [OK] the ROM's frames journaled $journal_n writes and drew $bands_n bands over $FRAMES frames"
  # The two sprite bits the program reads, overflow and collision,
  # raised on the same number of lines per frame as the older path
  # raises them pixel by pixel, on every frame of the every-frame
  # reference. The list path computes them from a table and a replay
  # without a pixel, and this is the only place that holds it to the
  # pixels.
  if [ "$9" = 1 ]; then
    if ! grep -qE '^sprite flags ([1-9][0-9]*)/\1$' "$2"; then
      tables
      grep 'flags differ' "$3" || true
      echo "  [FAIL] the sprite overflow or collision bit is not raised on the frames the older path raises it"
      return 1
    fi
    echo "  [OK] the sprite overflow and collision bits agree with the older path on every frame"
  fi

  # The synthetic scenes: the journal, the bands, the cap, the full
  # journal, register 2, the fine scroll, the lock, the full arena --
  # every expectation held. The runner names the first that was not.
  if ! grep -qE '^decor scenes ([1-9][0-9]*)/\1$' "$2"; then
    tables
    grep 'decor scene failed' "$3" || true
    echo "  [FAIL] a synthetic scene of the journal and the bands did not hold"
    return 1
  fi
  echo "  [OK] every synthetic scene of the journal and the bands holds"
  # The palette per line: on every picture presented, every line held
  # against the table of the segment it falls in, the conversion of the
  # colour memory as it stood when the line was counted -- the ROM's
  # frames, which have no segment, and the scenes that open some.
  if ! grep -qE '^palette lines ([1-9][0-9]*)/\1$' "$2"; then
    tables
    echo "  [FAIL] a line is shown through another table than the colour memory of its line"
    return 1
  fi
  echo "  [OK] $(grep '^palette lines' "$2") -- every line shows the table of its segment"
  # The small cels of the list -- sprites, priority runs, backdrop -- every
  # one sound on every band of every frame and every scene: its fields as
  # the engine reads them, its place in the chain, a sprite against the
  # list the runner derives from the attribute table by the documented
  # rules, a priority run against the priority bits of the tiles the
  # windows show, a backdrop block against the lines the runner saw the
  # display off on. The runner names the first fault.
  if ! grep -qE '^list cels ([1-9][0-9]*)/\1$' "$2"; then
    tables
    grep 'list cel unsound' "$3" || true
    echo "  [FAIL] a small cel of the list is not what the engine must read"
    return 1
  fi
  echo "  [OK] every small cel of the list is sound, every frame"
  # The reserve never spent: a refused cel is a sprite or a tile the
  # console would not draw, with a warning and nothing else to show it.
  if ! grep -qE '^list cels max=[0-9]+ refused=0$' "$2"; then
    tables
    echo "  [FAIL] the reserve of small cels ran out on some presentation"
    return 1
  fi
  echo "  [OK] $(grep '^list cels max=' "$2") -- no cel refused"
  # The cel of the older path as the boot trace still names it: eight
  # bits, WITHOUT the background flag on this path, and the preamble the
  # render computes by hand agreeing with the pair the library stub
  # computes for a coded cel of eight bits (tests/cel8/romrun.c,
  # CreateCel). Both are written from the same reading of the field, so
  # this holds the render to that reading and not to the library: the
  # arbiter with the real library is the console's own "cel pre lib= calc="
  # line, and its "preamble disagrees" warning is the one defect that draws
  # a sheared picture with no error anywhere.
  if ! grep -q 'cel ok 256x192 bpp=8 coded.* bgnd=0 ' "$3"; then
    echo "  [FAIL] the boot trace does not name a coded cel of eight bits without the background flag (log follows)"
    cat "$3"
    return 1
  fi
  if grep -q 'preamble disagrees' "$3"; then
    echo "  [FAIL] the hand-computed preamble disagrees with the library's (log follows)"
    grep 'preamble' "$3"
    return 1
  fi
  echo "  [OK] the cel is coded at eight bits, transparent on zero, and the hand-computed preamble is the stub's (the console line is the arbiter)"
  if ! grep -q 'sprites and priority tiles via cel list' "$3"; then
    echo "  [FAIL] the boot trace does not name the sprite sheet and the cel reserve (log follows)"
    cat "$3"
    return 1
  fi
  echo "  [OK] the boot trace names the sprites and the priority tiles drawn through the list"
  return 0
}

# The judge's status, taken apart from the shell's exit-on-error: 0, 1 or
# 3, and a 3 stops the script here rather than inside a copy's log.
run_judge() {
  set +e
  judge "$@"
  jrc=$?
  set -e
  if [ "$jrc" = 3 ]; then
    exit 3
  fi
  if [ "$jrc" != 0 ]; then
    echo "failed=1"
    exit 1
  fi
}

if [ "$FROZEN" = 1 ]; then
  echo "== 1. the older path, $FRAMES frames, one picture every $EVERY, row against the frozen reference =="
  run_judge "$WORK/romrun_old" "$WORK/old.out" "$WORK/old.log" "" "$REF" "$EVERY" "$WANT_LINES" 0 0
else
  echo "== 1. (left out: FROZEN=0, the frozen reference is not compared) =="
fi

echo "== 2. the older path writes a reference of every frame =="
if ! mint "$WORK/romrun_old" "$WORK/every.fnv" 1 "working-tree" "$WORK/mint.log"; then
  echo "failed=1"
  exit 1
fi
echo "  $(head -c 120 "$WORK/every.fnv" | head -1)"

if [ "$FROZEN" = 1 ]; then
  echo "== 3a. the delivered path, one picture every $EVERY, row against the frozen reference =="
  run_judge "$WORK/romrun" "$WORK/out" "$WORK/log" "${PPM:-}" "$REF" "$EVERY" "$WANT_LINES" 1 0
else
  echo "== 3a. (left out: FROZEN=0) =="
  # The older path still judged on its own figures -- the raster scenes
  # against the oracle among them -- against the every-frame reference
  # it has just written, which it trivially matches row for row.
  echo "== 3a'. the older path, every frame, its own figures =="
  run_judge "$WORK/romrun_old" "$WORK/old.out" "$WORK/old.log" "" "$WORK/every.fnv" 1 "$ALL_LINES" 0 0
fi

echo "== 3b. the delivered path, every frame, row against the older path =="
run_judge "$WORK/romrun" "$WORK/every.out" "$WORK/every.log" "" "$WORK/every.fnv" 1 "$ALL_LINES" 1 1

# The raster scenes' pictures, digested by both paths: the list must draw
# what the older path drew, register by register and line by line, over
# and above the oracle each was held to.
old_fnv=$(grep '^scroll bands digest=' "$WORK/old.out" | head -1)
new_fnv=$(grep '^scroll bands digest=' "$WORK/every.out" | head -1)
if [ -z "$old_fnv" ] || [ "$old_fnv" != "$new_fnv" ]; then
  echo "  [FAIL] the raster scenes digest apart on the two paths (older: ${old_fnv:-none}; list: ${new_fnv:-none})"
  echo "failed=1"
  exit 1
fi
echo "  [OK] the raster scenes digest the same on both paths ($old_fnv)"

if [ "${MUTATE:-0}" != 1 ]; then
  if [ "$FROZEN" = 1 ]; then
    echo "failed=0"
  else
    echo "failed=0 (FROZEN=0: the frozen reference was not compared)"
  fi
  exit 0
fi

# The check seen to bite: copies of src/vdp.c, each broken one way the
# console would show and nothing else would report, each run through the
# same judge against the every-frame reference, each expected red. A copy
# that stays green is a failure of THIS script. The mutation is a sed on
# one line; a pattern that no longer matches is refused rather than run as
# an intact copy.
mutate() {
  mkdir -p "$WORK/$1"
  sed "$2" "$S/vdp.c" > "$WORK/$1/vdp.c"
  if cmp -s "$S/vdp.c" "$WORK/$1/vdp.c"; then
    echo "FAIL: mutation $1 changed nothing, its pattern no longer matches"
    exit 1
  fi
  build "$WORK/$1/vdp.c" "$WORK/romrun_$1" 1
  set +e
  judge "$WORK/romrun_$1" "$WORK/$1.out" "$WORK/$1.log" "" "$WORK/every.fnv" 1 "$ALL_LINES" 1 1 >"$WORK/$1.verdict" 2>&1
  jrc=$?
  set -e
  if [ "$jrc" = 3 ]; then
    echo "  skipped: the rom on disc was refused inside mutation $1 (nothing compared, nothing proved)"
    cat "$WORK/$1.verdict"
    exit 3
  fi
  if [ "$jrc" = 0 ]; then
    echo "  [FAIL] mutation $1 ($3) left the check green"
    cat "$WORK/$1.verdict"
    return 1
  fi
  echo "  [OK] mutation $1 ($3) turns the check red: $(grep -m1 'FAIL' "$WORK/$1.verdict" | sed 's/^ *//')"
  return 0
}

echo "== the render broken, thirty-three ways =="
fail=0
# Every window one pixel to the right: the picture shifts, and screen
# column 0 is covered by no window wherever the scroll is a multiple of
# four.
mutate shift 's/^  x = (int32)xa - (int32)f;$/  x = (int32)xa - (int32)f + 1;/' \
       "every window one pixel to the right" || fail=1
# One tile of the picture never converted: its place in the picture keeps
# the zeroes of the boot whatever the name table names there.
mutate stale 's/^  old = sms.vdp.decor_word\[t\];$/  old = sms.vdp.decor_word[t]; if(t == 400UL) return;/' \
       "tile 400 never converted" || fail=1
# The writes of a band's first line put back one band late: the band
# starting on line L shows the memory as it stood before line L.
mutate replay 's/((uint32)sms.vdp.journal\[i\].line <= a)/((uint32)sms.vdp.journal[i].line < a)/' \
       "the write of a band's first line replayed one band late" || fail=1
# A write journaled one line later than it landed: the boundary moves.
mutate late 's/^    vdp_journal_add(sms.vdp.vcount,addr,(uint32)sms.vdp.vram\[addr\],value);$/    vdp_journal_add(sms.vdp.vcount + 1UL,addr,(uint32)sms.vdp.vram[addr],value);/' \
       "every journaled write one line late" || fail=1
# No band at all: everything folded into band 0, every visible write
# shown from line 0.
mutate fold 's/if(n >= VDP_LIST_BANDS)/if(n >= 1UL)/' \
       "no band cut, everything folded into the first" || fail=1
# A rewritten pattern the picture shows no longer marks its tiles for
# conversion: the picture keeps drawing the old pattern.
mutate refs 's/if(sms.vdp.refs\[chunk\] != 0U)/if(0)/' \
       "a rewritten pattern converts none of the tiles that show it" || fail=1
# The locked top rows end at row 8 instead of 16: rows 8 to 15 take the
# horizontal scroll.
mutate toplock 's/(ra < 16UL)/(ra < 8UL)/;s/rb = (b < 16UL) ? b : 16UL;/rb = (b < 8UL) ? b : 8UL;/' \
       "the locked top rows end at row 8" || fail=1
# The locked right columns take the vertical scroll.
mutate vlock 's/vdp_list_region(ya,yb,split,VDP_PIX_WIDTH,hs,0UL,0UL);/vdp_list_region(ya,yb,split,VDP_PIX_WIDTH,hs,1UL,sms.vdp.vscroll);/' \
       "the locked right columns scroll vertically" || fail=1
# The first block of a band no longer loads the palette.
mutate plut 's/vdp_chain_head->ccb_Flags |= CCB_LDPLUT;/;/' \
       "no block loads the palette" || fail=1
# The sprites drawn from the lowest numbered entry to the highest: where
# two overlap, the wrong one shows on top.
mutate order 's/^      i = (alive - 1UL) - k;$/      i = k;/' \
       "the sprites drawn in table order, the last one on top" || fail=1
# The priority palette keeps entry 16 opaque: colour 0 of the second bank
# covers the sprites under a priority tile.
mutate plut16 's/^  sms.vdp.plut_prio\[16\] = 0;$/  ;/' \
       "entry 16 of the priority palette left opaque" || fail=1
# A magnified sprite drawn at its natural width.
mutate zoom 's/^  hdx = (int32)((1UL << zoom) << 20);$/  hdx = 1L << 20;/' \
       "a magnified sprite drawn one pixel per pixel" || fail=1
# The ninth sprite of a line admitted and drawn, the overflow never raised.
mutate ninth 's/^            sms.vdp.spr_ovf_line\[y\] = 1;$/            sms.vdp.spr_adm[y][i >> 5] |= 1UL << (i \& 31UL);/' \
       "the ninth sprite of a line admitted, the overflow bit never raised" || fail=1
# The priority runs drawn before the sprites: the sprites cover them.
mutate prio_first '/^  vdp_list_sprites(a,b);$/{N;s/  vdp_list_sprites(a,b);\n  vdp_list_prio();/  vdp_list_prio();\n  vdp_list_sprites(a,b);/}' \
       "the priority tiles drawn before the sprites" || fail=1
# The background flag on every sprite: its colour 0 paints over the picture.
mutate bgnd_spr 's/^#define VDP_SPRITE_BGND 0UL$/#define VDP_SPRITE_BGND CCB_BGND/' \
       "the background flag set on every sprite" || fail=1
# The collision never raised.
mutate no_col '/^              sms.vdp.spr_collision = 1;$/{N;s/              sms.vdp.spr_collision = 1;\n              VDP_COUNT(spr_col);/              ;/}' \
       "the collision bit never raised" || fail=1
# The backdrop column not refilled when register 7 moves.
mutate column7 's/^          vdp_column_fill();$/          ;/' \
       "the backdrop column keeps the boot backdrop after register 7 moves" || fail=1
# The watch bytes not rebuilt when register 5 moves the table.
mutate watch5 '/^  if((number == 5UL)/,/^    }$/{s/^      vdp_decor_watch_rebuild();$/      ;/}' \
       "the moved sprite table is not watched" || fail=1
# Register 6 moving the pattern base leaves the per-line table standing.
mutate dirty6 's/^  if((number == 6UL) .*$/  if(0)/' \
       "a move of the sprite pattern base leaves the per-line table standing" || fail=1
# A tall sprite named by its odd pattern drawn from that pattern, not the pair.
mutate tall '/^vdp_list_sprites(uint32 a,$/,/^}$/{s/^        p &= ~1UL;$/        ;/}' \
       "a tall sprite named by its odd pattern drawn from the wrong place" || fail=1
# The last line of a magnified run on the first line of its row: dropped.
mutate ztail 's/^  if(((d1 & 1UL) != 0UL) && (la < lb))$/  if(0)/' \
       "the last line of a magnified sprite cut on a row dropped" || fail=1
# The patterns of the previous sprite table unwatched inside the picture.
mutate named_hot '/^vdp_sprite_scan(uint32 from)$/,/^}$/{s/^      if(from != 0UL)$/      if(0)/}' \
       "a pattern of the previous sprite table unwatched inside the picture" || fail=1
# Register 8 journaled on the line of its write instead of the line
# after: the line of the write shows the new scroll one line early.
mutate late8 's/^  line = (number == 8UL) ? (sms.vdp.vcount + 1UL) : sms.vdp.vcount;$/  line = sms.vdp.vcount;/' \
       "register 8 journaled on its own line, not the line after" || fail=1
# No register journaled at all: a register written mid-picture shows
# its last value from line 0.
mutate noreg 's/^          vdp_reg_note(number,(uint32)sms.vdp.reg\[number\],value);$/          ;/' \
       "no register journaled, the list takes the last value" || fail=1
# The journal no longer sorted: a byte written after register 8 on the
# same line lands after the register's entry, dated one line later.
mutate unsorted 's/^  while((i > 0UL) \&\& ((uint32)sms.vdp.journal\[i - 1UL\].line > line))$/  while(0)/' \
       "the journal no longer sorted by line" || fail=1
# The palette segments capped at two: a picture with more colour lines
# shows the last table from the second line on.
mutate segfold 's/^  if(k >= VDP_PAL_SEGMENTS)$/  if(k >= 2UL)/' \
       "the palette segments folded past the second" || fail=1
# A colour written on line 0 opens a segment: an entry of no lines.
mutate seg0 '/^vdp_cram_note(void)$/,/^}$/{s/^  if(line == 0UL)$/  if(0)/}' \
       "a colour written on line 0 opens a segment" || fail=1
# The Game Gear profile sets the window but shifts no cel: the window
# shows the top-left of the picture, not its middle.
mutate ggshift 's/^      sms.vdp.pic_x = -VDP_GG_VIEW_X;$/      sms.vdp.pic_x = 0;/' \
       "the game gear window without the cel shift" || fail=1
# A register entry not undone at the head of the presentation: band 0
# draws with the final value of a register written mid-picture.
mutate undo_reg 's/^\( *\)vdp_reg_apply((uint32)j->addr & 0x0FUL,(uint32)j->old);$/\1;/' \
       "register entries not undone before the first band" || fail=1
# The entries past the picture (register 8 on line 191) not put back
# after the last band: the next picture starts from the old value.
mutate tail_reg '/^    if(all != 0UL)$/,/^        sms.vdp.journal_replayed = i;$/{s/^\( *\)vdp_reg_apply((uint32)j->addr & 0x0FUL,(uint32)j->val);$/\1;/}' \
       "the entries past the picture never put back" || fail=1
# A segment count other than the previous picture's not reported as a
# change: a picture without a segment after one with leaves the screens
# on their lists.
mutate same_k 's/^    if(n != sms.vdp.clut_seg_prev)$/    if(0)/' \
       "a change of segment count not reported" || fail=1
# A partial sweep of the sprite table not marked: the lines before it
# keep last picture's table on the next picture.
mutate partial 's/^  sms.vdp.spr_partial = (from != 0UL) ? 1UL : 0UL;$/  sms.vdp.spr_partial = 0;/' \
       "a partial sweep of the sprite table forgotten" || fail=1
# The bitmap line of a segment without the view's origin: every segment
# starts view_y lines too high on the screen.
mutate seg_y 's/^        sms.vdp.clut_seg_line\[k\] = (uint8)(sms.vdp.view_y$/        sms.vdp.clut_seg_line[k] = (uint8)(0/' \
       "the segment lines without the view origin" || fail=1

echo
if [ "$fail" = 0 ]; then
  echo "the list draws the older path's picture on every frame, and the check has been seen to bite"
  echo "failed=0"
else
  echo "failed=1"
fi
exit $fail
