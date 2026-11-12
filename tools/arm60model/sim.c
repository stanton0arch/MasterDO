/*
 * ARM60 cycle model: an ARMv3 big-endian interpreter that counts S, N and
 * I cycles per instruction with the timings of the ARM60 datasheet, runs a
 * plain binary linked by armlink at 0x8000, and provides the host services
 * of hstub.s (printf, memory, clock, ROM, picture dumps).
 *
 * Time model (fitted on Phoenix in step 1): S = 1.05, N = 3.2, I = 1
 * cycles of 80 ns. Not modelled: DRAM page effects beyond the fit, the
 * interrupts of the 3DO OS, the CEL engine.
 *
 * Host services (SWI 0xF000nn, see hstub.s): 0 exit, 1 printf, 2 allocate
 * (counted against the memory left to the run: -dramfree n, default the
 * DRAM Phoenix leaves to the console's executable of the same build, see
 * dram_from_aif; the VRAM less the image), 3 free (counted back),
 * 4 microsecond clock, 5 ROM address, 6 ROM size,
 * 7 picture dump (view_NNNNNN.ppm), 8 command-line argument, 9 pad
 * buttons of a frame, 10 time marks (15: reset the profile), 11 code
 * buffer dump (code.bin), 13 free memory (plat_mem_free), 15 the
 * kernel's system time (SampleSystemTimeTV: the TimeVal at r0, the call
 * charged as SIMCLOCK microseconds of I cycles, half before the sample
 * and half after), 16 the microsecond clock without charge (sim_time).
 *
 * Options: -rom file, -frames n, -bench, -dump n, -from n, -img, -sym,
 * -aif file (the AIF header of the console's executable, default sim.aif
 * beside the image), -dramfree n (overrides the figure derived from it).
 * Environment: SIMPAD="frame:bits,..." (buttons held from that frame on,
 * SMS_PAD_* bits, 0x100 PAUSE), SIMPROF=1 (cycles per function, the 40
 * largest; SIMPROFALL=1 every function),
 * SIMALLOC=1 (every allocation with its size and memory type), SIMSCALE=f
 * (every cycle weight multiplied by f: another host timing),
 * SIMDIS=symbol[,symbol...] (executions and cycles per instruction of
 * functions; with several, each listing starts with a "dis name" line),
 * SIMBLOCKLOG=file (the block descriptors scanned at every frame, see
 * block_log), SIMCLOCK=us (cost of a kernel call of the system time,
 * default 26: 1 000 calls in 26 064 us on Phoenix; scaled by SIMSCALE
 * like every cycle), SIMTRLOG=file (the cost of every block translation,
 * pass by pass and instruction by instruction, see tr_flush; needs an
 * image built with mk.sh -labels; SIMTRWORDS=1 adds the words emitted
 * for each instruction).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <signal.h>

#define MEM_SIZE   0x01000000u
#define LOAD_ADDR  0x8000u
#define ROM_ADDR   0x00C00000u
#define HEAP_BASE  0x00100000u
#define HEAP_END   0x00B00000u
#define STACK_TOP  0x00FF0000u

static uint8_t *mem;
static uint32_t r[16];
static uint32_t fn, fz, fc, fv;
static uint64_t cS, cN, cI;
static uint64_t ninsn;
static uint32_t heap = HEAP_BASE;
static uint32_t rom_size;
/* Memory of the console left to the cartridge's run (Phoenix, NTSC, the
 * ISO's own code and the system loaded): the allocations are counted
 * against it, an allocation past it fails as AllocMem does, and the
 * image takes its size of VRAM. The DRAM is derived from the size of the
 * console's executable (dram_from_aif); without its header, the figure
 * Phoenix gave last. */
static uint32_t dram_free = 1283456;
static uint32_t vram_free = 571392;
static int dram_given;
/* DRAM Phoenix leaves free before the cartridge, plus what the console's
 * executable takes there: its read-only and read-write parts rounded up to
 * 16 bytes, and its zero-initialised part rounded up to 16 bytes. Fitted
 * to the byte on five builds of the emulator whose Phoenix logs give the
 * free DRAM (1 283 456 to 1 289 632 bytes for executables of 205 152 to
 * 211 328 bytes). */
#define DRAM_PHOENIX 1494784u
static struct { uint32_t a, size, vram; } allocs[256];
static uint32_t nallocs;
static uint32_t args[32] = { 0, 0, 0, 0xFFFFFFFFu };
static int done;

/* Profile: cycles (x100) per instruction word of the image. */
static uint32_t img_end;
static double *prof;
static uint32_t *pcount;
static double prof_jit;
static uint8_t *execmap;   /* one byte per word of memory above the image: executed */
static float *jitcyc;      /* cycles per word of memory outside the image */

typedef struct { uint32_t addr; char name[64]; } sym_t;
static sym_t *syms;
static int nsyms;

static double weightS = 1.05, weightN = 3.2, weightI = 1.0;
/* Half the cost of a kernel call of the system time, in I cycles. */
static uint64_t clock_half = 163;

static double cycles(void) { return cS * weightS + cN * weightN + cI * weightI; }
static uint32_t usec_now(void) { return (uint32_t)(cycles() * 0.08); }

/* Translation trace (SIMTRLOG). Time stamps are taken when the program
 * reaches given labels of the translator (built with every label exported:
 * mk.sh -labels): the entries and returns of timed_translate (a translation
 * at once, the clock looks included), jit_translate_block and its three
 * passes, the per-instruction loop heads of the decoder (db_loop), of the
 * backward walk of the analysis (p3_loop) and of the emitter (eb_loop), the
 * stub loop (eb_stub_loop) and the ends of those loops. The span between
 * two stamps is charged to the instruction (or stub) whose loop head came
 * last, whatever the code called in between; the rest is the block's.
 * A few callees are also timed whole (calls and cycles per block). */
static FILE *trlog;
static int trwords;
enum { TA_TT, TA_TB, TA_DEC, TA_ANA, TA_EMIT, TA_DBLOOP, TA_DBEND, TA_P3LOOP, TA_P4,
       TA_P2, TA_EBLOOP, TA_EBEND, TA_EBSTUBS, TA_STUB, TA_N };
#define TA_CALLS 5              /* the first five are timed to their return */
#define TA_RET   16             /* event of the return of call k: TA_RET + k */
static const char *ta_name[TA_N] = {
    "timed_translate", "jit_translate_block", "jit_decode_block", "jit_analyse_block",
    "jit_emit_block", "db_loop", "db_end", "p3_loop", "p4_start", "p2_start", "eb_loop",
    "eb_noskip2", "eb_stubs", "eb_stub_loop" };
static uint32_t ta_addr[TA_N], tr_lo = 0xFFFFFFFFu, tr_hi, tr_bctx;
static uint32_t tr_ret[TA_CALLS];
static int tr_on[TA_CALLS];
static double tr_t0[TA_CALLS], tr_inc[TA_CALLS];
#define TR_CALLEES 6
static const char *tc_name[TR_CALLEES] = {
    "jit_flags_needed", "jit_link_host", "jit_link_stub", "jit_out_groups", "jit_busy_loop",
    "jit_next_zone" };
static uint32_t tc_addr[TR_CALLEES], tc_ret[TR_CALLEES];
static int tc_on[TR_CALLEES], tc_n[TR_CALLEES];
static double tc_t0[TR_CALLEES], tc_cyc[TR_CALLEES];
#define TR_MAXEV 8192
static double ev_c[TR_MAXEV];
static uint8_t ev_k[TR_MAXEV];
static uint32_t ev_a[TR_MAXEV], ev_r5[TR_MAXEV];
static int nev;
static void tr_flush(int at_once);
static void tr_event(int k, uint32_t a);

