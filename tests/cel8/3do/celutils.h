#ifndef BENCH_CELUTILS_H
#define BENCH_CELUTILS_H
/* Shim of include/3do/celutils.h for the host bench: the one entry point
 * vdp.c calls, backed by the stub in bench_cart.c. One binary compiles the
 * stub to return NULL so the manual path is proven. Option values verbatim
 * from include/3do/celutils.h:101-105. */
#include "types.h"
#include "hardware.h"

#define CREATECEL_UNCODED 0x00000000
#define CREATECEL_CODED   0x00000001

CCB *CreateCel(int32 width, int32 height, int32 bitsPerPixel,
               int32 options, void *dataBuf);

/* include/3do/celutils.h:307 -- the library's own per-pixel writer. */
/* RenderCelPixel is deliberately NOT declared here. The library's page
   excludes six bit coded cels from it, and a console run showed exactly
   that: nothing written, a flat black rectangle. vdp.c packs its rows
   itself; a return of that call breaks this bench at compile time. */

#endif
