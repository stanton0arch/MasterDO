#ifndef BENCH_HARDWARE_H
#define BENCH_HARDWARE_H
/* Shim of include/3do/hardware.h + graphics_ccb.h for the host bench:
 * the CCB flag words, the two preamble word layouts and the minimal CCB
 * structure vdp.c fills. Values verbatim from include/3do/hardware.h:146-234
 * and the structure from include/3do/graphics_ccb.h:7-33. */
#include "types.h"

typedef int32 Coord;
typedef uint32 CelData;

typedef struct CCB CCB;
struct CCB
{
  uint32 ccb_Flags;

  CCB     *ccb_NextPtr;
  CelData *ccb_SourcePtr;
  void    *ccb_PLUTPtr;

  Coord  ccb_XPos;
  Coord  ccb_YPos;
  int32  ccb_HDX;
  int32  ccb_HDY;
  int32  ccb_VDX;
  int32  ccb_VDY;
  int32  ccb_HDDX;
  int32  ccb_HDDY;
  uint32 ccb_PIXC;
  uint32 ccb_PRE0;
  uint32 ccb_PRE1;

  int32 ccb_Width;
  int32 ccb_Height;
};

/* CCB control word flags (hardware.h:147-175) */
#define CCB_SKIP        0x80000000
#define CCB_LAST        0x40000000
#define CCB_NPABS       0x20000000
#define CCB_SPABS       0x10000000
#define CCB_PPABS       0x08000000
#define CCB_LDSIZE      0x04000000
#define CCB_LDPRS       0x02000000
#define CCB_LDPPMP      0x01000000
#define CCB_LDPLUT      0x00800000
#define CCB_CCBPRE      0x00400000
#define CCB_YOXY        0x00200000
#define CCB_ACSC        0x00100000
#define CCB_ALSC        0x00080000
#define CCB_ACW         0x00040000
#define CCB_ACCW        0x00020000
#define CCB_TWD         0x00010000
#define CCB_LCE         0x00008000
#define CCB_ACE         0x00004000
#define CCB_MARIA       0x00001000
#define CCB_PXOR        0x00000800
#define CCB_USEAV       0x00000400
#define CCB_PACKED      0x00000200
#define CCB_POVER_MASK  0x00000180
#define CCB_PLUTPOS     0x00000040
#define CCB_BGND        0x00000020
#define CCB_NOBLK       0x00000010

/* Cel first preamble word (hardware.h:185-210) */
#define PRE0_LITERAL    0x80000000
#define PRE0_BGND       0x40000000
#define PRE0_VCNT_MASK  0x0000FFC0
#define PRE0_LINEAR     0x00000010
#define PRE0_REP8       0x00000008
#define PRE0_BPP_MASK   0x00000007

#define PRE0_VCNT_SHIFT  6
#define PRE0_BPP_SHIFT   0

#define PRE0_BPP_1   0x00000001
#define PRE0_BPP_2   0x00000002
#define PRE0_BPP_4   0x00000003
#define PRE0_BPP_6   0x00000004
#define PRE0_BPP_8   0x00000005
#define PRE0_BPP_16  0x00000006

#define PRE0_VCNT_PREFETCH    1

/* Cel second preamble word (hardware.h:213-234) */
#define PRE1_WOFFSET8_MASK   0xFF000000
#define PRE1_WOFFSET10_MASK  0x03FF0000
#define PRE1_NOSWAP          0x00004000
#define PRE1_TLLSB_MASK      0x00003000
#define PRE1_LRFORM          0x00000800
#define PRE1_TLHPCNT_MASK    0x000007FF

#define PRE1_WOFFSET8_SHIFT   24
#define PRE1_WOFFSET10_SHIFT  16
#define PRE1_TLLSB_SHIFT      12
#define PRE1_TLHPCNT_SHIFT    0

#define PRE1_TLLSB_0     0x00000000
#define PRE1_TLLSB_PDC0  0x00001000

#define PRE1_WOFFSET_PREFETCH 2
#define PRE1_TLHPCNT_PREFETCH 1

#endif
