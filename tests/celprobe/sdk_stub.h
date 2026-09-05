#ifndef CELPROBE_SDK_STUB_H
#define CELPROBE_SDK_STUB_H
/*
 * What the host stubs of tests/vdp-profile/3do/ do not declare and the cel
 * probe needs: the display list bits, the colour entry macros, the folio
 * calls it makes and the folio base it reads. Forced in front of the probe
 * by -include, so that the stub headers the probe then includes find every
 * name already defined and define nothing twice.
 *
 * Values verbatim from include/3do/hardware.h (the VDL_ bits, :19-141) and
 * include/3do/graphics.h (DEFAULT_DISPCTRL :62-71, VDLTYPE_FULL :76, the
 * colour entry macros :264-273). None of them is exercised for its value
 * here: the host check reads lists and patterns, never a display.
 */
#include "types.h"
#include "hardware.h"

typedef uint32 VDLEntry;

#define VDLTYPE_FULL 1

#define VDL_ENVIDDMA    0x00200000
#define VDL_RELSEL      0x00040000
#define VDL_LDCUR       0x00010000
#define VDL_LDPREV      0x00008000
#define VDL_LEN_SHIFT   9
#define VDL_LINE_SHIFT  0
#define VDL_DISPMOD_320 0x00000000
#define VDL_FULLRGB     0x00000000
#define VDL_DISPCTRL    0xC0000000
#define VDL_BACKGROUND  0x20000000
#define VDL_HINTEN      0x00000004
#define VDL_VINTEN      0x00000008
#define VDL_BLSB_BLUE   0x00000020
#define VDL_HSUB_ZERO   0x00000000
#define VDL_VSUB_ZERO   0x00000000
#define VDL_WINBLSB_BLUE  0x00010000
#define VDL_WINHSUB_ZERO  0x00000000
#define VDL_WINVSUB_ZERO  0x00000000
#define VDL_NOP         0xE1000000

#define DEFAULT_DISPCTRL \
  ( VDL_DISPCTRL|VDL_HINTEN|VDL_VINTEN \
    |VDL_BLSB_BLUE|VDL_HSUB_ZERO|VDL_VSUB_ZERO \
    |VDL_WINBLSB_BLUE|VDL_WINHSUB_ZERO|VDL_WINVSUB_ZERO )

#define MakeCLUTColorEntry(index,r,g,b) ((((uint32)(index)<<24)|VDL_FULLRGB \
                                          |((uint32)(r)<<16)|((uint32)(g)<<8)|((uint32)(b))))
#define MakeCLUTBackgroundEntry(r,g,b) ((VDL_DISPCTRL|VDL_BACKGROUND \
                                         |((uint32)(r)<<16)|((uint32)(g)<<8)|((uint32)(b))))

struct GrafFolio
{
  VDLEntry *gf_VDLPostDisplay;
};
extern struct GrafFolio *GrafBase;

Err   DrawCels(Item bitmapItem, CCB *ccb);
Err   SetScreenColors(Item screenItem, uint32 *entries, int32 count);
Err   DisableHAVG(Item screenItem);
Err   DisableVAVG(Item screenItem);
Err   EnableHAVG(Item screenItem);
Err   EnableVAVG(Item screenItem);
Item  SubmitVDL(VDLEntry *VDLDataPtr, int32 length, int32 type);
Err   SetVDL(Item screenItem, Item vdlItem);
void *GetPixelAddress(Item screenItem, Coord x, Coord y);

#endif
