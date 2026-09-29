/*
 * Z80 benchmark program and shared set-up helpers.
 *
 * The program is shaped like game code:
 *
 * main:    LD SP,$DFF0
 * loop:    LD IX,$C100 / LD B,16 / LD DE,16
 * obj:     CALL update / ADD IX,DE / DJNZ obj
 *          LD HL,$C400 / LD DE,$C600 / LD BC,32
 * copy:    LD A,(HL) / LD (DE),A / INC HL / INC DE / DEC BC / LD A,B / OR C
 *          JR NZ,copy
 *          LD A,($C000) / INC A / LD ($C000),A / AND $0F / CP 7 / JR NZ,skip
 *          LD A,$55 / LD ($C700),A
 * skip:    JP loop
 * update:  x += dx (back to 0 from $F0), y += dy, anim -= 1, all through
 *          (IX+d), with a PUSH BC / POP BC pair, then RET.
 *
 * One pass of the loop is about 5 900 T-states and 557 instructions,
 * 10.5 T-states per instruction on average, about a quarter of them
 * IX-indexed.
 */

#include "z80prog.h"

#include "string.h"

/* Flag bits of both cores (PSR_* in bench_z80.s and z80jit.c). */
#define PSR_S 0x08
#define PSR_Z 0x04
#define PSR_P 0x01

/* Object table: 16 objects of 16 bytes at $C100. */
#define OBJ_BASE   0x0100
#define OBJ_COUNT  16
#define OBJ_STRIDE 16
#define OBJ_ANIM0  0xFF

const uint8 z80prog_code[] = {
    0x31, 0xF0, 0xDF,             /* 0000 LD SP,$DFF0      */
    0xDD, 0x21, 0x00, 0xC1,       /* 0003 LD IX,$C100      */
    0x06, 0x10,                   /* 0007 LD B,16          */
    0x11, 0x10, 0x00,             /* 0009 LD DE,$0010      */
    0xCD, 0x3A, 0x00,             /* 000C CALL $003A       */
    0xDD, 0x19,                   /* 000F ADD IX,DE        */
    0x10, 0xF9,                   /* 0011 DJNZ $000C       */
    0x21, 0x00, 0xC4,             /* 0013 LD HL,$C400      */
    0x11, 0x00, 0xC6,             /* 0016 LD DE,$C600      */
    0x01, 0x20, 0x00,             /* 0019 LD BC,$0020      */
    0x7E,                         /* 001C LD A,(HL)        */
    0x12,                         /* 001D LD (DE),A        */
    0x23,                         /* 001E INC HL           */
    0x13,                         /* 001F INC DE           */
    0x0B,                         /* 0020 DEC BC           */
    0x78,                         /* 0021 LD A,B           */
    0xB1,                         /* 0022 OR C             */
    0x20, 0xF7,                   /* 0023 JR NZ,$001C      */
    0x3A, 0x00, 0xC0,             /* 0025 LD A,($C000)     */
    0x3C,                         /* 0028 INC A            */
    0x32, 0x00, 0xC0,             /* 0029 LD ($C000),A     */
    0xE6, 0x0F,                   /* 002C AND $0F          */
    0xFE, 0x07,                   /* 002E CP 7             */
    0x20, 0x05,                   /* 0030 JR NZ,$0037      */
    0x3E, 0x55,                   /* 0032 LD A,$55         */
    0x32, 0x00, 0xC7,             /* 0034 LD ($C700),A     */
    0xC3, 0x03, 0x00,             /* 0037 JP $0003         */
    0xDD, 0x7E, 0x00,             /* 003A LD A,(IX+0)      */
    0xDD, 0x86, 0x02,             /* 003D ADD A,(IX+2)     */
    0xDD, 0x77, 0x00,             /* 0040 LD (IX+0),A      */
    0xFE, 0xF0,                   /* 0043 CP $F0           */
    0x38, 0x04,                   /* 0045 JR C,$004B       */
    0xAF,                         /* 0047 XOR A            */
    0xDD, 0x77, 0x00,             /* 0048 LD (IX+0),A      */
    0xDD, 0x7E, 0x01,             /* 004B LD A,(IX+1)      */
    0xDD, 0x86, 0x03,             /* 004E ADD A,(IX+3)     */
    0xDD, 0x77, 0x01,             /* 0051 LD (IX+1),A      */
    0xC5,                         /* 0054 PUSH BC          */
    0x4F,                         /* 0055 LD C,A           */
    0xDD, 0x7E, 0x04,             /* 0056 LD A,(IX+4)      */
    0x3D,                         /* 0059 DEC A            */
    0xDD, 0x77, 0x04,             /* 005A LD (IX+4),A      */
    0xC1,                         /* 005D POP BC           */
    0xC9                          /* 005E RET              */
};

