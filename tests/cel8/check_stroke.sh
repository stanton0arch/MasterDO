#!/bin/sh
#
# The two loop bodies the cel depth dossier's figures were counted from.
#
# comparaison-format-image.md prices one picture format against another by
# counting, instruction by instruction, the background stroke loop of
# vdp_render_line built two ways: six bit indexes packed four to three words,
# and one index per byte behind SMS_CEL_BPP8. Nothing in the delivered build
# exercises the second one. `make`, `make test-vdp` and `make test-z80` stay
# green whatever happens inside that #if -- a later edit can break it, or
# quietly change what it emits, and the only sign would be that nobody can
# re-derive the dossier's numbers any more.
#
# So this builds both objects and reads the two loops back out. It does not
# repeat the hand count of one stroke's current path: what it pins is the pair
# of loop bodies that count was made from -- how many instructions the
# compiler emits for a turn of the stroke loop, and how many of them touch
# memory. Every figure in the dossier is derived from those two, and a move in
# either is the signal to recount rather than to keep quoting.
#
# It also holds the switch to its other promise: at zero it must be inert, so
# the object built with -DSMS_CEL_BPP8=0 has to be byte for byte the object
# built without the switch at all.
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

# Outside build/, unlike the memory probe's check next door: this one builds
# three times and every build begins with a make clean, which would take the
# previous object and its listing with it. Removed on the way out.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Both builds are the development build and differ in the switch alone. The
# probe needs telemetry and a log, so there is no quieter configuration to
# compare it in, and comparing two different configurations would price the
# telemetry rather than the format.
build() {
  make clean >/dev/null 2>&1
  make DEFFLAGS="$1" >/dev/null 2>&1
  if [ ! -s "$OBJ" ]; then
    echo "FAIL: $OBJ was not produced by DEFFLAGS=$1"
    exit 1
  fi
  cp "$OBJ" "$2"
}

# The stroke loop of vdp_render_line: the first backward branch of that
# function whose body runs to a hundred instructions or more. The smaller
# loops before it are the uniform row of a line with the picture off and the
# eight byte decode of a missed pattern row; the composition loop of the
# scratch path, the same shape and the same size, comes after it. Source order
# settles which is which, and that is stated here rather than an address being
# trusted.
#
# Prints "<instructions> <memory instructions>", or "0 0" if no such loop is
# left -- which is itself a failure, and reported as one below.
loop() {
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
        if (s !~ /: B/) continue
        nf = split(s, g, " ")
        tgt = g[nf]
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
        if (i >= 100) { print i, m; exit }
      }
      print "0 0"
    }' "$1"
}

echo "== building the delivered form, six bit indexes packed =="
build "-DDEBUG=1" "$WORK/vdp-off.o"
"$DECAOF" -c "$WORK/vdp-off.o" > "$WORK/vdp-off.dis"

echo "== building the same file with the switch explicitly at zero =="
build "-DDEBUG=1 -DSMS_CEL_BPP8=0" "$WORK/vdp-zero.o"

echo "== building the probe, one index per byte =="
build "-DDEBUG=1 -DSMS_CEL_BPP8=1" "$WORK/vdp-on.o"
"$DECAOF" -c "$WORK/vdp-on.o" > "$WORK/vdp-on.dis"

fail=0


set -- $(loop "$WORK/vdp-off.dis")
if [ "$1" = "196" ] && [ "$2" = "64" ]; then
  echo "  [OK] six bit stroke loop: $1 instructions, $2 memory"
else
  echo "  [FAIL] six bit stroke loop: got $1 instructions and $2 memory, want 196 and 64"
  fail=1
fi

set -- $(loop "$WORK/vdp-on.dis")
if [ "$1" = "132" ] && [ "$2" = "48" ]; then
  echo "  [OK] eight bit stroke loop: $1 instructions, $2 memory"
else
  echo "  [FAIL] eight bit stroke loop: got $1 instructions and $2 memory, want 132 and 48"
  fail=1
fi

if cmp -s "$WORK/vdp-off.o" "$WORK/vdp-zero.o"; then
  echo "  [OK] the switch at zero is inert: the object is byte for byte the one without it"
else
  echo "  [FAIL] the switch at zero changed the object: the delivered build is no longer untouched"
  fail=1
fi

echo
if [ "$fail" = 0 ]; then
  echo "both loop bodies are the ones the dossier counted, and the switch off costs nothing"
  echo "failed=0"
else
  echo "a loop body moved: the dossier's cycle figures are no longer derivable from this code"
  echo "failed=1"
fi
exit $fail