static const char *sym_near(uint32_t a, uint32_t *off);
static struct { uint32_t from, to; } jumps[32];
static uint32_t njump;
static uint32_t reg_code, reg_code_end, reg_blocks, reg_nblocks, reg_ctx;   /* sim_register */
static void dump_code(uint32_t code, uint32_t cur, uint32_t blocks, uint32_t nblocks);

/* A fatal error of the simulated program: the message, then the registers
 * with the symbols of pc and lr and the top of the stack (the saved return
 * addresses of the callers, usually), to locate the fault. */
static void fatal(const char *fmt, ...)
{
    va_list ap;
    uint32_t o1, o2;
    int k;
    va_start(ap, fmt);
    fprintf(stderr, "sim: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (pc %08x, %llu insns)\n", r[15], (unsigned long long)ninsn);
    va_end(ap);
    fprintf(stderr, "sim: pc %s+%x", sym_near(r[15], &o1), o1);
    fprintf(stderr, " lr %s+%x\n", sym_near(r[14], &o2), o2);
    for (k = 0; k < 16; k++)
        fprintf(stderr, " r%d %08x%s", k, r[k], (k & 7) == 7 ? "\n" : "");
    if (r[13] >= LOAD_ADDR && r[13] + 64 <= MEM_SIZE) {
        fprintf(stderr, "sim: stack:");
        for (k = 0; k < 16; k++) {
            uint32_t w = *(uint32_t *)(mem + r[13] + 4 * k);
            w = ((w & 0xFF) << 24) | ((w & 0xFF00) << 8) | ((w >> 8) & 0xFF00) | (w >> 24);
            fprintf(stderr, " %08x", w);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "sim: last branches (from -> to):\n");
    for (k = 0; k < 32; k++) {
        uint32_t i = (njump + k) & 31;
        uint32_t oa, ob;
        if (njump < 32 && i >= njump) continue;
        fprintf(stderr, "  %08x (%s+%x) -> %08x (%s+%x)\n", jumps[i].from, sym_near(jumps[i].from, &oa), oa,
                jumps[i].to, sym_near(jumps[i].to, &ob), ob);
    }
    if (reg_code != 0) {
        /* The code buffer and the descriptors as at the fault, and the
         * lookup table of the context (lookup.bin, 65536 words). */
        FILE *f;
        dump_code(reg_code, reg_code_end, reg_blocks, reg_nblocks);
        f = fopen("lookup.bin", "wb");
        if (f) { fwrite(mem + reg_ctx + 0x4100, 1, 0x40000, f); fclose(f); }
        fprintf(stderr, "sim: code.bin and lookup.bin written (ctx %08x)\n", reg_ctx);
    }
    if (r[15] >= LOAD_ADDR + 128 && r[15] + 64 <= MEM_SIZE) {
        /* The words around pc: filler (0xA5A5A5A5) tells an unused part
         * of the code buffer, stub data words a jump into a stub. */
        fprintf(stderr, "sim: code around pc:");
        for (k = -24; k < 8; k++) {
            uint32_t a = r[15] + 4 * k;
            uint32_t w = ((uint32_t)mem[a] << 24) | ((uint32_t)mem[a + 1] << 16) | ((uint32_t)mem[a + 2] << 8) | mem[a + 3];
            fprintf(stderr, "%s%08x", (k & 7) == 0 ? "\n  " : " ", w);
        }
        fprintf(stderr, "\n");
    }
    exit(1);
}

static inline uint32_t rd32(uint32_t a)
{
    if (a >= MEM_SIZE - 3) fatal("read32 %08x", a);
    if (a & 3) fatal("unaligned read32 %08x", a);
    return ((uint32_t)mem[a] << 24) | ((uint32_t)mem[a + 1] << 16) | ((uint32_t)mem[a + 2] << 8) | mem[a + 3];
}
/* SIMWATCH=hex: the run stops, with the registers, at the first write
 * into the four bytes at that address (to find a stray store). */
static uint32_t watch_addr = 0xFFFFFFFFu;
static uint32_t watch_skip = 0;     /* SIMWATCHSKIP: hits to let through first (the set-up) */
static void watch_hit(uint32_t a, uint32_t v)
{
    uint32_t o;
    if (watch_skip > 0) {
        watch_skip--;
        fprintf(stderr, "sim: watched write %08x = %08x at %s+%x, let through\n", a, v, sym_near(r[15], &o), o);
        return;
    }
    fatal("watched write %08x = %08x", a, v);
}

/* With SIMBLOCKLOG, a store into the code buffer clears the execution
 * count of the word, so that the counts are those of the code it holds. */
static uint32_t blog_lo, blog_span;
static uint32_t *jitcnt;   /* executions per word of memory outside the image */
static inline void wr32(uint32_t a, uint32_t v)
{
    if (a >= MEM_SIZE - 3 || a < 0x100) fatal("write32 %08x", a);
    if (a - blog_lo < blog_span) jitcnt[a >> 2] = 0;
    if (a - watch_addr < 4u || watch_addr - a < 4u) watch_hit(a, v);
    if (a & 3) fatal("unaligned write32 %08x", a);
    mem[a] = v >> 24; mem[a + 1] = v >> 16; mem[a + 2] = v >> 8; mem[a + 3] = v;
}
static inline uint32_t rd8(uint32_t a)
{
    if (a >= MEM_SIZE) fatal("read8 %08x", a);
    return mem[a];
}
static inline void wr8(uint32_t a, uint32_t v)
{
    if (a >= MEM_SIZE || a < 0x100) fatal("write8 %08x", a);
    if (a - blog_lo < blog_span) jitcnt[a >> 2] = 0;
    if (a - watch_addr < 4u) watch_hit(a, v);
    mem[a] = (uint8_t)v;
}

static int cond_ok(uint32_t c)
{
    switch (c) {
    case 0x0: return fz;
    case 0x1: return !fz;
    case 0x2: return fc;
    case 0x3: return !fc;
    case 0x4: return fn;
    case 0x5: return !fn;
    case 0x6: return fv;
    case 0x7: return !fv;
    case 0x8: return fc && !fz;
    case 0x9: return !fc || fz;
    case 0xA: return fn == fv;
    case 0xB: return fn != fv;
    case 0xC: return !fz && fn == fv;
    case 0xD: return fz || fn != fv;
    case 0xE: return 1;
    default: return 0;
    }
}

/* Register value as an operand: r15 reads as the instruction + 8 (+12
 * for a register-specified shift, not used by the code here). */
static inline uint32_t reg(uint32_t n, uint32_t pc) { return n == 15 ? pc + 8 : r[n]; }

/* Shifter operand of a data processing instruction; *carry gets the
 * shifter carry-out. */
static uint32_t shifter(uint32_t op, uint32_t pc, uint32_t *carry, int *regshift)
{
    uint32_t v, amt, type;

    *regshift = 0;
    if (op & (1u << 25)) {
        uint32_t imm = op & 0xFF;
        uint32_t rot = ((op >> 8) & 15) * 2;
        v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        *carry = rot ? (v >> 31) : fc;
        return v;
    }
    v = reg(op & 15, pc);
    type = (op >> 5) & 3;
    if (op & 0x10) {                    /* shift by register */
        *regshift = 1;
        if ((op & 15) == 15) v += 4;
        amt = r[(op >> 8) & 15] & 0xFF;
        if (amt == 0) { *carry = fc; return v; }
        switch (type) {
        case 0:
            if (amt < 32) { *carry = (v >> (32 - amt)) & 1; return v << amt; }
            *carry = amt == 32 ? v & 1 : 0; return 0;
        case 1:
            if (amt < 32) { *carry = (v >> (amt - 1)) & 1; return v >> amt; }
            *carry = amt == 32 ? v >> 31 : 0; return 0;
        case 2:
            if (amt < 32) { *carry = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
            *carry = v >> 31; return (v >> 31) ? 0xFFFFFFFFu : 0;
        default:
            amt &= 31;
            if (amt == 0) { *carry = v >> 31; return v; }
            *carry = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
        }
    }
    amt = (op >> 7) & 31;
    switch (type) {
    case 0:
        if (amt == 0) { *carry = fc; return v; }
        *carry = (v >> (32 - amt)) & 1; return v << amt;
    case 1:
        if (amt == 0) { *carry = v >> 31; return 0; }
        *carry = (v >> (amt - 1)) & 1; return v >> amt;
    case 2:
        if (amt == 0) { *carry = v >> 31; return (v >> 31) ? 0xFFFFFFFFu : 0; }
        *carry = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default:
        if (amt == 0) { uint32_t c = fc; *carry = v & 1; return (v >> 1) | (c << 31); }
        *carry = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}

/* Address offset of a single data transfer with a register offset. */
static uint32_t ls_offset(uint32_t op, uint32_t pc)
{
    uint32_t v = reg(op & 15, pc);
    uint32_t amt = (op >> 7) & 31;

    switch ((op >> 5) & 3) {
    case 0: return v << amt;
    case 1: return amt ? v >> amt : 0;
    case 2: return amt ? (uint32_t)((int32_t)v >> amt) : ((v >> 31) ? 0xFFFFFFFFu : 0);
    default: return amt ? (v >> amt) | (v << (32 - amt)) : ((v >> 1) | (fc << 31));
    }
}

/*--------------------------------------------------------------------------
 * Host services
 *------------------------------------------------------------------------*/

static void read_str(uint32_t a, char *buf, int max)
{
    int i;
    for (i = 0; i < max - 1; i++) {
        char ch = (char)rd8(a + i);
        buf[i] = ch;
        if (!ch) return;
    }
    buf[i] = 0;
}

/* printf (format in r0, arguments from r1) and sprintf (destination in
 * r0, format in r1, arguments from r2): the text is formatted on the
 * host and written to stdout or into the ARM memory. */
static void do_format(int to_memory)
{
    char fmt[1024], out[4096], spec[32], tmp[512];
    uint32_t argn = to_memory ? 2 : 1;
    char *p, *o = out;

    read_str(r[to_memory ? 1 : 0], fmt, sizeof(fmt));
#define NEXTARG() (argn <= 3 ? r[argn++] : rd32(r[13] + 4 * (argn++ - 4)))
    for (p = fmt; *p; p++) {
        if (*p != '%') { *o++ = *p; continue; }
        {
            char *s = spec;
            *s++ = '%';
            p++;
            while (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') *s++ = *p++;
            while (*p >= '0' && *p <= '9') *s++ = *p++;
            if (*p == '.') { *s++ = *p++; while (*p >= '0' && *p <= '9') *s++ = *p++; }
            while (*p == 'l' || *p == 'h') p++;
            *s++ = *p;
            *s = 0;
            switch (*p) {
            case 'd': case 'i': o += sprintf(o, spec, (int)(int32_t)NEXTARG()); break;
            case 'u': case 'x': case 'X': case 'o': o += sprintf(o, spec, (unsigned)NEXTARG()); break;
            case 'c': o += sprintf(o, spec, (int)NEXTARG()); break;
            case 's': read_str(NEXTARG(), tmp, sizeof(tmp)); o += sprintf(o, spec, tmp); break;
            case '%': *o++ = '%'; break;
            default: o += sprintf(o, "<%s>", spec); break;
            }
        }
    }
    *o = 0;
    if (to_memory) {
        uint32_t n = (uint32_t)(o - out), i;
        for (i = 0; i <= n; i++) wr8(r[0] + i, (uint8_t)out[i]);
        r[0] = n;
        return;
    }
    fputs(out, stdout);
    fflush(stdout);
}

static void do_dump(uint32_t fb, uint32_t width, uint32_t height, uint32_t tag)
{
    char name[64];
    FILE *f;
    uint32_t x, y;

    sprintf(name, tag >= 500000 ? "cels_%06u.ppm" : "view_%06u.ppm", tag >= 500000 ? tag - 500000 : tag);
    f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t w = rd32(fb + 4 * ((y >> 1) * width + x));
            uint32_t c = (y & 1) ? (w & 0xFFFF) : (w >> 16);
            uint8_t rgb[3];
            rgb[0] = ((c >> 10) & 31) * 255 / 31;
            rgb[1] = ((c >> 5) & 31) * 255 / 31;
            rgb[2] = (c & 31) * 255 / 31;
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* Pad script: "frame:bits" pairs in the environment variable SIMPAD
 * give the buttons held from that frame on (bit 8: PAUSE). */
static uint32_t pad_frames[1024], pad_bits[1024];
static int npad;

static uint32_t pad_at(uint32_t frame)
{
    uint32_t b = 0;
    int i;
    for (i = 0; i < npad; i++)
        if (pad_frames[i] <= frame) b = pad_bits[i];
    return b;
}

static double mark_t[16];
static double mark_sum[16];
static uint32_t mark_n[16];

static const char *sym_near(uint32_t a, uint32_t *off);
static void dump_code(uint32_t code, uint32_t cur, uint32_t blocks, uint32_t nblocks)
{
    FILE *f = fopen("code.bin", "wb");
    uint32_t k;
    if (f) {
        uint32_t hdr[4] = { code, cur, blocks, nblocks };
        fwrite(hdr, 4, 4, f);
        fwrite(mem + code, 1, cur - code, f);
        /* The first five words of each 24-byte descriptor (pc, key,
         * entry, body, hash chain), as the scripts read them. */
        for (k = 0; k < nblocks; k++) fwrite(mem + blocks + 24 * k, 1, 20, f);
        fwrite(execmap + (code >> 2), 1, (cur - code) >> 2, f);
        fwrite(jitcyc + (code >> 2), 4, (cur - code) >> 2, f);
        fclose(f);
    }
    f = fopen("codecnt.bin", "wb");
    if (f) {
        fwrite(jitcnt + (code >> 2), 4, (cur - code) >> 2, f);
        fclose(f);
    }
}

/* SIMBLOCKLOG=file: the translator's block descriptors are scanned at the
 * start of every frame (the pad service, called once per frame, costs no
 * simulated cycle more), and the changes since the previous scan are
 * written as lines of the frame that made them (the frame before; -1 is
 * the translation before the run):
 *   T frame slot pc key entry           a block translated
 *   E frame slot pc key entry last runs a block evicted (or replaced): the
 *                                       last frame it ran (-1: never) and
 *                                       its entries since its translation
 *   R frame slot slot ...               the blocks entered in the frame
 * A block's entries are the executions of the first or the second word
 * of its body, whichever ran more (the first word, the chunk mark store,
 * is not where every entry lands). */
static FILE *blog;
static uint32_t *bl_entry, *bl_pc, *bl_key, *bl_body, *bl_cnt, *bl_runs;
static int32_t *bl_last;
static int32_t bl_frame = -2;

static void block_log(int32_t frame)
{
    uint32_t k, n = reg_nblocks;
    int32_t f = frame - 1;
    int any = 0;
    if (!blog || reg_blocks == 0 || frame <= bl_frame) return;
    bl_frame = frame;
    if (!bl_entry) {
        bl_entry = calloc(n, 4); bl_pc = calloc(n, 4); bl_key = calloc(n, 4); bl_body = calloc(n, 4);
        bl_cnt = calloc(n, 4); bl_runs = calloc(n, 4); bl_last = calloc(n, 4);
    }
    for (k = 0; k < n; k++) {
        uint32_t d = reg_blocks + 24 * k;
        uint32_t pc = rd32(d) & 0xFFFF, key = rd32(d + 4), entry = rd32(d + 8), body = rd32(d + 12);
        /* A free descriptor keeps the pc, key and entry written before an
         * emission stopped by the zone's room check (the block then takes
         * another descriptor in the next zone): only a body within the
         * entry check's reach of the entry tells a block in use. */
        if (body - entry > 256u) entry = 0;
        if (bl_entry[k] && (entry != bl_entry[k] || pc != bl_pc[k] || key != bl_key[k])) {
            fprintf(blog, "E %d %u %04x %x %x %d %u\n", f, k, bl_pc[k], bl_key[k], bl_entry[k], bl_last[k], bl_runs[k]);
            bl_entry[k] = 0;
        }
        if (entry && !bl_entry[k]) {
            fprintf(blog, "T %d %u %04x %x %x\n", f, k, pc, key, entry);
            bl_entry[k] = entry; bl_pc[k] = pc; bl_key[k] = key; bl_body[k] = body;
            bl_cnt[k] = 0; bl_runs[k] = 0; bl_last[k] = -1;
        }
    }
    for (k = 0; k < n; k++) {
        uint32_t c;
        if (!bl_entry[k]) continue;
        c = jitcnt[bl_body[k] >> 2];
        if (jitcnt[(bl_body[k] >> 2) + 1] > c) c = jitcnt[(bl_body[k] >> 2) + 1];
        if (c != bl_cnt[k]) {
            if (!any) { fprintf(blog, "R %d", f); any = 1; }
            fprintf(blog, " %u", k);
            bl_runs[k] += c > bl_cnt[k] ? c - bl_cnt[k] : c;    /* the word rewritten: counted again from 0 */
            bl_cnt[k] = c;
            bl_last[k] = f;
        }
    }
    if (any) fprintf(blog, "\n");
}

/* The DRAM free before the cartridge for the console's executable whose
 * AIF header (its first 128 bytes, copied by mk.sh from takeme/LaunchMe)
 * is in the file: a build that grows shows the code buffer zones it costs
 * on the console. */
static void dram_from_aif(const char *path)
{
    uint8_t h[36];
    uint32_t w[9], foot;
    FILE *f = fopen(path, "rb");
    int k;

    if (!f || fread(h, 1, sizeof(h), f) != sizeof(h)) {
        if (f) fclose(f);
        fprintf(stderr, "sim: no executable header in %s: DRAM free before the cartridge "
                "taken as %u bytes (-dramfree n to set it)\n", path, (unsigned)dram_free);
        return;
    }
    fclose(f);
    for (k = 0; k < 9; k++)
        w[k] = (uint32_t)h[4 * k] << 24 | (uint32_t)h[4 * k + 1] << 16 |
               (uint32_t)h[4 * k + 2] << 8 | h[4 * k + 3];
    if (w[4] != 0xEF000011u)            /* SWI exit: the fourth entry of an AIF header */
        fatal("%s: not an AIF header", path);
    foot = ((w[5] + w[6] + 15) & ~15u) + ((w[8] + 15) & ~15u);
    dram_free = DRAM_PHOENIX - foot;
    fprintf(stderr, "sim: %u bytes of DRAM free before the cartridge (executable of %u bytes "
            "in DRAM, %s)\n", (unsigned)dram_free, (unsigned)foot, path);
}

static void do_swi(uint32_t n)
{
    switch (n) {
    case 0: done = 1; break;
    case 1: do_format(0); break;
    case 12: do_format(1); break;
    case 2: {
        uint32_t size = (r[1] + 7) & ~7u;
        uint32_t vram = (r[2] & 0x10000u) != 0;
        uint32_t *avail = vram ? &vram_free : &dram_free;
        uint32_t a = heap;
        if (getenv("SIMALLOC")) {
            uint32_t o;
            const char *sy = sym_near(r[14], &o);
            fprintf(stderr, "Alloc: %u bytes %s from %s+%x%s\n", (unsigned)r[1],
                    vram ? "VRAM" : "DRAM", sy, (unsigned)o, size > *avail ? ": no memory" : "");
        }
        if (size > *avail) {
            r[0] = 0;
            break;
        }
        *avail -= size;
        if (nallocs < 256) {
            allocs[nallocs].a = a;
            allocs[nallocs].size = size;
            allocs[nallocs++].vram = vram;
        }
        heap += size;
        if (heap > HEAP_END) fatal("out of heap");
        if (r[2] & 0x100u) memset(mem + a, r[2] & 0xFF, size);  /* MEMTYPE_FILL */
        else memset(mem + a, 0xA5, size);
        r[0] = a;
        break;
    }
    case 3: {
        /* The space is given back to the count; to the heap as well when
         * the block is the last one allocated (an allocation freed before
         * the next one, as the reference view's tables are). */
        uint32_t k;
        for (k = 0; k < nallocs; k++) {
            if (allocs[k].a == r[1]) {
                *(allocs[k].vram ? &vram_free : &dram_free) += allocs[k].size;
                if (allocs[k].a + allocs[k].size == heap)
                    heap = allocs[k].a;
                allocs[k] = allocs[--nallocs];
                break;
            }
        }
        break;
    }
    case 13:
        wr32(r[0], dram_free);
        wr32(r[1], vram_free);
        break;
    case 4: r[0] = usec_now(); break;
    case 15: {
        uint32_t t;
        cI += clock_half;
        t = usec_now();
        wr32(r[0], t / 1000000u);
        wr32(r[0] + 4, t % 1000000u);
        cI += clock_half;
        break;
    }
    case 16: r[0] = usec_now(); break;
    case 5: r[0] = ROM_ADDR; break;
    case 6: r[0] = rom_size; break;
    case 7: do_dump(r[0], r[1], r[2], r[3]); break;
    case 8: r[0] = r[0] < 32 ? args[r[0]] : 0; break;
    case 9: block_log((int32_t)r[0]); r[0] = pad_at(r[0]); break;
    case 10: {
        uint32_t id = r[0] & 15;
        if (id == 15) { memset(jitcyc, 0, (MEM_SIZE / 4) * sizeof(float)); memset(jitcnt, 0, (MEM_SIZE / 4) * sizeof(uint32_t)); memset(prof, 0, ((img_end - LOAD_ADDR) / 4 + 1) * sizeof(double)); memset(pcount, 0, ((img_end - LOAD_ADDR) / 4 + 1) * sizeof(uint32_t)); prof_jit = 0; break; }
        if (id & 1) mark_t[id] = cycles();
        else { mark_sum[id] += cycles() - mark_t[id - 1]; mark_n[id]++; }
        break;
    }
    case 11: dump_code(r[0], r[1], r[2], r[3]); break;
    case 14:
        reg_code = r[0]; reg_code_end = r[1]; reg_blocks = r[2]; reg_nblocks = r[3]; reg_ctx = rd32(r[13]);
        if (blog) { blog_lo = reg_code; blog_span = reg_code_end - reg_code; }
        break;
    default: fatal("unknown swi %x", n);
    }
}

/*--------------------------------------------------------------------------
 * Interpreter
 *------------------------------------------------------------------------*/

/* SIGUSR1: where the simulated program is (pc, lr, sp, the symbols of
 * pc and lr), to find a hang without a debugger. */
static const char *sym_near(uint32_t a, uint32_t *off)
{
    int k;
    const char *name = "?";
    *off = a;
    for (k = 0; k < nsyms; k++) {
        if (syms[k].addr <= a) { name = syms[k].name; *off = a - syms[k].addr; }
        else break;
    }
    return name;
}

static void on_usr1(int sig)
{
    uint32_t o1, o2;
    const char *s1 = sym_near(r[15], &o1);
    const char *s2 = sym_near(r[14], &o2);
    (void)sig;
    fprintf(stderr, "sim: at pc %08x (%s+%x) lr %08x (%s+%x) sp %08x r0 %08x r1 %08x, %llu insns, %.0f us\n",
            r[15], s1, o1, r[14], s2, o2, r[13], r[0], r[1], (unsigned long long)ninsn, cycles() * 0.08);
}

static void tr_event(int k, uint32_t a)
{
    if (nev < TR_MAXEV) {
        ev_c[nev] = cycles();
        ev_k[nev] = (uint8_t)k;
        ev_a[nev] = a;
        ev_r5[nev] = r[5];      /* the emitter's cursor */
        nev++;
    }
}

/* Called at each instruction while SIMTRLOG is set: the stamps. */
static void tr_step(uint32_t pc)
{
    int k;

    for (k = 0; k < TA_CALLS; k++)
        if (tr_on[k] && pc == tr_ret[k]) {
            tr_on[k] = 0;
            tr_inc[k] = cycles() - tr_t0[k];
            tr_event(TA_RET + k, 0);
            if (k == TA_TB && !tr_on[TA_TT])
                tr_flush(0);
            if (k == TA_TT)
                tr_flush(1);
        }
    for (k = 0; k < TR_CALLEES; k++) {
        if (tc_on[k] && pc == tc_ret[k]) {
            tc_on[k] = 0;
            tc_cyc[k] += cycles() - tc_t0[k];
        } else if (!tc_on[k] && pc == tc_addr[k]) {
            tc_on[k] = 1;
            tc_ret[k] = r[14];
            tc_t0[k] = cycles();
            tc_n[k]++;
        }
    }
    if (pc < tr_lo || pc > tr_hi)
        return;
    for (k = 0; k < TA_N; k++) {
        if (pc != ta_addr[k])
            continue;
        if (k < TA_CALLS) {
            tr_on[k] = 1;
            tr_ret[k] = r[14];
            tr_t0[k] = cycles();
            if (k == TA_TT || (k == TA_TB && !tr_on[TA_TT])) {
                int q;
                nev = 0;
                for (q = 0; q < TR_CALLEES; q++) {
                    tc_cyc[q] = 0;
                    tc_n[q] = 0;
                }
            }
        }
        /* The stub loop: the first word of the stub record (r7, the end
         * in r9), which holds its kind. */
        tr_event(k, k == TA_STUB ? (r[7] < r[9] ? rd32(r[7]) : 0xFFFFFFFFu) : 0);
    }
}

/* One translated block, written when jit_translate_block returns (or
 * timed_translate, for a translation at once):
 *   B pc key n at_once tt T tb T dec T ana T emit T decins T anains T
 *     emitins T stubs T nstub n retry n wstub n wtail n sk n:T x 8
 *     callees n:T x 6
 * with T cycles: tt the whole timed_translate (0 when not at once), tb
 * jit_translate_block, dec / ana / emit the three passes, decins / anains /
 * emitins the parts charged to instructions, stubs the stub pass; nstub
 * the stubs, retry the emissions made again in another zone, wstub and
 * wtail the words of the stubs and of the code between the last
 * instruction and the stubs; sk the stubs and their cycles by kind
 * (STUB_TIMEOUT to STUB_DYN, z80jit_int.h); callees the calls and
 * inclusive cycles of tc_name. Then one line per instruction:
 *   I w0 ... w9 dec ana emit words
 * the first ten words of its record (jit_insn) in hexadecimal, the cycles
 * of the three passes charged to it and the words emitted for it (its
 * segment check and interrupt check included), followed with SIMTRWORDS
 * by a W line holding those words. */
static void tr_flush(int at_once)
{
    uint32_t ins = rd32(tr_bctx + 4), n = rd32(tr_bctx + 8);
    uint32_t key = rd32(tr_bctx + 16), pc = rd32(tr_bctx + 56);
    double dec[64] = {0}, ana[64] = {0}, emi[64] = {0};
    uint32_t wr[64] = {0}, w_end = 0, w_body = 0, w_stubs = 0, w_last = 0;
    double dec_ins = 0, ana_ins = 0, emi_ins = 0, stub_tot = 0, stubc[16] = {0};
    int stubn[16] = {0};
    int idb = -1, ip3 = -1, ieb = -1, phase = -1, cur = -1, nstub = 0, retries = 0;
    int i, k;

    if (n > 64)
        n = 64;
    for (i = 0; i < nev; i++) {
        if (i > 0) {
            double d = ev_c[i] - ev_c[i - 1];
            if (phase == 0 && idb >= 0) dec[idb] += d;
            else if (phase == 1 && ip3 >= 0) ana[ip3] += d;
            else if (phase == 2 && ieb >= 0) emi[ieb] += d;
            else if (phase == 3 && cur >= 0) { stubc[cur] += d; stub_tot += d; }
        }
        k = ev_k[i];
        switch (k) {
        case TA_DBLOOP: phase = 0; idb++; if (idb > 63) idb = 63; break;
        case TA_P3LOOP: phase = 1; ip3 = ip3 < 0 ? (int)n - 1 : (ip3 > 0 ? ip3 - 1 : 0); break;
        case TA_EMIT:
            /* An emission made again in the next zone: only the last counts. */
            if (ieb >= 0) retries++;
            ieb = -1; nstub = 0; stub_tot = 0;
            memset(emi, 0, sizeof emi);
            memset(stubc, 0, sizeof stubc);
            memset(stubn, 0, sizeof stubn);
            break;
        case TA_EBLOOP:
            phase = 2;
            if (ieb >= 0) wr[ieb] = ev_r5[i];
            else w_body = ev_r5[i];
            if (ieb < 63) ieb++;
            break;
        case TA_EBEND: phase = -1; if (ieb >= 0) wr[ieb] = ev_r5[i]; w_end = ev_r5[i]; break;
        case TA_EBSTUBS: phase = -1; w_stubs = ev_r5[i]; break;
        case TA_STUB:
            phase = 3;
            cur = ev_a[i] == 0xFFFFFFFFu ? 15 : (int)((ev_a[i] >> 8) & 15);
            if (cur != 15) { stubn[cur]++; nstub++; }
            break;
        case TA_RET + TA_EMIT: phase = -1; w_last = rd32(rd32(tr_bctx) + 12); break;    /* j->cur */
        default: phase = -1; break;
        }
    }
    for (i = 0; i < (int)n; i++) {
        dec_ins += dec[i];
        ana_ins += ana[i];
        emi_ins += emi[i];
    }
    fprintf(trlog, "B %04x %x %u %d tt %.0f tb %.0f dec %.0f ana %.0f emit %.0f decins %.0f anains %.0f "
            "emitins %.0f stubs %.0f nstub %d retry %d wstub %d wtail %d sk",
            (unsigned)(pc & 0xFFFF), (unsigned)key, (unsigned)n, at_once, at_once ? tr_inc[TA_TT] : 0,
            tr_inc[TA_TB], tr_inc[TA_DEC], tr_inc[TA_ANA], tr_inc[TA_EMIT], dec_ins, ana_ins, emi_ins,
            stub_tot, nstub, retries, (int)(w_last - w_stubs) / 4, (int)(w_stubs - w_end) / 4);
    for (k = 0; k < 8; k++)
        fprintf(trlog, " %d:%.0f", stubn[k], stubc[k]);
    fprintf(trlog, " callees");
    for (k = 0; k < TR_CALLEES; k++)
        fprintf(trlog, " %d:%.0f", tc_n[k], tc_cyc[k]);
    fprintf(trlog, "\n");
    for (i = 0; i < (int)n; i++) {
        uint32_t a = ins + 48 * i, prev = i == 0 ? w_body : wr[i - 1], w;

        fprintf(trlog, "I");
        for (k = 0; k < 10; k++)
            fprintf(trlog, " %08x", rd32(a + 4 * k));
        fprintf(trlog, " %.0f %.0f %.0f %d\n", dec[i], ana[i], emi[i], (int)(wr[i] - prev) / 4);
        if (trwords) {
            fprintf(trlog, "W");
            for (w = prev; w < wr[i] && w - prev < 256; w += 4)
                fprintf(trlog, " %08x", rd32(w));
            fprintf(trlog, "\n");
        }
    }
    nev = 0;
}

static void run(void)
{
    signal(SIGUSR1, on_usr1);
    while (!done) {
        uint32_t pc = r[15];
        uint32_t op = rd32(pc);
        double c0 = 0;
        uint64_t s0 = cS, n0 = cN, i0 = cI;

        if (trlog)
            tr_step(pc);

        ninsn++;
        r[15] = pc + 4;
        if (!cond_ok(op >> 28)) {
            cS++;
            goto account;
        }
        switch ((op >> 25) & 7) {
        case 0:
            if ((op & 0x0FC000F0u) == 0x00000090u) {        /* MUL / MLA */
                uint32_t rm = r[op & 15], rs = r[(op >> 8) & 15];
                uint32_t res = rm * rs;
                uint32_t m;
                if (op & (1u << 21)) res += r[(op >> 12) & 15];
                r[(op >> 16) & 15] = res;
                if (op & (1u << 20)) { fn = res >> 31; fz = res == 0; }
                /* Booth: 2 bits per cycle with early termination. */
                if (rs <= 1) m = 1;
                else if (rs >= (1u << 29)) m = 16;
                else { m = 1; while (m < 16 && !(rs < (1u << (2 * m - 1)))) m++; }
                cS += 1; cI += m;
                break;
            }
            if ((op & 0x0FB00FF0u) == 0x01000090u) {        /* SWP */
                uint32_t a = r[(op >> 16) & 15], v;
                if (op & (1u << 22)) { v = rd8(a); wr8(a, r[op & 15]); }
                else { v = rd32(a); wr32(a, r[op & 15]); }
                r[(op >> 12) & 15] = v;
                cS += 1; cN += 2; cI += 1;
                break;
            }
            /* fall through */
        case 1: {
            uint32_t opc = (op >> 21) & 15;
            uint32_t s = (op >> 20) & 1;
            uint32_t rn = (op >> 16) & 15, rdn = (op >> 12) & 15;
            uint32_t a, b, res = 0, carry;
            int regshift;
            int logical = 0, write = 1;

            if ((op & 0x0FBF0FFFu) == 0x010F0000u) {        /* MRS */
                r[rdn] = (fn << 31) | (fz << 30) | (fc << 29) | (fv << 28) | 0x13;
                cS++;
                break;
            }
            if ((op & 0x0DB0F000u) == 0x0120F000u) {        /* MSR */
                uint32_t v = (op & (1u << 25)) ? shifter(op, pc, &carry, &regshift) : r[op & 15];
                fn = v >> 31; fz = (v >> 30) & 1; fc = (v >> 29) & 1; fv = (v >> 28) & 1;
                cS++;
                break;
            }
            b = shifter(op, pc, &carry, &regshift);
            a = reg(rn, pc);
            if (regshift && rn == 15) a += 4;
            switch (opc) {
            case 0x0: res = a & b; logical = 1; break;
            case 0x1: res = a ^ b; logical = 1; break;
            case 0x2: res = a - b; if (s) { fc = a >= b; fv = ((a ^ b) & (a ^ res)) >> 31; } break;
            case 0x3: res = b - a; if (s) { fc = b >= a; fv = ((b ^ a) & (b ^ res)) >> 31; } break;
            case 0x4: res = a + b; if (s) { fc = res < a; fv = (~(a ^ b) & (a ^ res)) >> 31; } break;
            case 0x5: { uint64_t t = (uint64_t)a + b + fc; res = (uint32_t)t;
                if (s) { fc = (uint32_t)(t >> 32); fv = (~(a ^ b) & (a ^ res)) >> 31; } break; }
            case 0x6: { uint64_t t = (uint64_t)a - b - (1 - fc); res = (uint32_t)t;
                if (s) { fc = !((t >> 32) & 1); fv = ((a ^ b) & (a ^ res)) >> 31; } break; }
            case 0x7: { uint64_t t = (uint64_t)b - a - (1 - fc); res = (uint32_t)t;
                if (s) { fc = !((t >> 32) & 1); fv = ((b ^ a) & (b ^ res)) >> 31; } break; }
            case 0x8: res = a & b; logical = 1; write = 0; break;
            case 0x9: res = a ^ b; logical = 1; write = 0; break;
            case 0xA: res = a - b; write = 0; fc = a >= b; fv = ((a ^ b) & (a ^ res)) >> 31; break;
            case 0xB: res = a + b; write = 0; fc = res < a; fv = (~(a ^ b) & (a ^ res)) >> 31; break;
            case 0xC: res = a | b; logical = 1; break;
            case 0xD: res = b; logical = 1; break;
            case 0xE: res = a & ~b; logical = 1; break;
            default:  res = ~b; logical = 1; break;
            }
            if (s) {
                fn = res >> 31; fz = res == 0;
                if (logical) fc = carry;
            }
            cS++;
            if (regshift) cI++;
            if (write) {
                r[rdn] = res;
                if (rdn == 15) {
                    if (s) fatal("data processing with S to pc");
                    r[15] = res & ~3u;
                    cS++; cN++;
                }
            }
            break;
        }
        case 2:
        case 3: {                                           /* LDR / STR */
            uint32_t rn = (op >> 16) & 15, rdn = (op >> 12) & 15;
            uint32_t off, base, addr;
            int pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1;
            int wb = (op >> 21) & 1, load = (op >> 20) & 1;

            if ((op & (1u << 25)) && (op & 0x10)) fatal("undefined instruction %08x", op);
            off = (op & (1u << 25)) ? ls_offset(op, pc) : (op & 0xFFF);
            base = reg(rn, pc);
            addr = pre ? (up ? base + off : base - off) : base;
            if (load) {
                uint32_t v = byte ? rd8(addr) : rd32(addr);
                if (!pre) { r[rn] = up ? base + off : base - off; }
                else if (wb) r[rn] = addr;
                r[rdn] = v;
                cS++; cN++; cI++;
                if (rdn == 15) { r[15] = v & ~3u; cS++; cN++; }
            } else {
                uint32_t v = reg(rdn, pc);
                if (rdn == 15) v += 4;
                if (byte) wr8(addr, v); else wr32(addr, v);
                if (!pre) r[rn] = up ? base + off : base - off;
                else if (wb) r[rn] = addr;
                cN += 2;
            }
            break;
        }
        case 4: {                                           /* LDM / STM */
            uint32_t rn = (op >> 16) & 15;
            uint32_t list = op & 0xFFFF;
            int pre = (op >> 24) & 1, up = (op >> 23) & 1;
            int wb = (op >> 21) & 1, load = (op >> 20) & 1;
            uint32_t n = __builtin_popcount(list);
            uint32_t base = r[rn];
            uint32_t addr, final;
            int i;

            if (n == 0) fatal("empty LDM/STM");
            if (up) { addr = pre ? base + 4 : base; final = base + 4 * n; }
            else { addr = pre ? base - 4 * n : base - 4 * n + 4; final = base - 4 * n; }
            if (load) {
                uint32_t vals[16];
                int k = 0;
                for (i = 0; i < 16; i++) if (list & (1u << i)) { vals[k++] = rd32(addr); addr += 4; }
                if (wb) r[rn] = final;
                k = 0;
                for (i = 0; i < 16; i++) if (list & (1u << i)) r[i] = vals[k++];
                cS += n; cN++; cI++;
                if (list & 0x8000) { r[15] &= ~3u; cS++; cN++; }
            } else {
                for (i = 0; i < 16; i++) if (list & (1u << i)) {
                    uint32_t v = (i == 15) ? pc + 12 : r[i];
                    wr32(addr, v);
                    addr += 4;
                }
                if (wb) r[rn] = final;
                cS += n - 1; cN += 2;
            }
            break;
        }
        case 5: {                                           /* B / BL */
            int32_t off = (int32_t)(op << 8) >> 6;
            if (op & (1u << 24)) r[14] = pc + 4;
            r[15] = pc + 8 + off;
            cS += 2; cN++;
            break;
        }
        case 7:
            if (op & (1u << 24)) {                          /* SWI */
                uint32_t n = op & 0xFFFFFF;
                if ((n & 0xFFFF00u) != 0xF00000u) fatal("OS SWI %06x", n);
                cS += 2; cN++;
                do_swi(n & 0xFF);
                break;
            }
            /* fall through */
        default:
            fatal("undefined instruction %08x", op);
        }
account:
        if (r[15] != pc + 4) {
            /* The last taken branches, dumped by fatal(). */
            jumps[njump & 31].from = pc;
            jumps[njump & 31].to = r[15];
            njump++;
        }
        c0 = (cS - s0) * weightS + (cN - n0) * weightN + (cI - i0) * weightI;
        if (pc < img_end && pc >= LOAD_ADDR) { prof[(pc - LOAD_ADDR) >> 2] += c0; pcount[(pc - LOAD_ADDR) >> 2]++; }
        else { prof_jit += c0; execmap[pc >> 2] = 1; jitcyc[pc >> 2] += (float)c0; jitcnt[pc >> 2]++; }
    }
}

/*--------------------------------------------------------------------------
 * Set-up and report
 *------------------------------------------------------------------------*/

static int sym_cmp(const void *a, const void *b)
{
    const sym_t *x = a, *y = b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256], name[128];
    unsigned addr;
    if (!f) return;
    syms = calloc(8192, sizeof(sym_t));
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%127s %x", name, &addr) == 2 && strchr(name, '$') == NULL && nsyms < 8192) {
            if (addr < LOAD_ADDR || addr >= img_end) continue;
            syms[nsyms].addr = addr;
            strncpy(syms[nsyms].name, name, 63);
            nsyms++;
        }
    }
    fclose(f);
    qsort(syms, nsyms, sizeof(sym_t), sym_cmp);
}

static void report_profile(double total)
{
    double *per = calloc(nsyms + 1, sizeof(double));
    int i, k = -1;
    uint32_t a;
    typedef struct { double c; int i; } ent;
    ent *e;
    int ne = 0;

    for (a = LOAD_ADDR; a < img_end; a += 4) {
        while (k + 1 < nsyms && syms[k + 1].addr <= a) k++;
        if (k >= 0) per[k] += prof[(a - LOAD_ADDR) >> 2];
    }
    e = calloc(nsyms + 2, sizeof(ent));
    for (i = 0; i < nsyms; i++) if (per[i] > 0) { e[ne].c = per[i]; e[ne].i = i; ne++; }
    for (i = 0; i < ne; i++) {
        int j;
        for (j = i + 1; j < ne; j++) if (e[j].c > e[i].c) { ent t = e[i]; e[i] = e[j]; e[j] = t; }
    }
    fprintf(stderr, "profile: total %.0f cycles (%.0f us)\n", total, total * 0.08);
    fprintf(stderr, "  %-28s %12.0f cycles %6.2f%%\n", "(generated code)", prof_jit, 100.0 * prof_jit / total);
    /* The 40 largest, or every function with cycles (SIMPROFALL). */
    for (i = 0; i < ne && (i < 40 || getenv("SIMPROFALL")); i++)
        fprintf(stderr, "  %-28s %12.0f cycles %6.2f%% %10u entries\n", syms[e[i].i].name, e[i].c,
                100.0 * e[i].c / total, pcount[(syms[e[i].i].addr - LOAD_ADDR) >> 2]);
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    const char *img = "sim.bin", *romp = NULL, *sym = "sim.sym", *aif = NULL;
    int i;
    char *padenv;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-rom") && i + 1 < argc) romp = argv[++i];
        else if (!strcmp(argv[i], "-img") && i + 1 < argc) img = argv[++i];
        else if (!strcmp(argv[i], "-sym") && i + 1 < argc) sym = argv[++i];
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc) args[0] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-bench")) args[1] = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc) args[2] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-from") && i + 1 < argc) args[3] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-interp")) args[4] = 1;
        else if (!strcmp(argv[i], "-gg")) args[24] = 1;
        else if (!strcmp(argv[i], "-sms")) args[24] = 2;
        else if (!strcmp(argv[i], "-seed")) args[11] = 1;
        else if (!strcmp(argv[i], "-check")) args[13] = 1;
        else if (!strcmp(argv[i], "-hash")) args[14] = 100;
        else if (!strcmp(argv[i], "-hashevery") && i + 1 < argc) args[14] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-cels") && i + 1 < argc) args[16] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-celhash")) args[15] = 1;
        else if (!strcmp(argv[i], "-dumpdiff") && i + 1 < argc) args[17] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-rasterlog")) args[18] = 1;
        else if (!strcmp(argv[i], "-framelog")) args[19] = 1;
        else if (!strcmp(argv[i], "-dectest")) args[21] = 1;
        else if (!strcmp(argv[i], "-anatest")) args[22] = 1;
        else if (!strcmp(argv[i], "-emittest")) args[23] = 1;
        else if (!strcmp(argv[i], "-force") && i + 1 < argc) args[20] = atoi(argv[++i]) + 1;
        else if (!strcmp(argv[i], "-forcelimit") && i + 1 < argc) args[25] = atoi(argv[++i]) + 1;
        else if (!strcmp(argv[i], "-zones") && i + 1 < argc) args[27] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dramfree") && i + 1 < argc) { dram_free = atoi(argv[++i]); dram_given = 1; }
        else if (!strcmp(argv[i], "-aif") && i + 1 < argc) aif = argv[++i];
        else if (!strcmp(argv[i], "-hotzones") && i + 1 < argc) args[12] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-budget") && i + 1 < argc) {
            unsigned f = 0, c = 0;
            sscanf(argv[++i], "%u:%u", &f, &c);
            args[5] = f; args[6] = c; args[7] = 1;
        }
        else if (!strcmp(argv[i], "-hot") && i + 1 < argc) {
            unsigned q = 0, t = 0;
            sscanf(argv[++i], "%u:%u", &q, &t);
            args[8] = q; args[9] = t; args[10] = 1;
        }
    }
    if (!dram_given) {
        /* sim.aif beside the image by default. */
        static char path[4096];
        const char *slash = strrchr(img, '/');
        size_t dir = slash ? (size_t)(slash - img) + 1 : 0;

        if (!aif && dir + 8 < sizeof(path)) {
            memcpy(path, img, dir);
            strcpy(path + dir, "sim.aif");
            aif = path;
        }
        if (aif) dram_from_aif(aif);
    }
    mem = calloc(1, MEM_SIZE);
    execmap = calloc(1, MEM_SIZE / 4);
    jitcyc = calloc(MEM_SIZE / 4, sizeof(float));
    jitcnt = calloc(MEM_SIZE / 4, sizeof(uint32_t));
    f = fopen(img, "rb");
    if (!f) { perror(img); return 1; }
    n = fread(mem + LOAD_ADDR, 1, MEM_SIZE - LOAD_ADDR, f);
    fclose(f);
    img_end = LOAD_ADDR + ((n + 3) & ~3);
    prof = calloc((img_end - LOAD_ADDR) / 4 + 1, sizeof(double));
    pcount = calloc((img_end - LOAD_ADDR) / 4 + 1, sizeof(uint32_t));
    if (romp && args[24] == 0) {
        /* The Game Gear mode for a .gg image, as the console's loader
         * decides it from the file name (-gg or -sms force it). */
        size_t l = strlen(romp);

        if (l > 3 && !strcasecmp(romp + l - 3, ".gg"))
            args[24] = 1;
    }
    if (args[24] == 2)
        args[24] = 0;
    if (romp) {
        f = fopen(romp, "rb");
        if (!f) { perror(romp); return 1; }
        rom_size = fread(mem + ROM_ADDR, 1, 0x100000, f);
        fclose(f);
        /* Some dumps carry a 512-byte copier header before the image,
         * skipped as the console's loader does. */
        if (rom_size > 512 && (rom_size & 0x3FFF) == 512) {
            memmove(mem + ROM_ADDR, mem + ROM_ADDR + 512, rom_size - 512);
            rom_size -= 512;
        }
        vram_free = vram_free > rom_size ? vram_free - rom_size : 0;
    }
    if (getenv("SIMSCALE")) {
        /* Every cycle weight scaled: another host timing, to see whether
         * a run that depends on its own clock takes another course. */
        double f = atof(getenv("SIMSCALE"));
        weightS *= f; weightN *= f; weightI *= f;
    }
    if (getenv("SIMCLOCK"))
        clock_half = (uint64_t)(atof(getenv("SIMCLOCK")) / 0.08 / 2 + 0.5);
    if (getenv("SIMWATCH"))
        watch_addr = (uint32_t)strtoul(getenv("SIMWATCH"), 0, 16);
    if (getenv("SIMWATCHSKIP"))
        watch_skip = (uint32_t)strtoul(getenv("SIMWATCHSKIP"), 0, 10);
    padenv = getenv("SIMPAD");
    if (padenv) {
        char *p = padenv;
        while (*p && npad < 1024) {
            unsigned fr, bits;
            if (sscanf(p, "%u:%x", &fr, &bits) != 2) break;
            pad_frames[npad] = fr; pad_bits[npad] = bits; npad++;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    load_syms(sym);
    if (getenv("SIMTRLOG")) {
        int k;

        trlog = fopen(getenv("SIMTRLOG"), "w");
        if (!trlog) { perror(getenv("SIMTRLOG")); return 1; }
        trwords = getenv("SIMTRWORDS") != NULL;
        for (k = 0; k < TA_N; k++) {
            for (i = 0; i < nsyms; i++)
                if (!strcmp(syms[i].name, ta_name[k])) ta_addr[k] = syms[i].addr;
            if (!ta_addr[k]) fatal("SIMTRLOG: no symbol %s (build with mk.sh -labels)", ta_name[k]);
            if (ta_addr[k] < tr_lo) tr_lo = ta_addr[k];
            if (ta_addr[k] > tr_hi) tr_hi = ta_addr[k];
        }
        for (k = 0; k < TR_CALLEES; k++) {
            for (i = 0; i < nsyms; i++)
                if (!strcmp(syms[i].name, tc_name[k])) tc_addr[k] = syms[i].addr;
            if (!tc_addr[k]) fatal("SIMTRLOG: no symbol %s", tc_name[k]);
        }
        for (i = 0; i < nsyms; i++)
            if (!strcmp(syms[i].name, "jit_bctx")) tr_bctx = syms[i].addr;
        if (!tr_bctx) fatal("SIMTRLOG: no symbol jit_bctx");
    }
    for (i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, "sim_start")) r[15] = syms[i].addr;
    if (!r[15]) r[15] = LOAD_ADDR;
    r[13] = STACK_TOP;
    r[10] = STACK_TOP - 0x10000;
    r[14] = 0;
    if (getenv("SIMBLOCKLOG")) blog = fopen(getenv("SIMBLOCKLOG"), "w");
    run();
    if (blog) { block_log(bl_frame + 1); fclose(blog); }
    if (trlog) fclose(trlog);
    fprintf(stderr, "sim: %llu instructions, S %llu N %llu I %llu, %.0f us\n",
            (unsigned long long)ninsn, (unsigned long long)cS, (unsigned long long)cN,
            (unsigned long long)cI, cycles() * 0.08);
    fprintf(stderr, "sim: %u bytes of DRAM and %u bytes of VRAM left by the allocations\n",
            (unsigned)dram_free, (unsigned)vram_free);
    for (i = 1; i < 15; i += 2)
        if (mark_n[i + 1]) fprintf(stderr, "mark %d: %u spans, %.0f us average\n", i, mark_n[i + 1], mark_sum[i + 1] * 0.08 / mark_n[i + 1]);
    if (getenv("SIMPROF")) report_profile(cycles());
    if (getenv("SIMDIS")) {
        const char *list = getenv("SIMDIS");
        int k;
        for (k = 0; k < nsyms; k++) {
            const char *w = list;
            size_t n = strlen(syms[k].name);
            int hit = 0;
            while (*w) {                        /* names separated by commas */
                const char *c = strchr(w, ',');
                size_t len = c ? (size_t)(c - w) : strlen(w);
                if (len == n && !strncmp(w, syms[k].name, n)) { hit = 1; break; }
                if (!c) break;
                w = c + 1;
            }
            if (hit) {
                uint32_t a, end = k + 1 < nsyms ? syms[k + 1].addr : syms[k].addr + 256;
                if (strchr(list, ','))
                    fprintf(stderr, "dis %s\n", syms[k].name);
                for (a = syms[k].addr; a < end; a += 4)
                    fprintf(stderr, "%08x %08x %10u %12.0f\n", a, rd32(a), pcount[(a - LOAD_ADDR) >> 2], prof[(a - LOAD_ADDR) >> 2]);
            }
        }
    }
    return 0;
}
