#ifndef BENCH_CPU_H
#define BENCH_CPU_H

/*
 * CPU micro-benchmarks: raw ARM60 throughput and the cost of emulating a
 * Z80 with an interpreter following the SMSAdvance scheme (bench_z80.s)
 * and with the translator (z80jit.c), on a synthetic program.
 */

#include "types.h"

/*
 * Interpreter context. CONTRACT with the MAP in bench_z80.s: the assembly
 * addresses every field relative to a global pointer equal to &opz[0], so
 * the memory map lives at negative offsets. Keep both in sync.
 */
typedef struct {
    uint32 memmap[64];   /* 1 KiB pages, reversed: page p is memmap[63 - p]; */
                         /* entry = host address of the page - Z80 address */
    uint32 opz[256];     /* main opcode handlers */
    uint32 ddz[256];     /* handlers for DD (IX) prefixed opcodes */
    uint8  pzst[256];    /* sign / zero / parity flags of a byte */
    uint32 writemem[8];  /* write handlers, one per 8 KiB region */
    uint32 regs[8];      /* F A BC DE HL cycles PC SP (see bench_z80.s) */
    uint32 ix;           /* IX in bits 16-31 */
    uint32 iy;           /* IY in bits 16-31 */
    uint32 lastbank;     /* memmap entry of the page holding PC */
    uint32 nexttimeout;  /* handler called when a scanline is used up */
    uint32 ram_c000;     /* host RAM - $C000 */
    uint32 ram_e000;     /* host RAM - $E000 (mirror) */
    uint32 lines_left;   /* scanlines still to run */
    uint32 stop_op;      /* opcode that stopped the run (unimplemented) */
} z80b_ctx;

/* Implemented in bench_z80.s. */
void  z80b_setup(z80b_ctx *ctx);
int32 z80b_run(z80b_ctx *ctx, int32 lines);
void  arm_loop_alu(int32 iterations);
void  arm_loop_ldr(const uint32 *table, int32 iterations);
void  arm_fill_words(uint32 *dst, int32 blocks32, uint32 value);

/* Timing of one Z80 core on the benchmark program. */
typedef struct {
    uint32 frames;          /* NTSC frames in the timed run */
    uint32 total_us;        /* time of the timed run */
    uint32 frame_us;        /* per NTSC frame (262 lines) */
    uint32 pal_frame_us;    /* per PAL frame (313 lines) */
    uint32 first_us;        /* first frame (includes translation) */
    int32  status;          /* 0 ok, else the run stopped early */
    uint32 stop_op;         /* opcode that stopped it */
    uint32 stop_pc;
    int32  check;           /* 1 when the program results are consistent */
} bench_z80_result;

typedef struct {
    uint32 alu_ns;          /* ns per iteration of a SUBS/BNE loop */
    uint32 ldr_ns;          /* ns per iteration of an 8 x LDR loop */
    bench_z80_result interp;
    bench_z80_result jit;
    uint32 jit_blocks;      /* translator statistics on the program */
    uint32 jit_code_bytes;
} bench_cpu_result;

Err  bench_cpu_run(bench_cpu_result *res);
void bench_cpu_log(const bench_cpu_result *res);

#endif /* BENCH_CPU_H */
