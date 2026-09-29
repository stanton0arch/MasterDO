/*
 * CPU micro-benchmarks.
 *
 * 1. Raw ARM60 throughput: an ALU loop and a load loop, to compare the
 *    effective clock and memory speed between an emulator and the console.
 * 2. Z80 emulation: the program of z80prog.c runs for a whole second of
 *    Z80 time, first on the interpreter of bench_z80.s, then on the
 *    translator of z80jit.c with the memory of an SMS cartridge
 *    (smsmem.c). The RAM left by each run is checked against the program
 *    semantics.
 *
 * The cartridge itself runs in game.c.
 */

#include "bench_cpu.h"
#include "z80prog.h"
#include "z80jit.h"
#include "smsmem.h"
#include "platform.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"

#define LINES_NTSC 262
#define LINES_PAL  313
#define TIMED_FRAMES 60

#define ALU_ITERATIONS 200000
#define LDR_ITERATIONS 100000

#define JIT_CODE_BYTES (256 * 1024)
#define JIT_BLOCKS     2048

static const uint32 ldr_table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void fill_timing(bench_z80_result *r, uint32 t0, uint32 t1)
{
    r->frames = TIMED_FRAMES;
    r->total_us = t1 - t0;
    r->frame_us = r->total_us / TIMED_FRAMES;
    r->pal_frame_us = r->total_us * LINES_PAL / (LINES_NTSC * TIMED_FRAMES);
}

/*--------------------------------------------------------------------------
 * Interpreter
 *------------------------------------------------------------------------*/

static Err bench_interp(bench_z80_result *r, uint8 *cart)
{
    z80b_ctx *ctx;
    uint8 *ram;
    uint32 t0;
    uint32 t1;
    int32 status;
    int32 i;

    ctx = (z80b_ctx *)AllocMem(sizeof(z80b_ctx), MEMTYPE_DRAM | MEMTYPE_FILL);
    ram = (uint8 *)AllocMem(Z80PROG_RAM_SIZE, MEMTYPE_DRAM | MEMTYPE_FILL);
    if (ctx == NULL || ram == NULL) {
        printf("ERROR: bench_interp out of memory\n");
        if (ctx != NULL) FreeMem(ctx, sizeof(z80b_ctx));
        if (ram != NULL) FreeMem(ram, Z80PROG_RAM_SIZE);
        return -1;
    }

    z80prog_init_ram(ram);
    z80prog_fill_memmap(ctx->memmap, (uint32)cart, (uint32)ram);
    z80prog_fill_pzst(ctx->pzst);
    ctx->regs[7] = (uint32)Z80PROG_SP << 16;
    ctx->ram_c000 = (uint32)ram - 0xC000;
    ctx->ram_e000 = (uint32)ram - 0xE000;
    z80b_setup(ctx);

    /* One frame first, then a whole second of Z80 time. */
    t0 = plat_usec_now();
    status = z80b_run(ctx, LINES_NTSC);
    r->first_us = plat_usec_now() - t0;
    if (status == 0) {
        t0 = plat_usec_now();
        status = z80b_run(ctx, LINES_NTSC * TIMED_FRAMES);
        t1 = plat_usec_now();
        fill_timing(r, t0, t1);
    }
    /* The run ends at an arbitrary instruction: step line by line until PC
     * is outside the object update routine so that the RAM is consistent. */
    for (i = 0; status == 0 && i < 64 && ctx->regs[6] >= Z80PROG_SAFE_PC; i++)
        status = z80b_run(ctx, 1);

    r->status = status;
    r->stop_op = ctx->stop_op;
    r->stop_pc = ctx->regs[6];
    r->check = (status == 0) ? z80prog_check(ram) : 0;

    FreeMem(ram, Z80PROG_RAM_SIZE);
    FreeMem(ctx, sizeof(z80b_ctx));
    return 0;
}

/*--------------------------------------------------------------------------
 * Translator
 *------------------------------------------------------------------------*/

typedef struct {
    z80j_ctx  *ctx;
    uint32    *code;
    void      *blocks;
    z80j_state state;
    sms_mem    mem;
} jit_env;

static void jit_env_free(jit_env *e)
{
    if (e->blocks != NULL) FreeMem(e->blocks, (int32)z80j_block_bytes(JIT_BLOCKS));
    if (e->code != NULL) FreeMem(e->code, JIT_CODE_BYTES);
    if (e->ctx != NULL) FreeMem(e->ctx, sizeof(z80j_ctx));
}

static Err jit_env_alloc(jit_env *e)
{
    z80j_glue glue;

    memset(e, 0, sizeof(*e));
    e->ctx = (z80j_ctx *)AllocMem(sizeof(z80j_ctx), MEMTYPE_DRAM | MEMTYPE_FILL);
    e->code = (uint32 *)AllocMem(JIT_CODE_BYTES, MEMTYPE_DRAM);
    e->blocks = AllocMem((int32)z80j_block_bytes(JIT_BLOCKS), MEMTYPE_DRAM);
    if (e->ctx == NULL || e->code == NULL || e->blocks == NULL) {
        printf("ERROR: translator out of memory (%lu + %lu + %lu bytes)\n",
               (unsigned long)sizeof(z80j_ctx), (unsigned long)JIT_CODE_BYTES,
               (unsigned long)z80j_block_bytes(JIT_BLOCKS));
        jit_env_free(e);
        return -1;
    }
    z80j_default_glue(&glue);
    z80j_init(&e->state, e->ctx, e->code, JIT_CODE_BYTES / 4, e->blocks, JIT_BLOCKS, &glue);
    return 0;
}

