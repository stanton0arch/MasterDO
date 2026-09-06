#ifndef BENCH_GRAPHICS_H
#define BENCH_GRAPHICS_H
#include "types.h"
#include "hardware.h"
typedef uint32 Color;
#define MakeRGB15(r,g,b) ((Color)(((r)<<10)|((g)<<5)|(b)))
/* The screen colour table entries, verbatim from include/3do/graphics.h:264
   and :272; the VDL_ bits they use come from hardware.h beside this file. */
#define MakeCLUTColorEntry(index,r,g,b) ((((uint32)(index)<<24)|VDL_FULLRGB \
                                          |((uint32)(r)<<16)|((uint32)(g)<<8)|((uint32)(b))))
#define MakeCLUTBackgroundEntry(r,g,b) ((VDL_DISPCTRL|VDL_BACKGROUND    \
                                         |((uint32)(r)<<16)|((uint32)(g)<<8)|((uint32)(b))))
#endif
