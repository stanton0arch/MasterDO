/*
 * Native reference run of a Master System cartridge on the C interpreter
 * (z80int.c), one instruction at a time, with the maskable interrupt
 * taken at the first instruction boundary where it is pending, as the
 * Z80 does. The emulator takes interrupts at the boundary of a segment
 * of translated code instead (up to about 200 T-states late); this run
 * tells whether a game's timing depends on that.
 *
 * Machine: Sega mapper, 8 KiB of RAM mirrored, the VDP's ports, flags,
 * line counter and V counter (192-line mode, NTSC) as vdp.c models them,
 * no input. It prints, for each frame with some, the register 8 writes
 * made during the active display with the first line they affect (the
 * format of the cycle model's -rasterlog), and the line of every
 * interrupt.
 *
 * Build and run (from the repository root):
 *   gcc -O2 -fno-strict-aliasing -I tools/z80int_test -I src -o tools/z80int_test/smsref \
 *       tools/z80int_test/smsref.c tools/z80int_test/z80int.c
 *   tools/z80int_test/smsref takeme/roms/outrun.sms 850
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "z80int.h"
#include "z80jit_int.h"

char *jit_base;

#define LINE_T      228
#define LINES       262
#define FRAME_T     (LINE_T * LINES)
#define ACTIVE      192

static z80j_ctx *ctx;
static uint8 *rom;
static uint32 rom_banks;
static unsigned long long now;      /* T-states since the start */
static unsigned long long frame0;   /* start of the current frame */
static unsigned long long call0;    /* start of the instruction being run */
static uint32 frame;

/* VDP */
static uint8 vreg[16] = { 0x36, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF };
static uint8 vram[0x4000];
static uint8 cram[32];
static uint32 vaddr, vcode, vlatch, vpending, vbuffer;
static uint32 vstatus, line_flag, counter;
static uint32 lines_done;           /* lines of the frame whose end was processed */
static uint32 log_line[256], log_val[256], nlog;
static uint32 irq_lines[256], nirq;

static uint32 line_of(unsigned long long t)
{
    return (uint32)((t - frame0) / LINE_T);
}

/* Line ends up to time t: the counter and the VBlank flag. */
static void vdp_catch_up(unsigned long long t)
{
    uint32 k = line_of(t);

    while (lines_done < k && lines_done < LINES) {
        uint32 l = lines_done;      /* end of line l */

        if (l <= ACTIVE) {
            if (counter == 0) {
                counter = vreg[10];
                line_flag = 1;
            } else {
                counter--;
            }
        } else {
            counter = vreg[10];
        }
        lines_done++;
        if (lines_done == ACTIVE)
            vstatus |= 0x80;
    }
}

static uint32 irq_pending(void)
{
    return ((vstatus & 0x80) && (vreg[1] & 0x20)) || (line_flag && (vreg[0] & 0x10));
}

/* Time of a port access: the end of the instruction. */
static unsigned long long access_time(uint32 left)
{
    return call0 + (23u * 256u - left) / 256u;
}

/* Sega mapper: slot s shows bank b. */
static void map_slot(uint32 s, uint32 b)
{
    uint32 p;

    b %= rom_banks;
    for (p = s * 0x40; p < s * 0x40 + 0x40; p++) {
        if (s == 0 && p < 4)
            continue;               /* the first 1 KiB stays on bank 0 */
        ctx->rtab[255 - p] = JIT_ADDR(rom + b * 0x4000) - s * 0x4000;
    }
}

static uint32 host_in(z80j_machine *m, uint32 port, uint32 left)
{
    unsigned long long t = access_time(left);
    uint32 r;

    (void)m;
    vdp_catch_up(t);
    switch (((port >> 5) & 6) | (port & 1)) {
    case 2: {                       /* V counter */
        uint32 l = line_of(t);
        return (l > 0xDA) ? l - 6 : l;
    }
    case 3:
        return 0;                   /* H counter: not modelled */
    case 4:
        r = vbuffer;
        vbuffer = vram[vaddr];
        vaddr = (vaddr + 1) & 0x3FFF;
        vpending = 0;
        return r;
    case 5:
        r = vstatus;
        vstatus = 0;
        line_flag = 0;
        vpending = 0;
        return r;
    default:
        return 0xFF;
    }
}

static uint32 host_out(z80j_machine *m, uint32 port, uint32 value, uint32 left)
{
    unsigned long long t = access_time(left);

    (void)m;
    vdp_catch_up(t);
    value &= 0xFF;
    switch (((port >> 5) & 6) | (port & 1)) {
    case 4:
        vpending = 0;
        vbuffer = value;
        if (vcode == 3)
            cram[vaddr & 31] = (uint8)value;
        else
            vram[vaddr] = (uint8)value;
        vaddr = (vaddr + 1) & 0x3FFF;
        return 0;
    case 5:
        if (!vpending) {
            vlatch = value;
            vaddr = (vaddr & 0x3F00) | value;
            vpending = 1;
            return 0;
        }
        vpending = 0;
        vcode = value >> 6;
        vaddr = ((value & 0x3F) << 8) | vlatch;
        if (vcode == 0) {
            vbuffer = vram[vaddr];
            vaddr = (vaddr + 1) & 0x3FFF;
        } else if (vcode == 2 && (value & 0x0F) <= 10) {
            uint32 r = value & 0x0F;

            vreg[r] = (uint8)vlatch;
            if (r == 8) {
                uint32 l = line_of(t) + 1;

                if (l < ACTIVE && nlog < 256) {
                    log_line[nlog] = l;
                    log_val[nlog++] = vlatch;
                }
            }
        }
        return 0;
    default:
        return 0;
    }
}

