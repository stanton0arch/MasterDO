#!/bin/sh
# Builds sttest.bin / sttest.sym, the ARM60 model timing of the savestate
# coders (sttest.c, stc.c), with one state of -statedump built in:
#   STFLAGS="-DPROB_T=u32 -DROMMAP=8 -DLZ_DEPTH=8" stmk.sh state_NNNNNN.bin
# (-DSTTEST_CM adds the context mixing coder: minutes of model time).
set -e
S=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")"
M=$(cd ../.. && pwd)
export PATH=$M/bin/compiler/linux:$PATH
python3 -I -c "
import sys
b=open(sys.argv[1],'rb').read()
o=open('st_data.h','w')
o.write('#define ST_N %d\nstatic const u8 st_data[ST_N] = {\n' % len(b))
for i in range(0,len(b),24): o.write(','.join(str(x) for x in b[i:i+24])+',\n')
o.write('};\n')
" "$S"
armasm -bigend -fpu none -arch 3 -apcs "3/32/nofp/swst" hstub.s -o hstub.o
armcc -DNDEBUG=1 -DCM_LIMIT=30 $STFLAGS -O2 -zpno_check_stack -bigend -za1 -zi4 -fpu none -arch 3 \
      -apcs "3/32/nofp/swst/wide/softfp" -c sttest.c -o sttest.o 2>&1 | grep -iv warning | grep -v "^sttest.c:\|^stc.c:" || true
armlink -bin -ro-base 0x8000 -o sttest.bin -symbols sttest.sym hstub.o sttest.o $M/lib/community/libc.lib > sttest.txt 2>&1 || { cat sttest.txt; exit 1; }
rm -f st_data.h sttest.txt
echo "sttest.bin and sttest.sym built"
