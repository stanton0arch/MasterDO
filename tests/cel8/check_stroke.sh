#!/bin/sh
#
# The two stroke loops of the render, pinned to what the compiler emits.
#
# comparaison-format-image.md priced the picture format by counting,
# instruction by instruction, the background stroke loop of vdp_render_line;
# the port that followed it composes every line one index per byte and
# carries two such loops -- the short way, for a line with no sprite, and
# the sprite way, which lays a priority mask beside the row. `make`,
# `make test-vdp` and check_picture.sh stay green whatever the compiler
# makes of either loop: a later edit can quietly make one of them spill to
# the stack on every stroke, and the only sign would be a frame that got
# slower with nobody able to say why.
#
# So this builds the object and reads the two loops back out. It does not
# repeat the hand count of one stroke's current path: what it pins is the
# pair of loop bodies that count is made from -- how many instructions the
# compiler emits for a turn of each stroke loop, and how many of them touch
# memory. A move in either is the signal to recount rather than to keep
# quoting.
#
# It is not hooked to a make target: the exception AGENTS.md grants on the
# Makefile is for HOST benches, and this one needs the cross compiler and the
# SDK object dumper. Run it by hand from the repository root.
#
#   sh tests/cel8/check_stroke.sh
#
set -e

cd "$(dirname "$0")/../.."

DECAOF=./bin/compiler/linux/decaof
OBJ=build/vdp.c.o

# Outside build/, unlike the memory probe's check next door: the build
# begins with a make clean, which would take a previous object and its
# listing with it. Removed on the way out.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The development build, which is the one the loops are counted in: the
# telemetry counters sit inside both loops, and a count taken without them
# would not be the count of the object that runs.
build() {
  make clean >/dev/null 2>&1
  make DEFFLAGS="$1" >/dev/null 2>&1
  if [ ! -s "$OBJ" ]; then
    echo "FAIL: $OBJ was not produced by DEFFLAGS=$1"
    exit 1
  fi
  cp "$OBJ" "$2"
}

# The stroke loops of vdp_render_line: every CONDITIONAL backward branch of
# that function whose body runs to a hundred instructions or more, in
# address order. Conditional, because the function also ends on a plain
# jump back to a shared epilogue, which spans both loops and is not one.
# The smaller loops are the uniform row of a line with the picture off and
# the eight byte decode of a missed pattern row. Source order settles which
# of the two found is which: the short way comes first, the sprite way
# after it, and that is stated here rather than an address being trusted.
#
# Prints one "<instructions> <memory instructions>" line per loop found.
loops() {
  awk '
    /^vdp_render_line$/ { inside = 1; next }
    inside && /^[a-zA-Z_]/ { inside = 0 }
    inside && /^  0x/ {
      addr[n] = strtonum($1)
      body[n] = $0
      n++
    }
    END {
      for (k = 0; k < n; k++) {
        s = body[k]
        sub(/.*: /, "", s)
        split(s, op, " ")
        if (op[1] !~ /^B[A-Z][A-Z]$/) continue
        tgt = op[2]
        if (tgt !~ /^0x/) continue
        t = strtonum(tgt)
        if (t >= addr[k]) continue
        i = 0; m = 0
        for (j = 0; j < n; j++) {
          if (addr[j] < t || addr[j] > addr[k]) continue
          i++
          line = body[j]
          sub(/.*: /, "", line)
          split(line, f, " ")
          if (f[1] ~ /^(LDR|STR|LDM|STM)/) m++
        }
        if (i >= 100) print i, m
      }
    }' "$1"
}

echo "== building the development form, one index per byte =="
build "-DDEBUG=1" "$WORK/vdp.o"
"$DECAOF" -c "$WORK/vdp.o" > "$WORK/vdp.dis"

fail=0

loops "$WORK/vdp.dis" > "$WORK/loops"
count=$(wc -l < "$WORK/loops")
if [ "$count" != 2 ]; then
  echo "  [FAIL] found $count stroke loops of a hundred instructions or more, want 2:"
  sed 's/^/    /' "$WORK/loops"
  fail=1
else
  set -- $(sed -n 1p "$WORK/loops")
  if [ "$1" = "126" ] && [ "$2" = "46" ]; then
    echo "  [OK] short way stroke loop: $1 instructions, $2 memory"
  else
    echo "  [FAIL] short way stroke loop: got $1 instructions and $2 memory, want 126 and 46"
    fail=1
  fi

  set -- $(sed -n 2p "$WORK/loops")
  if [ "$1" = "137" ] && [ "$2" = "51" ]; then
    echo "  [OK] sprite way stroke loop: $1 instructions, $2 memory"
  else
    echo "  [FAIL] sprite way stroke loop: got $1 instructions and $2 memory, want 137 and 51"
    fail=1
  fi
fi

echo
if [ "$fail" = 0 ]; then
  echo "both loop bodies are the ones on record"
  echo "failed=0"
else
  echo "a loop body moved: recount before quoting any figure derived from it"
  echo "failed=1"
fi
exit $fail
