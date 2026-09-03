#!/bin/sh
# One command, five checks: three on the wrappers that let the render be
# broken into posts (src/vdp.h, VDP_REPEAT_BEGIN / VDP_REPEAT_END), one on the
# decoded row cache the background composes from, and one on the line that
# never touches the priority scratch.
#
#   1. IDENTITY.  The picture, the priority mask and the two sprite bits
#      taken LINE BY LINE, compared byte for byte against the control over
#      seven scenes, with a guard band on each side of the picture buffer.
#      Answers: do the four variants draw the same frame and leave the same
#      emulated state.
#
#   2. WORK.  The same bench under coverage, one variant per process, three
#      counters read out of the render. Answers: does each variant actually
#      do twice the work of its own post, and of no other. Plus one counter
#      that must NOT double: the decoding of a tile row. The first pass over
#      a line leaves every row of it decoded, so a repeated background post
#      is a pass of hits and its displacement is the cost of composing from
#      a warm cache -- the decoding falls into the residual instead. That is
#      a property of the cache and not a defect of the wrapper, but it is a
#      property the figures have to be read with, so it is pinned here
#      rather than left to be rediscovered.
#
#   3. DELIVERED FORM.  The bench built with the switch OFF -- the form of the
#      macros that goes out to a player, which checks 1 and 2 never compile --
#      rendering the same scenes and digesting them, against the instrumented
#      build's control. Answers: is the delivered render the one measured.
#
#   4. THE DECODED ROW CACHE, in the delivered form. Every line held against
#      a composition written the old way -- four plane tables per pixel --
#      plus the cache caught serving, caught being thrown away by a write
#      through the port, and the picture caught staying where it is under a
#      fine scroll. Answers: does the cache render what the machine renders,
#      and does it cache at all. Runs inside check 1 as well; here it runs
#      through the macros that ship.
#
#   5. THE LINE THAT SKIPS THE SCRATCH, in the delivered form. A line with
#      no sprite on it emits its sixty-four words from the composition and
#      writes no mask. Its row is held byte for byte against a composition
#      written the old way, one byte at a time, that owes the path nothing,
#      for each of the eight fine scrolls and with the left column masked.
#      Then the same background is rendered the sprite way, forced there
#      by sprites that draw nothing, and the two rows must be the same row:
#      that is what holds the two compositions of vdp.c together, since
#      neither the picture nor the digests would move if only one of them
#      changed. The recut of a stroke is held in BOTH byte orders against
#      the byte run it must lay, so the form that runs on the console is
#      exercised by the build that cannot run it.
#
# Why three checks on the wrappers and not one. A wrapper opened one line
# too low -- below the stroke index and the two cursors instead of above
# them -- leaves the loop already
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

# The three counters, and the line of vdp.c each is read off, with the
# number of lines that line is expected to be. A pattern that stops
# matching, or starts matching a different number of times, makes the proof
# meaningless in silence -- so both are refused below. EVERY pattern here
# names exactly one line: a pattern that matched two would have its two
# counts added, and a doubling on one site cancelled by a fall on the other
# would then pass unseen, which is precisely the blindness this check
# exists to prevent.
#
# A line is rendered one of two ways -- the short way when it carries no
# sprite, with the priority scratch when it does -- so the background post
# is pinned once on each, on the emit of a stroke, whose row cursor carries
# a different name on each way for exactly this purpose. Both sit inside
# the same wrapper and both must double with it.
pat_1='VDP_EMIT8(gw0,gw1,rw);'
pat_2='if((uint32)sat\[i\] == VDP_SPR_TERMINATOR)'
name_1='scratch-strokes'; name_2='sprite-entries'
lines_1=1; lines_2=1

# The background post again, on the line that never touches the scratch.
# It doubles with post 1 like the one above; pinned separately because the
# scenes that reach it are not the scenes that reach the other.
pat_fast='VDP_EMIT8(gw0,gw1,ow);'
name_fast='short-strokes'
lines_fast=1

# And the two that must stay put whatever variant runs: the decode of a
# tile row, inside the miss branch of the background post. A repeated
# background pass finds every row of the line already decoded, so these
# counts are the same under every variant. The decode stands in both ways
# of composing a line and each site is weighed on its own -- the two
# scratch pointers carry different names for exactly this reason.
pat_dec_s='eb\[x\] = (uint8)'
name_dec_s='scratch-decodes'
lines_dec_s=1
pat_dec_f='fb\[x\] = (uint8)'
name_dec_f='short-decodes'
lines_dec_f=1

