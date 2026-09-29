/*
 * Native test of the Z80 interpreter (src/z80int.c): runs a ZEXDOC or
 * ZEXALL SMS image with a minimal machine (64 KiB ROM image, 8 KiB of
 * RAM mirrored, the SDSC console on port $FD) and prints the console.
 * Every instruction is interpreted; nothing is translated.
 *
 * Build and run (from the repository root):
 *   gcc -O2 -I tools/z80int_test -I src -o tools/z80int_test/zex \
 *       tools/z80int_test/zex.c src/z80int.c
 *   tools/z80int_test/zex docs/sms_gg/ZEXALL-SMS-0.21/zexdoc.sms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "z80int.h"
#include "z80jit_int.h"

char *jit_base;

static uint32 host_in(z80j_machine *m, uint32 port, uint32 left)
{
    (void)m; (void)port; (void)left;
    return 0xFF;
}

static uint32 host_out(z80j_machine *m, uint32 port, uint32 value, uint32 left)
{
    (void)m; (void)left;
    if ((port & 0xFF) == 0xFD) {
        putchar((int)value);
        fflush(stdout);
    }
    return 0;
}

static uint32 host_write(z80j_machine *m, uint32 addr, uint32 value)
{
    (void)m; (void)addr; (void)value;
    return 0;                       /* ROM area: ignored */
}

static void host_event(z80j_machine *m)
{
    (void)m;
}

int main(int argc, char **argv)
{
    char *arena;
    z80j_ctx *ctx;
    z80j_machine *m;
    uint8 *rom;
    FILE *f;
    size_t n;
    uint32 p;
    uint32 v;
    z80i_params ip;
    unsigned long long insns = 0;
    unsigned long long runs = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: zex image.sms\n");
        return 1;
    }
    arena = malloc(sizeof(z80j_ctx) + 0x10000 + sizeof(z80j_machine) + 64);
    jit_base = arena - 4096;        /* offsets stay nonzero */
    ctx = (z80j_ctx *)arena;
    rom = (uint8 *)(arena + sizeof(z80j_ctx));
    m = (z80j_machine *)(rom + 0x10000);
    memset(ctx, 0, sizeof(*ctx));
    memset(rom, 0, 0x10000);
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    n = fread(rom, 1, 0x10000, f);
    fclose(f);
    printf("image: %lu bytes\n", (unsigned long)n);

    /* Flag tables, as z80j_init builds them. */
    for (v = 0; v < 256; v++) {
        uint32 par = v;
        uint32 fl = 0;
        uint32 e = 0;
        uint32 d = 0;

        par ^= par >> 4;
        par ^= par >> 2;
        par ^= par >> 1;
        if ((par & 1) == 0) fl |= PSR_P;
        if (v == 0) fl |= PSR_Z;
        if (v & 0x80) fl |= PSR_S;
        ctx->pzst[v] = (uint8)fl;
        if (v & PSR_S) e |= 0x80;
        if (v & PSR_Z) e |= 0x40;
        if (v & PSR_Y) e |= 0x20;
        if (v & PSR_H) e |= 0x10;
        if (v & PSR_X) e |= 0x08;
        if (v & PSR_V) e |= 0x04;
        if (v & PSR_n) e |= 0x02;
        if (v & PSR_C) e |= 0x01;
        ctx->fenc[v] = (uint8)e;
        if (v & 0x80) d |= PSR_S;
        if (v & 0x40) d |= PSR_Z;
        if (v & 0x20) d |= PSR_Y;
        if (v & 0x10) d |= PSR_H;
        if (v & 0x08) d |= PSR_X;
        if (v & 0x04) d |= PSR_V;
        if (v & 0x02) d |= PSR_n;
        if (v & 0x01) d |= PSR_C;
        ctx->fdec[v] = (uint8)d;
    }
    /* Pages: ROM image at $0000-$BFFF (read only), RAM at $C000 mirrored
     * at $E000. */
    for (p = 0; p < 0xC0; p++) {
        ctx->rtab[255 - p] = JIT_ADDR(rom);
        ctx->wtab[p] = 0;
    }
    for (p = 0xC0; p < 0x100; p++) {
        uint32 e = JIT_ADDR(ctx->mram) - ((p < 0xE0) ? 0xC000u : 0xE000u);
        ctx->rtab[255 - p] = e;
        ctx->wtab[p] = e;
    }
    for (p = 0; p < 0x10000; p++)
        ctx->lookup[p] = 1;             /* nothing translated: 1 = miss */
    m->in = host_in;
    m->out = host_out;
    m->write = host_write;
    m->event = host_event;
    m->user = 0;
    ctx->machine = JIT_ADDR(m);
    ctx->regs[1] = 0xFF000000u;
    ctx->regs[0] = ctx->fdec[0xFF];
    ctx->regs[7] = 0xFFFF0000u;
    ctx->ix = 0xFFFF0000u;
    ctx->iy = 0xFFFF0000u;
    ctx->regs[6] = 0;
    ctx->regs[5] = 0;
    ctx->line = 0;

    memset(&ip, 0, sizeof(ip));
    ip.miss = 1;
    for (;;) {
        uint32 r;

        ctx->regs[5] += 228u * 256u * 262u;    /* one frame of T-states */
        ctx->line += 262;
        r = z80i_run(ctx, &ip);
        runs++;
        if (r == Z80I_HALT) {
            printf("\nHALT at $%04lx after %llu runs\n", (unsigned long)ctx->regs[6], runs);
            break;
        }
        if (r == Z80I_IRQ_CHECK)
            ctx->regs[5] -= 228u * 256u * 262u;  /* no interrupts here: go on */
        if (runs > 4000000) {
            printf("\ntime limit\n");
            break;
        }
    }
    (void)insns;
    return 0;
}