static uint32 host_write(z80j_machine *m, uint32 addr, uint32 value)
{
    (void)m;
    if (addr >= 0xE000) {
        ctx->mram[addr - 0xE000] = (uint8)value;
        if (addr >= 0xFFFD)
            map_slot(addr - 0xFFFD, value);
    }
    return 0;                       /* ROM area: ignored */
}

static void host_event(z80j_machine *m)
{
    (void)m;
}

static void push16(uint32 v)
{
    uint32 sp = (ctx->regs[7] >> 16) - 2;
    uint32 a;

    for (a = 0; a < 2; a++) {
        uint32 adr = (sp + a) & 0xFFFF;
        uint32 byte = a ? (v >> 8) : (v & 0xFF);

        if (adr >= 0xC000)
            ctx->mram[adr & 0x1FFF] = (uint8)byte;
        if (adr >= 0xFFFD)
            map_slot(adr - 0xFFFD, byte);
    }
    ctx->regs[7] = (sp & 0xFFFF) << 16;
}

static void end_frame(void)
{
    uint32 k;

    if (nlog != 0) {
        printf("Raster: frame %lu: scroll end %lu:", (unsigned long)(frame + 1),
               (unsigned long)vreg[8]);
        for (k = 0; k < nlog; k++)
            printf(" %lu@%lu", (unsigned long)log_val[k], (unsigned long)log_line[k]);
        printf("\n");
    }
    printf("Irq: frame %lu:", (unsigned long)(frame + 1));
    for (k = 0; k < nirq && k < 8; k++)
        printf(" %lu", (unsigned long)irq_lines[k]);
    printf(" (%lu)\n", (unsigned long)nirq);
    nlog = 0;
    nirq = 0;
}

int main(int argc, char **argv)
{
    char *arena;
    z80j_machine *m;
    FILE *f;
    long size;
    uint32 p;
    uint32 v;
    uint32 frames;
    uint32 ei_wait = 0;
    z80i_params ip;

    if (argc < 3) {
        fprintf(stderr, "usage: smsref image.sms frames\n");
        return 1;
    }
    frames = (uint32)atoi(argv[2]);
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, size % 0x4000 == 512 ? 512 : 0, SEEK_SET);
    size -= size % 0x4000;
    arena = malloc(sizeof(z80j_ctx) + (size_t)size + sizeof(z80j_machine) + 64);
    jit_base = arena - 4096;
    ctx = (z80j_ctx *)arena;
    rom = (uint8 *)(arena + sizeof(z80j_ctx));
    m = (z80j_machine *)(rom + size);
    memset(ctx, 0, sizeof(*ctx));
    if (fread(rom, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    rom_banks = (uint32)(size / 0x4000);

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
    /* Pages: banks 0, 1, 2 in the slots, RAM at $C000 mirrored at $E000;
     * the page of the paging registers goes through host_write. */
    for (p = 0; p < 0xC0; p++) {
        ctx->rtab[255 - p] = JIT_ADDR(rom + (p >> 6) * 0x4000) - (p >> 6) * 0x4000;
        ctx->wtab[p] = 0;
    }
    for (p = 0xC0; p < 0x100; p++) {
        uint32 e = JIT_ADDR(ctx->mram) - ((p < 0xE0) ? 0xC000u : 0xE000u);

        ctx->rtab[255 - p] = e;
        ctx->wtab[p] = (p == 0xFF) ? 0 : e;
    }
    for (p = 0; p < 0x10000; p++)
        ctx->lookup[p] = 1;
    ctx->mram[0] = 0xAB;            /* as the BIOS leaves it */
    m->in = host_in;
    m->out = host_out;
    m->write = host_write;
    m->event = host_event;
    m->user = 0;
    ctx->machine = JIT_ADDR(m);
    ctx->regs[1] = 0xFF000000u;
    ctx->regs[0] = ctx->fdec[0xFF];
    ctx->regs[7] = 0xDFF0u << 16;
    ctx->ix = 0xFFFF0000u;
    ctx->iy = 0xFFFF0000u;
    ctx->regs[6] = 0;
    ctx->line = 0;
    counter = vreg[10];

    memset(&ip, 0, sizeof(ip));
    ip.miss = 1;
    while (frame < frames) {
        uint32 op;
        uint32 r;

        if (now >= frame0 + FRAME_T) {
            vdp_catch_up(frame0 + FRAME_T);
            end_frame();
            frame0 += FRAME_T;
            frame++;
            lines_done = 0;
        }
        vdp_catch_up(now);
        if (!ei_wait && ctx->iff1 && irq_pending()) {
            uint32 pc = ctx->regs[6] & 0xFFFF;

            if (ctx->halted) {
                ctx->halted = 0;
                pc = (pc + 1) & 0xFFFF;
            }
            push16(pc);
            ctx->regs[6] = 0x38;
            ctx->iff1 = ctx->iff2 = 0;
            now += 13;
            if (nirq < 256)
                irq_lines[nirq++] = line_of(now);
            continue;
        }
        if (ctx->halted) {
            now += 4;
            continue;
        }
        /* Page entry and address added in 32 bits, as the interpreter does. */
        op = *(const uint8 *)JIT_PTR(ctx->rtab[255 - ((ctx->regs[6] >> 8) & 0xFF)] +
                                     (ctx->regs[6] & 0xFFFF));
        ei_wait = (op == 0xFB);
        call0 = now;
        ctx->regs[5] = 23u * 256u;
        ctx->line = (uint32)((now + 23) / LINE_T + 1);
        r = z80i_run(ctx, &ip);
        if (r == Z80I_HALT) {
            now += 4;
            ctx->regs[6] = (ctx->regs[6] - 1) & 0xFFFF;   /* stay on the HALT */
            ctx->halted = 1;
        } else {
            now += (23u * 256u - ctx->regs[5]) / 256u;
        }
    }
    return 0;
}
