#!/bin/sh
#
# The cartridge read once, on the PC, and its code written as C.
#
# Builds the translator (tests/z80c/translate.c) on the host compiler,
# runs it on the image given, writes src/rom_code.c -- a generated file the
# repository ignores -- and compiles that file on the host compiler with
# the same strictness the console chain applies, so that a file the
# console would refuse is refused here first. Prints the translator's
# report and, after it, the size of the host object's code as the
# estimate of what the blocks will weigh in the boot binary.
#
# Without this step, `make` links the empty table (tests/z80c/
# rom_code_none.c) and the console runs the interpreter alone, saying so
# at boot. With it, `make clean && make` links the blocks.
#
#   sh tests/z80c/translate.sh takeme/roms/rom.sms
#   OUT=some/file.c sh tests/z80c/translate.sh <rom>   writes elsewhere
#
set -e

case "${1:-}" in
  '') echo "usage: sh tests/z80c/translate.sh <rom>"; exit 2;;
  /*) ROM=$1;;
  *)  ROM="$PWD/$1";;
esac
case "${OUT:-}" in ''|/*) ;; *) OUT="$PWD/$OUT";; esac

cd "$(dirname "$0")/../.."

CC=${CC:-gcc}
OUT=${OUT:-src/rom_code.c}
S=src
H=tests/cel8/3do

if [ ! -f "$ROM" ]; then
  echo "translate: no rom at $ROM (nothing written)"
  exit 2
fi

# No substitute may shadow a real header of src/ (tests/z80/run_z80.sh).
for h in "$H"/*.h; do
  if [ -e "$S/$(basename "$h")" ]; then
    echo "FAIL: $h shadows $S/$(basename "$h")"
    exit 1
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The sum a block closes at is the core's figure (src/z80c.h), read off
# the header and handed to the tool, so that the bound z80.h states for
# z80_run and the cap the tool applies are one number.
CAP=$(sed -n 's/^#define[[:space:]]*Z80C_BLOCK_TSTATES[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$S/z80c.h")
if [ -z "$CAP" ]; then
  echo "FAIL: Z80C_BLOCK_TSTATES not found in $S/z80c.h"
  exit 1
fi

$CC -O1 -std=gnu89 -Wall -Wextra -DBLOCK_TSTATES="${CAP}UL" \
    -o "$WORK/translate" tests/z80c/translate.c

# The report is kept to be printed last, with the estimate appended; the
# translator's exit status stops the script through set -e. The file is
# written beside the tool first and moved into place only once it has
# compiled: a refused file never sits where make would link it.
REPORT=$("$WORK/translate" "$ROM" "$WORK/rom_code.c")

# The file compiled as the console chain will compile it: strict C89,
# optimised so that the code size read off it is an estimate and not a
# debug listing, every warning on and fatal, the SDK stubs standing in
# for the console's headers.
$CC -O2 -std=c89 -Wall -Wextra -Werror -I"$H" -I"$S" \
    -c "$WORK/rom_code.c" -o "$WORK/rom_code.o"

TEXT=$(size "$WORK/rom_code.o" | awk 'NR == 2 { print $1 }')

mv -f "$WORK/rom_code.c" "$OUT"

echo "$REPORT est_text=${TEXT:-?}"
echo "written: $OUT"
