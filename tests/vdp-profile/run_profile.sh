#!/bin/sh
# One command, three checks, on the wrappers that let the render be broken
# into posts (src/vdp.h, VDP_REPEAT_BEGIN / VDP_REPEAT_END).
#
#   1. IDENTITY.  The picture, the composition scratch, the priority mask and
#      the two sprite bits taken LINE BY LINE, compared byte for byte against
#      the control over seven scenes, with a guard band on each side of the
#      picture buffer. Answers: do the five variants draw the same frame and
#      leave the same emulated state.
#
#   2. WORK.  The same bench under coverage, one variant per process, three
#      counters read out of the render. Answers: does each variant actually
#      do twice the work of its own post, and of no other.
#
#   3. DELIVERED FORM.  The bench built with the switch OFF -- the form of the
#      macros that goes out to a player, which checks 1 and 2 never compile --
#      rendering the same scenes and digesting them, against the instrumented
#      build's control. Answers: is the delivered render the one measured.
#
# Why three and not one. A wrapper opened one line too low -- below the stroke
# index and the two cursors instead of above them -- leaves the loop already
# finished when the second pass starts: it draws the SAME picture and does NO
# work, so check 1 passes and the post would have measured zero. Check 2 is
# what catches that, and it was verified to catch it. Conversely a post that
# advances a pointer it does not reset doubles its work and corrupts the
# picture, which is check 1's. And neither of the two says anything at all
# about the build with the switch off, which is check 3's whole subject.
#
# The reading half of the instrument -- the three functions of src/main.c that
# weigh a window and publish a round -- has its own bench, run first.
#
# The stub environment is the one of the cartridge bench, whose headers are
# copied beside this script so the test travels with the repository.

set -eu

B=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$B/../.." && pwd)
S="$ROOT/src"
H="$B/3do"
CC=${CC:-gcc}
FLAGS="-std=gnu89 -DLOG_LEVEL=3 -I$H -I$S"
W="$B/work"

[ -d "$S" ] || { echo "cannot find the sources at $S"; exit 1; }
[ -d "$H" ] || { echo "cannot find the stub headers at $H"; exit 1; }

# The coverage reader has to match the compiler that wrote the notes, or it
# refuses the file and every count comes back empty.
pick_gcov () {
  maj=$($CC -dumpversion 2>/dev/null | cut -d. -f1)
  for g in "${GCOV:-}" "gcov-$maj" gcov; do
    [ -n "$g" ] || continue
    command -v "$g" >/dev/null 2>&1 && { echo "$g"; return 0; }
  done
  return 1
}
GCOVBIN=$(pick_gcov) || { echo "no gcov on this machine: check 2 cannot run"; exit 1; }

fail=0
chk () {
  if [ "$1" = "$2" ]; then echo "  [OK] $3"
  else echo "  [FAIL] $3 ($1 vs $2)"; fail=1; fi
}
# A count scraped out of a coverage report must be a positive integer. Empty
# or zero means the pattern stopped matching, and a comparison between two
# empty strings would otherwise report OK and prove nothing.
number () {
  case "${1:-}" in
    ''|*[!0-9]*) echo "  [FAIL] $2: scraped '${1:-}', not a number"; fail=1; return 1 ;;
  esac
  [ "$1" -gt 0 ] || { echo "  [FAIL] $2: scraped 0"; fail=1; return 1; }
  return 0
}

echo "=== 0. the reading half: weighing a window, publishing a round ==="
$CC -O1 $FLAGS -w -DSMS_VDP_PROFILE=1 -o "$B/profile_main_bench" "$B/bench_profile_main.c"
"$B/profile_main_bench"

echo
echo "=== 1. identity: the same frame under the five variants ==="
# The bench itself is held to the warnings of the one modern compiler in the
# loop; vdp.c is not, its dialect being the target's.
$CC -O2 $FLAGS -Wall -Wextra -DSMS_VDP_PROFILE=1 -c -o "$B/.warncheck.o" "$B/bench_profile.c"
rm -f "$B/.warncheck.o"
$CC -O2 $FLAGS -w -DSMS_VDP_PROFILE=1 -o "$B/profile_bench" \
   "$B/bench_profile.c" "$S/vdp.c" "$S/sms.c"