# Reads the count gcov put on the one line a pattern matches, refusing a
# report where that line never ran ("#####") or is not code at all. The
# diagnostics go to the error stream and the total to the standard one:
# this runs in a command substitution, so anything it prints normally would
# be captured into the count instead of reaching the operator, and its
# fail=1 would be set in a subshell and lost. The caller turns an empty
# answer into the failure.
scrape () {
  s_hits=$(grep -c -- "$1" "$G" || true)
  if [ "$s_hits" != "$2" ]; then
    echo "  [FAIL] the pattern for $3 matches $s_hits lines of vdp.c, not $2" >&2
  fi
  grep -- "$1" "$G" | cut -d: -f1 | tr -d ' ' |
    awk '{ if ($0 !~ /^[0-9]+$/) bad = 1; else t += $0 }
         END { if (bad || t == 0) print ""; else print t }'
}

v=0
while [ "$v" -lt "$N" ]; do
  ( cd "$W" && rm -f ./*.gcda ./*.gcov && ./bench "$v" >/dev/null &&
    $GCOVBIN bench-vdp_under_test.gcda >/dev/null 2>&1 )
  G="$W/vdp_under_test.c.gcov"
  [ -f "$G" ] || { echo "  [FAIL] $GCOVBIN produced no report for variant $v"; fail=1; break; }
  line=""
  p=1
  while [ "$p" -le "$POSTS" ]; do
    eval "pat=\$pat_$p; nm=\$name_$p; want=\$lines_$p"
    c=$(scrape "$pat" "$want" "$nm")
    number "$c" "the $nm count of variant $v" || c=0
    eval "c${v}_$p=\$c"
    line="$line $nm=$c"
    p=$((p + 1))
  done
  f=$(scrape "$pat_fast" "$lines_fast" "$name_fast")
  number "$f" "the $name_fast count of variant $v" || f=0
  eval "f$v=\$f"
  ds=$(scrape "$pat_dec_s" "$lines_dec_s" "$name_dec_s")
  number "$ds" "the $name_dec_s count of variant $v" || ds=0
  eval "ds$v=\$ds"
  df=$(scrape "$pat_dec_f" "$lines_dec_f" "$name_dec_f")
  number "$df" "the $name_dec_f count of variant $v" || df=0
  eval "df$v=\$df"
  echo "  variant $v:$line $name_fast=$f $name_dec_s=$ds $name_dec_f=$df"
  v=$((v + 1))
done

[ "$fail" -eq 0 ] || { echo; echo "the coverage pass could not be trusted"; exit 1; }

# Expected: variant v doubles post p when v is p, or when v is the grouped one.
BGPOST=1
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
  eval "got=\$f$v"
  if [ "$v" -eq "$BGPOST" ] || [ "$v" -eq "$ALL" ]; then
    chk "$got" "$((f0 * 2))" "variant $v doubles the $name_fast"
  else
    chk "$got" "$f0"         "variant $v leaves the $name_fast alone"
  fi
  eval "got=\$ds$v"
  chk "$got" "$ds0" "variant $v decodes no extra tile row through the scratch: the repeat is a pass of hits"
  eval "got=\$df$v"
  chk "$got" "$df0" "variant $v decodes no extra tile row the short way: the repeat is a pass of hits"
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

echo
echo "=== 4. the decoded row cache, through the form that ships ==="
# A section that did not run prints nothing and leaves failed=0 behind it, so
# the count of its assertions is checked, not only its verdict. The floor is
# well under what it emits today: it is there to catch a pass that vanished,
# not to be edited every time one is added.
"$B/profile_bench_off" > "$W/off-full.txt" || fail=1
sed -n '/the decoded row cache/,/^$/p' "$W/off-full.txt" | sed 's/^/  /'
TCN=$(sed -n '/the decoded row cache/,/^$/p' "$W/off-full.txt" | grep -c '\[OK\]' || true)
number "$TCN" "the row cache assertion count" || TCN=0
if [ "$TCN" -lt 20 ]; then
  echo "  [FAIL] the row cache pass emitted $TCN assertions, fewer than the 20 expected"
  fail=1
fi
grep -q '^failed=0$' "$W/off-full.txt" || \
  { echo "  [FAIL] the delivered form did not come out clean"; fail=1; }

echo
echo "=== 5. the line that skips the scratch, through the form that ships ==="
# The section that proves the short way: the row it emits held against the
# byte reference over the eight fine scrolls, the scratch caught being
# left alone, and the same background rendered BOTH ways and compared. Its
# assertion count is checked like the row cache's, and for the same reason
# -- a pass that vanished prints nothing and fails nothing.
sed -n '/the line that skips the scratch/,/^$/p' "$W/off-full.txt" | sed 's/^/  /'
FPN=$(sed -n '/the line that skips the scratch/,/^$/p' "$W/off-full.txt" | grep -c '\[OK\]' || true)
number "$FPN" "the short path assertion count" || FPN=0
if [ "$FPN" -lt 40 ]; then
  echo "  [FAIL] the short path pass emitted $FPN assertions, fewer than the 40 expected"
  fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo
echo "all five checks green"
