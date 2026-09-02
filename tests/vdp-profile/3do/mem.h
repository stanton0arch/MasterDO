#ifndef BENCH_MEM_H
#define BENCH_MEM_H
#include "types.h"
#define MEMTYPE_ANY   (uint32)0
#define MEMTYPE_FILL  (uint32)0x00000100
#define MEMTYPE_DRAM  (uint32)0x00080000
/* The free memory query, as include/3do/mem.h:28-34 lays it out: the cel
   depth probe's boot line reads minfo_SysFree. Declared here, defined by the
   one host program that compiles the probe, tests/cel8/romrun.c. The render
   bench beside this header never compiles the probe and defines no AvailMem:
   a build of it with -DSMS_CEL_BPP8=1 would stop at the link, which is the
   right place for it to stop. */
typedef struct MemInfo
{
  uint32 minfo_SysFree;
  uint32 minfo_SysLargest;
  uint32 minfo_TaskFree;
  uint32 minfo_TaskLargest;
} MemInfo;
void AvailMem(MemInfo *minfo, uint32 memtype);
#endif
