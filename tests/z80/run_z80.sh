#!/bin/sh
# One command: build and run the ZEXALL host bench against src/ as it stands.
#
# The bench compiles the real files of the repository -- src/z80.c, which
# includes src/z80_ops.h, and src/sms.c for the world structure the core's
# state lives in.  No copy of either is made: "#include "z80_ops.h"" from
# src/z80.c resolves in src/ first, so no -I can substitute a header that
# lives there.  Only the 3DO SDK headers, which src/ does not carry, are taken
# from the substitutes in 3do/ -- which is why that -I comes first, and why
# step 1 below checks that none of them shadows a real header of src/.
#
# NO -DSMS_MAPPER.  The bench is built with the mapper the shipped binary is
# built with (MAPPER_SEGA, src/common.h's default), so Z80_WR8 expands to the
# store FOLLOWED BY THE MAPPER TRIGGER -- the macro every emulated write of
# the game goes through.  An earlier version passed -DSMS_MAPPER=2 and so
# measured the other branch of that macro, which nothing ships.
set -e

B=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
S=$B/../../src
ROOT=$B/../..
ASM=$ROOT/docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm

CC=${CC:-gcc}
# The dialect is NOT part of the overridable flags.  The bench is C89 plus the
# two bounded formatters, snprintf and vsnprintf, which the trace stubs use so
# that nothing they write can run off the end of their buffer; those two are
# C99, and gnu89 is what declares them.  A CFLAGS override that dropped the
# dialect would leave them implicitly declared, which is exactly the kind of
# quiet breakage this bench exists to catch elsewhere.
STD="-std=gnu89"
# -Wall -Wextra and not -w: this is the only place in the project where a host
# compiler ever sees src/z80.c and src/sms.c, and throwing that away is
# throwing away a second compiler's opinion for free.
CFLAGS=${CFLAGS:--O2 -Wall -Wextra}

# Every switch the bench's claims depend on, pinned rather than defaulted.
# bench_z80.c carries a matching #error for each, so a build that loses one
# fails loudly instead of measuring something else.
PINS="-DLOG_LEVEL=0 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0"

# ---- 1. no substitute may shadow a real header of src/ -------------------
# The -I of 3do/ comes before the -I of src/, so a file named like one of the
# core's own headers would silently replace it.  That trap has already cost
# this project time; it is one loop to close it for good.
shadow=0
for h in "$B"/3do/*.h; do
  name=$(basename "$h")
  if [ -e "$S/$name" ]; then
    echo "FAIL: tests/z80/3do/$name shadows src/$name" >&2
    shadow=1
  fi
done
[ "$shadow" -eq 0 ] || exit 1

# ---- 2. the committed tables must still be what the generator makes ------
# The SHA-256 in zexall_tests.c is a comment, and a comment checks nothing:
# editing a CRC by hand there is the cheapest way to make a red bench go green
# for ever.  When docs/ is present, regenerate into a temporary directory and
# diff.  When it is not -- a fresh clone -- say so rather than pretend.
if [ -f "$ASM" ]; then
  tmp=$(mktemp -d)
  python3 "$B/gen_tests.py" "$ASM" "$tmp" > "$tmp/gen.log" 2>&1 || {
    echo "FAIL: the generator refused; its output follows" >&2
    cat "$tmp/gen.log" >&2
    exit 1
  }
  for f in zexall_tests.c zexall_tests.h; do
    if ! diff -q "$tmp/$f" "$B/$f" > /dev/null; then
      echo "FAIL: $f differs from what gen_tests.py produces from" >&2
      echo "      $ASM" >&2
      diff -u "$B/$f" "$tmp/$f" | head -40 >&2
      exit 1
    fi
  done
  echo "descriptor tables: regenerated and identical to the committed pair"
  sed -n 's/^\(doc\|undoc\) /  &/p' "$tmp/gen.log"
else
  echo "descriptor tables: NOT re-checked -- $ASM is absent."
  echo "  docs/ is not tracked by git, so this is normal on a fresh clone."
  echo "  The committed tables are used as they are."
fi

# ---- 3. build ------------------------------------------------------------
# Separate objects so the bench's own units and the repository's are compiled
# with the same warning settings and either can be looked at on its own.
O=$(mktemp -d)
trap 'rm -rf "$tmp" "$O"' EXIT

objs=""
for src in "$B/bench_z80.c" "$B/zexall_tests.c" "$S/z80.c" "$S/sms.c"; do
  obj="$O/$(basename "$src" .c).o"
  $CC $STD $CFLAGS $PINS -I"$B/3do" -I"$S" -I"$B" -c "$src" -o "$obj"
  objs="$objs $obj"
done

# shellcheck disable=SC2086
$CC $STD $CFLAGS -o "$B/z80_bench" $objs

# ---- 4. run --------------------------------------------------------------
"$B/z80_bench"
