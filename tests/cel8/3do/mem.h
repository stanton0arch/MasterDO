#ifndef BENCH_MEM_H
#define BENCH_MEM_H
#include "types.h"
#define MEMTYPE_ANY   (uint32)0
#define MEMTYPE_FILL  (uint32)0x00000100
#define MEMTYPE_DRAM  (uint32)0x00080000
/* The free memory query, as include/3do/mem.h:28-34 lays it out. Nothing
   under src/ that the host programs compile reads it any more -- the cel
   depth probe whose boot line did is gone -- and it is kept because the
   header it stands in for declares it: a stub that dropped a declaration
   the real header has would make a future caller build here and fail on
   the console. No host program defines AvailMem today; one that calls it
   stops at the link, which is the right place for it to stop. */
typedef struct MemInfo
{
  uint32 minfo_SysFree;
  uint32 minfo_SysLargest;
  uint32 minfo_TaskFree;
  uint32 minfo_TaskLargest;
} MemInfo;
void AvailMem(MemInfo *minfo, uint32 memtype);
#endif
