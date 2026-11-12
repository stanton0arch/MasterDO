/*
 * Independent timing reference for a Game Gear cartridge: the MAME-derived
 * Z80 core of SMS Plus 0.9 (its loop shortcuts disabled), 228 T-states per
 * line with the overshoot of each line carried into the next, 262 lines
 * per frame, the VDP's interrupt flags and line counter as SMS Plus's
 * vdp_run sets them at each line start, the Sega mapper (first 1 KiB fixed),
 * 8 KiB of RAM, the Game Gear ports and the pad of a recorded play.
 *
 * Probe (optional): the opcode fetches at two addresses of slot 1 with a
 * given bank mapped there, typically the entry and the wake of a game's
 * frame wait routine: prints "wake frame T" for the first wake of each
 * frame, T being the T-states since the last fetch at the entry address.
 * The frames where the game wakes give its update rhythm, to compare with
 * the emulator's on the same recorded play.
 *
 * Pictures: DUMP_FROM=a DUMP_TO=b [DUMP_DIR=d] write the LCD window of
 * frames a to b as ref_NNNNNN.ppm, each line drawn with the registers and
 * video RAM as they stand at its start.
 *
 * Timing shifts, to measure how much a course depends on small timing
 * details: VBL_LINE=n sets the line whose start raises the frame
 * interrupt (192 by default, as SMS Plus), START_T=n gives the Z80 n more
 * T-states in the first line.
 *
 * ggref image frames "pad script" [bank entry wake]
 * The pad script is the "frame:bits,..." list of a Pad script: line (bits
 * as the emulator's pad word: up 0x01, down 0x02, left 0x04, right 0x08,
 * buttons 0x10 and 0x20, START 0x100).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

#define LINE_T 228
#define LINES  262

static unsigned char *rom;
static unsigned rom_pages;
static unsigned char ram[0x2000];
static unsigned char sram[0x8000];
static unsigned char fcr[4] = { 0, 0, 1, 2 };
static unsigned char vram[0x4000], cram[0x40], reg[16], status, latch, pending, buffer, code;
static unsigned addr;
static unsigned char cram_latch;
static unsigned char screen[192][256];   /* colour indexes of the frame's lines */
static int vscroll;                      /* register 9, taken at the frame start */
static int dump_from = -1, dump_to = -1;
static const char *dump_dir = ".";
static int line, left, frame;
static unsigned pad, start;
static unsigned char ggp[7] = { 0xC0, 0x7F, 0xFF, 0x00, 0xFF, 0x00, 0xFF };
static long long t_line0;            /* T-states at the start of the current line */
static int line_target;              /* T-states asked of z80_execute for this line */
static unsigned probe_bank = 0xFFFF, probe_entry = 0x10000, probe_wake = 0x10000;
static long long t_entry = -1;
static int last_wake_frame = -1;

static long long now(void) { return t_line0 + (line_target - z80_ICount); }

static void irq_update(void)
{
    int on = ((status & 0x80) && (reg[1] & 0x20)) || ((status & 0x40) && (reg[0] & 0x10));
    z80_set_irq_line(0, on ? ASSERT_LINE : CLEAR_LINE);
}

static unsigned char *slot_ptr(unsigned a)
{
    if (a < 0x400) return rom + a;
    if (a < 0x4000) return rom + (fcr[1] % rom_pages) * 0x4000 + a;
    if (a < 0x8000) return rom + (fcr[2] % rom_pages) * 0x4000 + (a - 0x4000);
    if (a < 0xC000) {
        if (fcr[0] & 8) return sram + ((fcr[0] & 4) ? 0x4000 : 0) + (a - 0x8000);
        return rom + (fcr[3] % rom_pages) * 0x4000 + (a - 0x8000);
    }
    return ram + (a & 0x1FFF);
}

int cpu_readop(int a)
{
    a &= 0xFFFF;
    if (a >= 0x4000 && a < 0x8000 && (fcr[2] % rom_pages) == probe_bank) {
        if ((unsigned)a == probe_entry) t_entry = now();
        else if ((unsigned)a == probe_wake && frame != last_wake_frame) {
            printf("wake %d %lld\n", frame, t_entry >= 0 ? now() - t_entry : -1);
            last_wake_frame = frame;
        }
    }
    return *slot_ptr(a);
}
int cpu_readop_arg(int a) { return *slot_ptr(a & 0xFFFF); }
int cpu_readmem16(int a) { return *slot_ptr(a & 0xFFFF); }
void cpu_writemem16(int a, int d)
{
    a &= 0xFFFF;
    if (a >= 0xC000) {
        ram[a & 0x1FFF] = d;
        if (a >= 0xFFFC) fcr[a & 3] = d;
    } else if (a >= 0x8000 && (fcr[0] & 8)) {
        sram[((fcr[0] & 4) ? 0x4000 : 0) + (a - 0x8000)] = d;
    }
}