const uint32 z80prog_size = sizeof(z80prog_code);

void z80prog_fill_memmap(uint32 *memmap, uint32 cart_addr, uint32 ram_addr)
{
    uint32 p;

    for (p = 0; p < 64; p++) {
        uint32 z80_addr = p << 10;
        uint32 entry;

        if (p < 48)
            entry = cart_addr + (z80_addr & (Z80PROG_CART_SIZE - 1)) - z80_addr;
        else if (p < 56)
            entry = ram_addr - 0xC000u;
        else
            entry = ram_addr - 0xE000u;
        memmap[63 - p] = entry;
    }
}

void z80prog_fill_pzst(uint8 *pzst)
{
    uint32 v;

    for (v = 0; v < 256; v++) {
        uint32 par = v;
        uint32 f = 0;

        par ^= par >> 4;
        par ^= par >> 2;
        par ^= par >> 1;
        if ((par & 1) == 0)
            f |= PSR_P;
        if (v == 0)
            f |= PSR_Z;
        if (v & 0x80)
            f |= PSR_S;
        pzst[v] = (uint8)f;
    }
}

void z80prog_init_ram(uint8 *ram)
{
    int32 i;

    memset(ram, 0, Z80PROG_RAM_SIZE);
    for (i = 0; i < OBJ_COUNT; i++) {
        uint8 *obj = ram + OBJ_BASE + i * OBJ_STRIDE;
        obj[0] = (uint8)(i * 8);        /* x */
        obj[1] = (uint8)(i * 4);        /* y */
        obj[2] = (uint8)(i + 1);        /* dx */
        obj[3] = (uint8)(2 * i + 1);    /* dy */
        obj[4] = OBJ_ANIM0;             /* animation counter */
    }
    for (i = 0; i < 32; i++)
        ram[0x0400 + i] = (uint8)(i * 7 + 3);
}

/*
 * The run stops anywhere outside the object update routine, so the objects
 * already updated in the current pass have one update more than the
 * completed passes, and they come first. y and the animation counter are
 * exact modulo 256; x must stay below $F0 and cannot stay at 0 everywhere.
 */
int32 z80prog_check(const uint8 *ram)
{
    uint32 passes = ram[0x0000];
    uint32 prev_extra = 1;
    uint32 x_or = 0;
    int32 i;

    for (i = 0; i < 32; i++) {
        if (ram[0x0600 + i] != ram[0x0400 + i])
            return 0;
    }
    if (passes >= 7 && ram[0x0700] != 0x55)
        return 0;

    for (i = 0; i < OBJ_COUNT; i++) {
        const uint8 *obj = ram + OBJ_BASE + i * OBJ_STRIDE;
        uint32 updates = (uint32)(uint8)(OBJ_ANIM0 - obj[4]);
        uint32 extra = (uint32)(uint8)(updates - passes);

        if (extra > 1 || extra > prev_extra)
            return 0;
        prev_extra = extra;
        if (obj[0] >= 0xF0)
            return 0;
        x_or |= obj[0];
        if (obj[1] != (uint8)(i * 4 + updates * (uint32)(2 * i + 1)))
            return 0;
    }
    return x_or != 0;
}
