#!/bin/sh
#
# The picture the cel depth probe composes, held against the delivered one.
#
# check_stroke.sh next door pins the two loop bodies the dossier counted.
# This one pins what those loops WRITE: the real core plays the real ROM on
# this host, built once without the switch and once with it, and every row
# of every picture taken must be the same 256 indexes in both builds. A row
# of the probe that differs from the delivered row is a failure, whatever
# the dossier's cycle figures say -- a format that draws another picture is
# not cheaper, it is wrong.
#
# It needs the ROM the console runs, which is not distributed with the
# repository: without it the check is SKIPPED and says so, with an exit
# status that is neither a pass nor a failure. A pass is never reported for
# a run that compared nothing.
#
# Not hooked to a make target for that reason: a target that passes on one
# machine and skips on another is not a test of the repository.
#
#   sh tests/cel8/check_picture.sh            twenty-one pictures over 1200 frames
#   PPM=some/dir sh tests/cel8/check_picture.sh   and one PPM per picture, per build
#
set -e

cd "$(dirname "$0")/../.."

ROM=takeme/roms/rom.sms
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
# render keeps are on. The log level is the probe's minimum, so its boot
# line comes out and its guard against a mute build is compiled in.
PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"
CORE="$S/cart.c $S/sms.c $S/vdp.c $S/z80.c"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The runner is the only host file here, and this is the only place a second
# compiler reads it: its warnings are kept, the core's are not (they are the
# SDK headers' and are counted elsewhere).
echo "== building the delivered form, six bit indexes packed =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -I"$H" -I"$S" -c -o "$WORK/romrun6.o" "$B/romrun.c"
$CC -O1 -std=gnu89 -w $PINS -I"$H" -I"$S" -o "$WORK/romrun6" "$WORK/romrun6.o" $CORE

echo "== building the probe, one index per byte =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -DSMS_CEL_BPP8=1 -I"$H" -I"$S" -c -o "$WORK/romrun8.o" "$B/romrun.c"
$CC -O1 -std=gnu89 -w $PINS -DSMS_CEL_BPP8=1 -I"$H" -I"$S" -o "$WORK/romrun8" "$WORK/romrun8.o" $CORE

if [ -n "${PPM:-}" ]; then
  mkdir -p "$PPM"
fi

# The status is taken from the runner itself, never from a pipe: a pipe
# reports its last command, and a check that bit in its figures once went
# green that way. And the log of a run that failed is shown before the trap
# removes it, whichever of the two runs it was.
echo "== playing $FRAMES frames, one picture every $EVERY, delivered form =="
set +e
"$WORK/romrun6" "$ROM" "$FRAMES" "$EVERY" write "$WORK/ref.raw" ${PPM:+"$PPM"} >"$WORK/out6" 2>"$WORK/log6"
rc6=$?
set -e
cat "$WORK/out6"
if [ "$rc6" != 0 ]; then
  echo "  [FAIL] the delivered build did not play (log follows)"
  cat "$WORK/log6"
  echo "failed=1"
  exit 1
fi
if grep -q 'cel8 probe=' "$WORK/log6"; then
  echo "  [FAIL] the delivered build announced the probe: it is not the delivered build"
  echo "failed=1"
  exit 1
fi

echo "== playing the same frames on the probe, row against row =="
set +e
"$WORK/romrun8" "$ROM" "$FRAMES" "$EVERY" compare "$WORK/ref.raw" ${PPM:+"$PPM"} >"$WORK/out8" 2>"$WORK/log8"
rc8=$?
set -e
cat "$WORK/out8"

if [ "$rc8" = 1 ]; then
  echo "  [FAIL] the probe draws another picture (log follows)"
  cat "$WORK/log8"
  echo "failed=1"
  exit 1
fi
if [ "$rc8" != 0 ]; then
  echo "  [FAIL] the probe did not compare anything (log follows)"
  cat "$WORK/log8"
  echo "failed=1"
  exit 1
fi
if ! grep -q 'cel8 probe=on compose=on' "$WORK/log8"; then
  echo "  [FAIL] the probe did not announce itself on (log follows)"
  cat "$WORK/log8"
  echo "failed=1"
  exit 1
fi
if ! grep -q 'lines=[1-9]' "$WORK/out8" || ! grep -q ' different=0 ' "$WORK/out8"; then
  echo "  [FAIL] the figures above are not a comparison that passed"
  echo "failed=1"
  exit 1
fi
echo "  [OK] every row the probe composes is the row the delivered build composes"
echo "failed=0"
exit 0
