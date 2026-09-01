#!/bin/sh
#
# The one property the memory probe's figures rest on, replayed.
#
# Every net= figure the probe publishes is one timed walk minus the reference
# walk. That subtraction isolates a memory access only if the compiled body of
# each walk is the compiled body of the reference PLUS EXACTLY ONE memory
# instruction. Nothing in the C says so, the compiler is free to break it, and
# it has already been broken once -- the first write walk dropped an add and
# under-reported every store by a register operation, which a run would have
# published as a number and no test would have caught.
#
# So this builds the probe switched on and reads the emitted code back.
#
# It is not hooked to a make target: the exception AGENTS.md grants on the
# Makefile is for HOST benches, and this one needs the cross compiler and the
# SDK object dumper. Run it by hand from the repository root.
#
#   sh tests/memprobe/check_shapes.sh
#
set -e

cd "$(dirname "$0")/../.."

DECAOF=./bin/compiler/linux/decaof
OBJ=build/memprobe.c.o
WORK=build/memprobe-check
DIS=$WORK/memprobe.dis

echo "== building with the probe switched on =="
make clean >/dev/null 2>&1
make DEFFLAGS='-DNDEBUG=1 -DLOG_LEVEL=2 -DSMS_MEM_PROBE=1' >/dev/null 2>&1

if [ ! -s "$OBJ" ]; then
  echo "FAIL: $OBJ was not produced"
  exit 1
fi

mkdir -p "$WORK"
"$DECAOF" -c "$OBJ" > "$DIS"

# The loop body of one walk: from the target of its backward branch to that
# branch. Prints "<instructions> <memory instructions>".
body() {
  awk -v fn="$1" '
    $0 ~ ("^" fn "$") { on = 1; next }
    on && /^[a-zA-Z_]/ { on = 0 }
    on && /0x/ {
      addr[n] = strtonum($1); line[n] = $0; n++
      if ($0 ~ /: B/) {
        t = strtonum($NF)
        if (t > 0 && t < strtonum($1)) { from = strtonum($1); to = t }
      }
    }
    END {
      if (!from) { print "0 0"; exit }
      for (k = 0; k < n; k++) {
        if (addr[k] >= to && addr[k] <= from) {
          i++
          sub(/.*: /, "", line[k])
          split(line[k], f, " ")
          if (f[1] ~ /^(LDR|STR|LDM|STM)/) m++
        }
      }
      print i+0, m+0
    }' "$DIS"
}

# walk                      expected instructions  expected memory instructions
CASES="memprobe_walk_none:6:0
memprobe_walk_rdw:7:1
memprobe_walk_wrw:7:1
memprobe_walk_rdb:7:1
memprobe_walk_wrb:7:1"

fail=0
for c in $CASES; do
  fn=$(echo "$c" | cut -d: -f1)
  wi=$(echo "$c" | cut -d: -f2)
  wm=$(echo "$c" | cut -d: -f3)
  set -- $(body "$fn")
  gi=$1
  gm=$2
  if [ "$gi" = "$wi" ] && [ "$gm" = "$wm" ]; then
    echo "  [OK] $fn: $gi instructions, $gm memory"
  else
    echo "  [FAIL] $fn: got $gi instructions and $gm memory, want $wi and $wm"
    fail=1
  fi
done

echo
if [ "$fail" = 0 ]; then
  echo "every walk is the reference body plus exactly one memory instruction"
  echo "failed=0"
else
  echo "a walk no longer isolates one access: its net= figure is NOT a memory access"
  echo "failed=1"
fi
exit $fail