static unsigned vcount(void)
{
    return line <= 0xDA ? line : line - 6;      /* NTSC, 192 lines */
}

int cpu_readport(int p)
{
    p &= 0xFF;
    if (p < 7) {
        if (p == 0) return (start ? 0 : 0x80) | 0x40;
        return ggp[p];
    }
    switch (((p >> 5) & 6) | (p & 1)) {
    case 2: return vcount();
    case 3: return 0;
    case 4: { unsigned char t = buffer; buffer = vram[addr & 0x3FFF]; addr++; pending = 0; return t; }
    case 5: { unsigned char t = status; status = 0; pending = 0; irq_update(); return t; }
    case 6: return ~pad & 0xFF;
    case 7: return 0xFF;
    }
    return 0xFF;
}

void cpu_writeport(int p, int d)
{
    p &= 0xFF;
    if (p < 7) { if (p != 0 && p != 4) ggp[p] = d; return; }
    switch (((p >> 5) & 6) | (p & 1)) {
    case 4:
        pending = 0;
        if (code == 3) {
            /* Game Gear colour RAM: the even byte is latched, the odd one
             * writes both. */
            if (!(addr & 1)) cram_latch = d;
            else { cram[(addr - 1) & 0x3F] = cram_latch; cram[addr & 0x3F] = d; }
        } else {
            vram[addr & 0x3FFF] = d;
        }
        addr++;
        break;
    case 5:
        if (!pending) { latch = d; pending = 1; break; }
        pending = 0;
        code = (d >> 6) & 3;
        addr = (d << 8 | latch) & 0x3FFF;
        if (code == 0) { buffer = vram[addr]; addr++; }
        if (code == 2) { reg[d & 15] = latch; irq_update(); }
        break;
    }
}

void change_pc16(int pc) { (void)pc; }
void change_pc(int pc) { (void)pc; }
int cpu_getactivecpu(void) { return 0; }
static int irq_cb(int l) { (void)l; return 0xFF; }

/* One line of the mode 4 picture drawn with the registers and video RAM as
 * they stand at the start of the line: background with its scrolls, the
 * first 8 sprites of the line, background priority, display enable. */
static void draw_line(int y)
{
    unsigned char *o = screen[y];
    unsigned char spr[256];
    unsigned char bgp[256];
    int x, i, n = 0;
    int nt = (reg[2] & 0x0E) << 10;
    int satb = (reg[5] & 0x7E) << 7;
    int hs = ((reg[0] & 0x40) && y < 16) ? 0 : reg[8];
    int h = (reg[1] & 2) ? 16 : 8;
    int zoom = reg[1] & 1;

    if (!(reg[1] & 0x40)) {
        memset(o, 16 + (reg[7] & 15), 256);
        return;
    }
    for (x = 0; x < 256; x++) {
        int bx = (x - hs) & 255;
        int row = ((reg[0] & 0x80) && x >= 192) ? y : (y + vscroll) % 224;
        int e = nt + (((row >> 3) * 32 + (bx >> 3)) * 2);
        int entry = vram[e & 0x3FFF] | vram[(e + 1) & 0x3FFF] << 8;
        int tile = entry & 0x1FF;
        int r = (entry & 0x400) ? 7 - (row & 7) : (row & 7);
        int b = (entry & 0x200) ? (bx & 7) : 7 - (bx & 7);
        int a = tile * 32 + r * 4;
        int c = ((vram[a] >> b) & 1) | ((vram[a + 1] >> b) & 1) << 1 |
                ((vram[a + 2] >> b) & 1) << 2 | ((vram[a + 3] >> b) & 1) << 3;
        o[x] = ((entry & 0x800) ? 16 : 0) + c;
        bgp[x] = (entry & 0x1000) && c != 0;
    }
    memset(spr, 0, sizeof(spr));
    for (i = 0; i < 64; i++) {
        int sy = vram[satb + i];
        int d, sx, tile, px, a, w;
        if (sy == 0xD0) break;
        if (sy > 0xD0) sy -= 256;
        d = y - (sy + 1);
        if (d < 0 || d >= (h << zoom)) continue;
        if (++n > 8) break;
        d >>= zoom;
        sx = vram[satb + 0x80 + 2 * i] - ((reg[0] & 8) ? 8 : 0);
        tile = vram[satb + 0x81 + 2 * i] | ((reg[6] & 4) << 6);
        if (h == 16) tile = (tile & ~1) + (d >> 3);
        a = tile * 32 + (d & 7) * 4;
        w = 8 << zoom;
        for (px = 0; px < w; px++) {
            int xx = sx + px, b = 7 - (px >> zoom), c;
            if (xx < 0 || xx > 255 || spr[xx]) continue;
            c = ((vram[a] >> b) & 1) | ((vram[a + 1] >> b) & 1) << 1 |
                ((vram[a + 2] >> b) & 1) << 2 | ((vram[a + 3] >> b) & 1) << 3;
            if (c == 0) continue;
            spr[xx] = 16 + c;
        }
    }
    for (x = 0; x < 256; x++)
        if (spr[x] && !bgp[x]) o[x] = spr[x];
    if (reg[0] & 0x20)
        memset(o, 16 + (reg[7] & 15), 8);
}

