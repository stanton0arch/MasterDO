#!/bin/sh
#
# The profile of every ROM of takeme/roms/, read by the two host probes
# of this directory: the code it runs and how it waits
# (tests/z80c/probe_code.c), and the writes it makes to the video part
# while the picture is being counted (tests/z80c/probe_raster.c). One
# line per ROM:
#
#   sweep: <file> frames=... insns_per_frame=... ... reg10_writes=...
#
# A ROM that does not boot, or whose core stops, is named with the probe
# and its status, and the others are still played; the exit status is 1
# if any was.
#
# src/ is compiled as it stands for the code probe. The raster probe needs
# two hooks in the video part: they are laid by sed on COPIES of src/ in
# the work directory -- every file, since a file of the core includes the
# headers of its own directory first -- and a pattern that no longer
# matches stops the script rather than build a probe that sees nothing.
# The work directory is removed at the end.
#
#   sh tests/z80c/sweep.sh                3600 frames a ROM
#   FRAMES=600 sh tests/z80c/sweep.sh     fewer
#   ROMS=some/dir sh tests/z80c/sweep.sh  the ROMs of another directory
#
set -e

case "${ROMS:-}" in ''|/*) ;; *) ROMS="$PWD/$ROMS";; esac

cd "$(dirname "$0")/../.."

ROMS=${ROMS:-takeme/roms}
FRAMES=${FRAMES:-3600}
case "$FRAMES" in ''|*[!0-9]*|0) echo "FRAMES must be a positive integer, not '$FRAMES'"; exit 2;; esac
CC=${CC:-gcc}
S=src
H=tests/cel8/3do
B=tests/z80c

for h in "$H"/*.h; do
  if [ -e "$S/$(basename "$h")" ]; then
    echo "FAIL: $h shadows $S/$(basename "$h")"
    exit 1
  fi
done

set -- "$ROMS"/*.sms "$ROMS"/*.gg
found=0
for r in "$@"; do
  [ -f "$r" ] && found=1
done
if [ "$found" -eq 0 ]; then
  echo "sweep: no rom in $ROMS, nothing played"
  exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"
CORE="cart sms vdp z80 z80c"

echo "== building the code probe on src/ as it stands =="
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -I"$H" -I"$S" -o "$WORK/probe_code" \
    "$B/probe_code.c" "$B/host.c" \
    $(for f in $CORE; do printf '%s ' "$S/$f.c"; done) "$B/rom_code_none.c" 2>"$WORK/code.build" \
  || { cat "$WORK/code.build"; exit 1; }
grep -E 'probe_code.c|host.c' "$WORK/code.build" || true

echo "== building the raster probe on a copy of src/ with its two hooks =="
mkdir -p "$WORK/src"
cp "$S"/*.c "$S"/*.h "$WORK/src/"
# The first statement of the data port's write: a colour write or a video
# memory write, with the address it lands on.
sed '/^#define VDP_IO_DATA_WRITE(v)/i\
extern void probe_note(int kind, unsigned long a);
/^#define VDP_IO_DATA_WRITE(v)/,/while(0)/s/^\( *\)sms\.vdp\.latch = 0;/\1probe_note((sms.vdp.code == VDP_CODE_CRAM_WRITE) ? 0 : 1,(unsigned long)sms.vdp.addr); sms.vdp.latch = 0;/' \
    "$S/vdp.h" > "$WORK/src/vdp.h"
# The register about to be stored, its number low and its value above.
sed 's/^  sms\.vdp\.reg\[number\] = (uint8)value;$/  probe_note(2,(unsigned long)number | (((unsigned long)value \& 0xFFUL) << 8));\
&/' "$S/vdp.c" > "$WORK/src/vdp.c"
# Counted as calls and declarations, "probe_note(": src/vdp.c already
# carries a name that contains the word.
if [ "$(grep -c 'probe_note(' "$WORK/src/vdp.h")" -ne 2 ] || [ "$(grep -c 'probe_note(' "$WORK/src/vdp.c")" -ne 1 ]; then
  echo "FAIL: the hooks of the raster probe no longer match src/vdp.h and src/vdp.c"
  exit 1
fi
$CC -O1 -std=gnu89 -Wall -Wextra $PINS -I"$H" -I"$WORK/src" -o "$WORK/probe_raster" \
    "$B/probe_raster.c" "$B/host.c" \
    $(for f in $CORE; do printf '%s ' "$WORK/src/$f.c"; done) "$B/rom_code_none.c" 2>"$WORK/raster.build" \
  || { cat "$WORK/raster.build"; exit 1; }
grep -E 'probe_raster.c|host.c' "$WORK/raster.build" || true

echo "== $FRAMES frames a rom =="
fail=0
for rom in "$@"; do
  [ -f "$rom" ] || continue
  name=$(basename "$rom")
  set +e
  "$WORK/probe_code" "$rom" "$FRAMES" >"$WORK/$name.code" 2>"$WORK/$name.code.log" &
  pc=$!
  "$WORK/probe_raster" "$rom" "$FRAMES" >"$WORK/$name.raster" 2>"$WORK/$name.raster.log" &
  pr=$!
  wait $pc; crc=$?
  wait $pr; rrc=$?
  set -e
  if [ "$crc" -ne 0 ] || [ "$rrc" -ne 0 ]; then
    echo "sweep: $name FAIL code=$crc raster=$rrc: $(cat "$WORK/$name.code" "$WORK/$name.raster" | grep -m1 FAIL || tail -n1 "$WORK/$name.code.log")"
    fail=1
    continue
  fi
  echo "sweep: $name $(cat "$WORK/$name.code") $(sed 's/^frames=[0-9]* //' "$WORK/$name.raster")"
done
exit $fail
