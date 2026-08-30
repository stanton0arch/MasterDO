#ifndef BENCH_GRAPHICS_H
#define BENCH_GRAPHICS_H
#include "types.h"
typedef uint32 Color;
#define MakeRGB15(r,g,b) ((Color)(((r)<<10)|((g)<<5)|(b)))
#endif