/* The Game Gear's LCD window (160 x 144 from 48,24) as a PPM file. */
static void dump_frame(int f)
{
    char name[512];
    FILE *o;
    int x, y;

    sprintf(name, "%s/ref_%06d.ppm", dump_dir, f);
    o = fopen(name, "wb");
    if (!o) return;
    fprintf(o, "P6\n160 144\n255\n");
    for (y = 24; y < 168; y++)
        for (x = 48; x < 208; x++) {
            int c = screen[y][x];
            unsigned char rgb[3];
            rgb[0] = (cram[2 * c] & 15) * 17;
            rgb[1] = (cram[2 * c] >> 4) * 17;
            rgb[2] = (cram[2 * c + 1] & 15) * 17;
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
}

/* SMS Plus's vdp_run, at the start of each line. */
static int vbl_line = 0xC0;

static void line_start(void)
{
    if (line <= vbl_line) {
        if (line == vbl_line) status |= 0x80;
        if (line == 0) left = reg[10];
        if (left == 0) { left = reg[10]; status |= 0x40; }
        else left--;
    } else {
        left = reg[10];
    }
    irq_update();
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    int frames, npad = 0, k;
    static unsigned pf[4096], pb[4096];
    int carry = getenv("START_T") ? atoi(getenv("START_T")) : 0;

    if (argc < 4) { fprintf(stderr, "ggref image frames \"pad script\" [bank entry wake]\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    rom = calloc(1, 0x100000);
    n = fread(rom, 1, 0x100000, f);
    fclose(f);
    rom_pages = (unsigned)(n / 0x4000);
    frames = atoi(argv[2]);
    {
        char *p = argv[3];
        while (*p && npad < 4096) {
            if (sscanf(p, "%u:%x", &pf[npad], &pb[npad]) != 2) break;
            npad++;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    if (argc >= 7) {
        probe_bank = strtoul(argv[4], 0, 0);
        probe_entry = strtoul(argv[5], 0, 0);
        probe_wake = strtoul(argv[6], 0, 0);
    }
    if (getenv("VBL_LINE")) vbl_line = atoi(getenv("VBL_LINE"));
    if (getenv("DUMP_FROM")) dump_from = atoi(getenv("DUMP_FROM"));
    if (getenv("DUMP_TO")) dump_to = atoi(getenv("DUMP_TO"));
    if (getenv("DUMP_DIR")) dump_dir = getenv("DUMP_DIR");
    z80_reset(0);
    z80_set_irq_callback(irq_cb);
    k = 0;
    for (frame = 0; frame < frames; frame++) {
        while (k < npad && (int)pf[k] <= frame) {
            pad = pb[k] & 0x3F;
            start = (pb[k] & 0x100) != 0;
            k++;
        }
        for (line = 0; line < LINES; line++) {
            int done;
            line_start();
            if (line == 0) vscroll = reg[9];
            if (line < 192 && frame >= dump_from && frame <= dump_to) draw_line(line);
            line_target = LINE_T + carry;
            done = z80_execute(line_target);
            carry = line_target - done;
            t_line0 += done;
        }
        if (frame >= dump_from && frame <= dump_to) dump_frame(frame);
    }
    fprintf(stderr, "ggref: %d frames, %lld T-states (%.3f per frame)\n", frames, t_line0,
            (double)t_line0 / frames);
    return 0;
}
