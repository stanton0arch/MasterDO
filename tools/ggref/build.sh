#!/bin/sh
# Builds ggref: the Z80 core of SMS Plus 0.9 (docs/smsplus_v0.9.0, not part
# of this repository) copied to a work directory with its loop shortcuts
# turned off (block instructions repeated at once, busy and delay loop
# skips), compiled with the declarations of prelude.h, and the driver.
set -e
H=$(cd "$(dirname "$0")" && pwd)
SRC=${SMSPLUS:-$H/../../docs/smsplus_v0.9.0}
W=${1:-$H/work}
mkdir -p "$W"
cp "$SRC/cpu/Z80.C" "$W/z80.c"
cp "$SRC/cpu/Z80.H" "$W/z80.h"
cp "$SRC/cpu/OSD_CPU.H" "$W/osd_cpu.h"
cp "$SRC/cpu/CPUINTRF.H" "$W/cpuintrf.h"
cp "$SRC/cpu/Z80DAA.H" "$W/z80daa.h"
sed -i 's/^#define REPEAT_AT_ONCE\t\t1/#define REPEAT_AT_ONCE\t\t0/; s/^#define BUSY_LOOP_HACKS \t1/#define BUSY_LOOP_HACKS \t0/; s/^#define TIME_LOOP_HACKS \t1/#define TIME_LOOP_HACKS \t0/' "$W/z80.c"
grep -q '^#define REPEAT_AT_ONCE		0' "$W/z80.c"
grep -q '^#define BUSY_LOOP_HACKS 	0' "$W/z80.c"
grep -q '^#define TIME_LOOP_HACKS 	0' "$W/z80.c"
gcc -O2 -w -include "$H/prelude.h" -I"$W" -c "$W/z80.c" -o "$W/z80.o"
gcc -O2 -w -include "$H/prelude.h" -I"$W" -o "$W/ggref" "$H/ggref.c" "$W/z80.o"
echo "$W/ggref built"
