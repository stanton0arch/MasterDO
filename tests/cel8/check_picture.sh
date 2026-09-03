#!/bin/sh
#
# The picture the delivered render composes, held against the one it drew
# before the format moved to a byte a pixel.
#
# check_stroke.sh next door pins the two stroke loops of the render. This
# one pins what those loops WRITE: the real core plays the real ROM on this
# host, and every row of every picture taken must digest to what the same
# row digested to on the build that drew the picture at six bits packed --
# the reference beside this script, taken once from that build. That
# build's own runner played the same frames and wrote its pictures raw,
# unpacked to one index per byte; each row was digested as romrun.c
# digests a row now, and the derivation was replayed from the archived
# tree, row for row, before the reference was kept. A row that differs is
# a failure, whatever a cycle count says: a format that draws another
# picture is not cheaper, it is wrong.
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
#   sh tests/cel8/check_picture.sh                twenty-one pictures over 1200 frames
#   PPM=some/dir sh tests/cel8/check_picture.sh   and one PPM per picture
#   WRITE=some/file TAKEN=<commit> sh tests/cel8/check_picture.sh
#       takes a NEW reference from the build as it stands instead of comparing.
#       Only ever after a change of picture that was meant: name the commit
#       in TAKEN=, and say in the story what changed and why.
#
set -e

# A path the caller gives is read from where the caller stands, not from
# the repository root this script moves to.
case "${WRITE:-}" in ''|/*) ;; *) WRITE="$PWD/$WRITE";; esac
case "${PPM:-}"   in ''|/*) ;; *) PPM="$PWD/$PPM";; esac
case "${REF:-}"   in ''|/*) ;; *) REF="$PWD/$REF";; esac

cd "$(dirname "$0")/../.."

ROM=takeme/roms/rom.sms
REF=${REF:-tests/cel8/picture-106b64a.fnv}
FRAMES=${FRAMES:-1200}
EVERY=${EVERY:-60}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
case "$EVERY"  in ''|*[!0-9]*|0) echo "EVERY must be a positive integer, not '$EVERY'"; exit 2;; esac
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
# arbiter's warning if it fires -- comes out.
PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"
CORE="$S/cart.c $S/sms.c $S/vdp.c $S/z80.c"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The runner is the only host file here, and this is the only place a second
# compiler reads it: its warnings are kept, the core's are not (they are the
# SDK headers' and are counted elsewhere).
echo "== building the delivered form, one index per byte =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -I"$H" -I"$S" -c -o "$WORK/romrun.o" "$B/romrun.c"
$CC -O1 -std=gnu89 -w $PINS -I"$H" -I"$S" -o "$WORK/romrun" "$WORK/romrun.o" $CORE

if [ -n "${PPM:-}" ]; then
  mkdir -p "$PPM"
fi

if [ -n "${WRITE:-}" ]; then
  echo "== playing $FRAMES frames, one picture every $EVERY, WRITING a new reference to $WRITE =="
  set +e
  "$WORK/romrun" "$ROM" "$FRAMES" "$EVERY" write "$WRITE" "${PPM:-}" "${TAKEN:-unnamed}" >"$WORK/out" 2>"$WORK/log"
  rc=$?
  set -e
  cat "$WORK/out"
  if [ "$rc" != 0 ]; then
    echo "  [FAIL] the reference was not taken (log follows)"
    cat "$WORK/log"
    exit 2
  fi
  # A reference is never minted from a run the compare path would refuse.
  if ! grep -q 'cel ok 256x192 bpp=8 coded' "$WORK/log" || grep -q 'preamble disagrees' "$WORK/log"; then
    echo "  [FAIL] the run does not draw through a sound cel of eight bits: reference discarded (log follows)"
    cat "$WORK/log"
    rm -f "$WRITE"
    exit 2
  fi
  echo "  reference written: $WRITE (this is not a comparison, nothing is proved)"
  exit 0
fi

# The status is taken from the runner itself, never from a pipe: a pipe
# reports its last command, and a check that bit in its figures once went
# green that way. The log of a run that failed is shown before the trap
# removes it.
echo "== playing $FRAMES frames, one picture every $EVERY, row against the reference =="
set +e
"$WORK/romrun" "$ROM" "$FRAMES" "$EVERY" compare "$REF" "${PPM:-}" >"$WORK/out" 2>"$WORK/log"
rc=$?
set -e
cat "$WORK/out"

if [ "$rc" = 3 ]; then
  grep 'skipped' "$WORK/log" || true
  echo "skipped: the rom on disc is not the one the reference names (nothing compared, nothing proved)"
  exit 3
fi
if [ "$rc" = 1 ]; then
  echo "  [FAIL] the render draws another picture than the reference (log follows)"
  cat "$WORK/log"
  echo "failed=1"
  exit 1
fi
if [ "$rc" != 0 ]; then
  echo "  [FAIL] the run did not compare anything (log follows)"
  cat "$WORK/log"
  echo "failed=1"
  exit 1
fi
if ! grep -q 'lines=[1-9]' "$WORK/out" || ! grep -q ' different=0$' "$WORK/out"; then
  echo "  [FAIL] the figures above are not a comparison that passed"
  echo "failed=1"
  exit 1
fi
echo "  [OK] every row the render composes is the row the reference holds"

# The cel the picture is drawn through, as the boot trace names it: eight
# bits, and the preamble the render computes by hand -- the pair a manual
# fallback would write into the block -- agreeing with the pair the library
# stub computes for a coded cel of eight bits (tests/cel8/romrun.c,
# CreateCel). Both are written from the same reading of the field, so this
# holds the render to that reading and not to the library: the arbiter
# with the real library is the console's own "cel pre lib= calc=" line,
# and its "preamble disagrees" warning is the one defect that draws a
# sheared picture with no error anywhere.
if ! grep -q 'cel ok 256x192 bpp=8 coded' "$WORK/log"; then
  echo "  [FAIL] the boot trace does not name a coded cel of eight bits (log follows)"
  cat "$WORK/log"
  echo "failed=1"
  exit 1
fi
if grep -q 'preamble disagrees' "$WORK/log"; then
  echo "  [FAIL] the hand-computed preamble disagrees with the library's (log follows)"
  grep 'preamble' "$WORK/log"
  echo "failed=1"
  exit 1
fi
echo "  [OK] the cel is coded at eight bits and the hand-computed preamble is the stub's (the console line is the arbiter)"
echo "failed=0"
exit 0