static void bench_jit(bench_cpu_result *res, jit_env *e, uint8 *cart)
{
    bench_z80_result *r = &res->jit;
    z80j_ctx *ctx = e->ctx;
    uint32 t0;
    uint32 t1;
    int32 i;

    z80j_flush(&e->state);
    sms_mem_init(&e->mem, ctx, cart, Z80PROG_CART_SIZE, 0);
    z80j_reset(ctx);
    z80prog_init_ram(ctx->mram);

    /* The first frame translates the program; it is timed apart. */
    t0 = plat_usec_now();
    z80j_run(ctx, LINES_NTSC);
    r->first_us = plat_usec_now() - t0;
    if (ctx->exit_reason == Z80J_EXIT_LINES) {
        t0 = plat_usec_now();
        z80j_run(ctx, LINES_NTSC * TIMED_FRAMES);
        t1 = plat_usec_now();
        fill_timing(r, t0, t1);
    }
    for (i = 0; ctx->exit_reason == Z80J_EXIT_LINES && i < 64 &&
                ctx->regs[6] >= Z80PROG_SAFE_PC; i++)
        z80j_run(ctx, 1);

    r->status = (int32)ctx->exit_reason;
    r->stop_op = ctx->exit_arg;
    r->stop_pc = ctx->regs[6];
    r->check = (ctx->exit_reason == Z80J_EXIT_LINES) ? z80prog_check(ctx->mram) : 0;
    res->jit_blocks = e->state.stats.translations;
    res->jit_code_bytes = e->state.stats.code_bytes;
}

/* Kept off the small task stack (the block hash alone is 4 KiB). */
static jit_env env;

Err bench_cpu_run(bench_cpu_result *res)
{
    uint8 *cart;
    uint32 t0;
    uint32 t1;

    memset(res, 0, sizeof(*res));

    /* Raw ARM throughput. */
    t0 = plat_usec_now();
    arm_loop_alu(ALU_ITERATIONS);
    t1 = plat_usec_now();
    res->alu_ns = (t1 - t0) * 1000u / ALU_ITERATIONS;

    t0 = plat_usec_now();
    arm_loop_ldr(ldr_table, LDR_ITERATIONS);
    t1 = plat_usec_now();
    res->ldr_ns = (t1 - t0) * 1000u / LDR_ITERATIONS;

    res->interp.status = -1;
    res->jit.status = -1;

    cart = (uint8 *)AllocMem(Z80PROG_CART_SIZE, MEMTYPE_DRAM | MEMTYPE_FILL);
    if (cart == NULL) {
        printf("ERROR: bench_cpu out of memory\n");
        return -1;
    }
    memcpy(cart, z80prog_code, (size_t)z80prog_size);
    bench_interp(&res->interp, cart);

    if (jit_env_alloc(&env) == 0) {
        bench_jit(res, &env, cart);
        jit_env_free(&env);
    }
    FreeMem(cart, Z80PROG_CART_SIZE);
    return 0;
}

static void log_core(const char *name, const bench_z80_result *r)
{
    if (r->status != 0) {
        printf("%s: run stopped (reason %ld), argument $%lx at PC $%04lx\n", name,
               (long)r->status, (unsigned long)r->stop_op, (unsigned long)r->stop_pc);
        return;
    }
    printf("%s: %lu frames in %lu us -> %lu us per NTSC frame (%lu%% of %d us), "
           "%lu us per PAL frame (%lu%% of %d us), first frame %lu us, result check %s\n",
           name, (unsigned long)r->frames, (unsigned long)r->total_us,
           (unsigned long)r->frame_us,
           (unsigned long)(r->frame_us * 100u / PLAT_NTSC_FRAME_US), PLAT_NTSC_FRAME_US,
           (unsigned long)r->pal_frame_us,
           (unsigned long)(r->pal_frame_us * 100u / PLAT_PAL_FRAME_US), PLAT_PAL_FRAME_US,
           (unsigned long)r->first_us, r->check ? "OK" : "FAILED");
}

void bench_cpu_log(const bench_cpu_result *res)
{
    printf("CPU: ALU loop %lu ns/iteration (16 ALU + 1 branch), LDR loop %lu ns/iteration (8 LDR + 2)\n",
           (unsigned long)res->alu_ns, (unsigned long)res->ldr_ns);
    log_core("Z80 interpreter", &res->interp);
    log_core("Z80 translator", &res->jit);
    printf("Z80 translator: program in %lu blocks, %lu bytes of ARM code\n",
           (unsigned long)res->jit_blocks, (unsigned long)res->jit_code_bytes);
}
