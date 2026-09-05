#!/bin/sh
#
# The lists the cel probe times, checked on the host before a console run
# times them.
#
# A figure the probe publishes is the time of a list; it is the cost the
# probe names only if the list reads what the probe says it reads. This
# builds the probe's source on the host compiler against the stub headers
# of tests/vdp-profile/3do/, calls its builders directly and holds their
# lists against the rules written in list_check.c: every visible pixel read
# once by the windows of every band case, every pixel of the picture read
# once by the 896 tiles, sources on word addresses, preamble words the
# render's pair, one last cel a chain, the decision rule on figures either
# side of it.
#
# Then it breaks the probe two ways on a copy and expects the check to go
# red each time: a window four pixels too wide (a strip read twice), and
# the decision rule inverted. A check that stays green over a broken probe
# checks nothing.
#
# Not hooked to a make target: the probe is scaffolding that leaves with
# the render it measures for. Run it by hand from anywhere:
#
#   sh tests/celprobe/check_list.sh
#
set -e

cd "$(dirname "$0")/../.."

CC=${CC:-gcc}
S=src
H=tests/vdp-profile/3do
T=tests/celprobe

# No substitute may shadow a real header of src/ (tests/z80/run_z80.sh).
for h in "$H"/*.h "$T"/sdk_stub.h; do
  if [ -e "$S/$(basename "$h")" ]; then
    echo "FAIL: $h shadows $S/$(basename "$h")"
    exit 1
  fi
done

PINS="-DSMS_CEL_PROBE=1 -DSMS_TELEMETRY=1 -DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"
# The probe stores two console addresses in 32 bit words, which the host
# compiler is right to notice and this check is right to ignore: no
# address is ever dereferenced here.
WARN="-Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-function"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# $1 = directory searched first for celprobe.c, $2 = label, $3.. = extra flags
build() {
  d=$1; l=$2; shift 2
  $CC -O1 -std=gnu89 $WARN $PINS "$@" -include "$T/sdk_stub.h" \
      -DCELPROBE_SRC='"celprobe.c"' -I"$d" -I"$H" -I"$S" \
      -o "$WORK/list_check_$l" "$T/list_check.c"
}

# The stub header copies SDK values; the SDK headers, when they are on
# this machine, are held against the copies so that a divergence is seen.
echo "== the stub values against the SDK headers =="
if [ -f include/3do/hardware.h ] && [ -f include/3do/graphics.h ]; then
  bad=0
  grep -E '^#define (VDL_[A-Z0-9_]+|VDLTYPE_FULL) +[0-9x]+' "$T/sdk_stub.h" | while read -r _ name value; do
    real=$(grep -hE "^#define[[:space:]]+$name[[:space:]]" include/3do/hardware.h include/3do/graphics.h | head -1 | awk '{print $3}')
    if [ -z "$real" ]; then echo "  [FAIL] $name: not in the SDK headers"; echo x >> "$WORK/stubbad"
    elif [ "$(printf '%d' "$real")" != "$(printf '%d' "$value")" ]; then echo "  [FAIL] $name: stub $value, SDK $real"; echo x >> "$WORK/stubbad"
    fi
  done
  if [ -s "$WORK/stubbad" ]; then echo "failed=1"; exit 1; fi
  echo "  [OK] every VDL value of the stub is the SDK's"
else
  echo "  skipped: no SDK headers on this machine (nothing compared)"
fi

# The three refusals of the switch, each a build that must fail with its
# own message: a guard nobody has seen bite is a guard that may not.
echo "== the three refusals =="
refuse() {
  if build "$S" "refuse_$1" "$2" >"$WORK/refuse_$1.out" 2>&1; then
    echo "  [FAIL] $2 built the probe: the guard did not fire"; return 1
  fi
  if ! grep -q "$3" "$WORK/refuse_$1.out"; then
    echo "  [FAIL] $2 failed for another reason than the guard:"; head -5 "$WORK/refuse_$1.out"; return 1
  fi
  echo "  [OK] $2 is refused: $3"
}
refuse telemetry -DSMS_TELEMETRY=0 "SMS_CEL_PROBE needs SMS_TELEMETRY"
refuse log -DLOG_ENABLE=0 "SMS_CEL_PROBE needs LOG_ENABLE"
refuse level -DLOG_LEVEL=1 "SMS_CEL_PROBE needs LOG_LEVEL"

echo "== the probe as it stands =="
build "$S" intact
set +e
"$WORK/list_check_intact"
rc=$?
set -e
if [ "$rc" != 0 ]; then
  echo "failed=1"
  exit 1
fi

# The two mutations, each on its own copy, each expected to fail.
mutate() {
  mkdir -p "$WORK/$1"
  sed "$2" "$S/celprobe.c" > "$WORK/$1/celprobe.c"
  if cmp -s "$S/celprobe.c" "$WORK/$1/celprobe.c"; then
    echo "FAIL: mutation $1 changed nothing, its pattern no longer matches"
    exit 1
  fi
  build "$WORK/$1" "$1"
  set +e
  "$WORK/list_check_$1" > "$WORK/$1.out" 2>/dev/null
  rc=$?
  set -e
  if [ "$rc" = 0 ]; then
    echo "  [FAIL] mutation $1 ($3) left the check green"
    cat "$WORK/$1.out"
    return 1
  fi
  echo "  [OK] mutation $1 ($3) turns the check red"
  return 0
}

echo "== the probe broken, four ways =="
fail=0
mutate wide 's/(CELPROBE_PIC_W - CELPROBE_SCROLL)/(CELPROBE_PIC_W - CELPROBE_SCROLL + 4UL)/' \
       "first window four pixels too wide" || fail=1
mutate rule 's/(b_us <= a_us) ? "B" : "A"/(b_us > a_us) ? "B" : "A"/' \
       "decision rule inverted" || fail=1
mutate pitch 's/(CELPROBE_STRIDE_WORDS - PRE1_WOFFSET_PREFETCH)/(CELPROBE_STRIDE_WORDS - PRE1_WOFFSET_PREFETCH - 1UL)/' \
       "row pitch one word short in the preamble" || fail=1
mutate last 's/prev->ccb_Flags \&= ~CCB_LAST;/prev->ccb_Flags |= CCB_LAST;/' \
       "chain cut after its first cel" || fail=1

echo
if [ "$fail" = 0 ]; then
  echo "the lists read what the probe says they read, and the check has been seen to bite"
  echo "failed=0"
else
  echo "failed=1"
fi
exit $fail