"$B/profile_bench"

N=$("$B/profile_bench" variants)
number "$N" "the variant count" || exit 1
POSTS=$((N - 2))          # control, one variant per post, then the grouped one
ALL=$((N - 1))
echo
echo "  (the rotation has $N variants: a control, $POSTS posts, and the grouped one)"

echo
echo "=== 2. work: each variant doubles its own post and nothing else ==="
rm -rf "$W"; mkdir -p "$W"
cp "$S/vdp.c" "$W/vdp_under_test.c"
( cd "$W" && $CC -O0 -fno-inline --coverage $FLAGS -w -DSMS_VDP_PROFILE=1 \
    -o bench "$B/bench_profile.c" vdp_under_test.c "$S/sms.c" )

# The three counters, and the line of vdp.c each is read off. A pattern that
# stops matching, or starts matching twice, makes the proof meaningless in
# silence -- so both are refused below.
pat_1='word = read16_le(nt'
pat_2='if((uint32)sat\[i\] == VDP_SPR_TERMINATOR)'
pat_3='row += 16;'
name_1='strokes'; name_2='sprite-entries'; name_3='pack-groups'

v=0
while [ "$v" -lt "$N" ]; do
  ( cd "$W" && rm -f ./*.gcda ./*.gcov && ./bench "$v" >/dev/null &&
    $GCOVBIN bench-vdp_under_test.gcda >/dev/null 2>&1 )
  G="$W/vdp_under_test.c.gcov"
  [ -f "$G" ] || { echo "  [FAIL] $GCOVBIN produced no report for variant $v"; fail=1; break; }
  line=""
  p=1
  while [ "$p" -le "$POSTS" ]; do
    eval "pat=\$pat_$p; nm=\$name_$p"
    hits=$(grep -c -- "$pat" "$G" || true)
    if [ "$hits" != "1" ]; then
      echo "  [FAIL] the pattern for $nm matches $hits lines of vdp.c, not 1"
      fail=1
    fi
    c=$(grep -- "$pat" "$G" | head -1 | cut -d: -f1 | tr -d ' ')
    number "$c" "the $nm count of variant $v" || c=0
    eval "c${v}_$p=\$c"
    line="$line $nm=$c"
    p=$((p + 1))
  done
  echo "  variant $v:$line"
  v=$((v + 1))
done

[ "$fail" -eq 0 ] || { echo; echo "the coverage pass could not be trusted"; exit 1; }

# Expected: variant v doubles post p when v is p, or when v is the grouped one.
v=1
while [ "$v" -lt "$N" ]; do
  p=1
  while [ "$p" -le "$POSTS" ]; do
    eval "base=\$c0_$p; got=\$c${v}_$p; nm=\$name_$p"
    if [ "$v" -eq "$p" ] || [ "$v" -eq "$ALL" ]; then
      chk "$got" "$((base * 2))" "variant $v doubles the $nm"
    else
      chk "$got" "$base"         "variant $v leaves the $nm alone"
    fi
    p=$((p + 1))
  done
  v=$((v + 1))
done

echo
echo "=== 3. the delivered form of the macros, with the switch off ==="
$CC -O2 $FLAGS -w -DSMS_VDP_PROFILE=0 -o "$B/profile_bench_off" \
   "$B/bench_profile.c" "$S/vdp.c" "$S/sms.c"
"$B/profile_bench"     digest > "$W/on.txt"
"$B/profile_bench_off" digest > "$W/off.txt"
if diff -u "$W/on.txt" "$W/off.txt" >/dev/null; then
  echo "  [OK] the build that ships renders exactly what the control measures"
  sed 's/^/    /' "$W/off.txt"
else
  echo "  [FAIL] the delivered form and the instrumented control disagree"
  diff -u "$W/on.txt" "$W/off.txt" | sed 's/^/    /'
  fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo
echo "all three checks green"
