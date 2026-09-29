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
 * Host services (SWI 0xF000nn, see hstub.s): 0 exit, 1 printf, 2 allocate,
 * 3 free (ignored), 4 microsecond clock, 5 ROM address, 6 ROM size,
 * 7 picture dump (view_NNNNNN.ppm), 8 command-line argument, 9 pad
 * buttons of a frame, 10 time marks (15: reset the profile), 11 code
 * buffer dump (code.bin).
 *
 * Options: -rom file, -frames n, -bench, -dump n, -from n, -img, -sym.
 * Environment: SIMPAD="frame:bits,..." (buttons held from that frame on,
 * SMS_PAD_* bits, 0x100 PAUSE), SIMPROF=1 (cycles per function),
 * SIMDIS=symbol (executions per instruction of a function).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

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
static uint32_t args[8] = { 0, 0, 0, 0xFFFFFFFFu };
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

static double cycles(void) { return cS * weightS + cN * weightN + cI * weightI; }
static uint32_t usec_now(void) { return (uint32_t)(cycles() * 0.08); }

static void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sim: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (pc %08x, %llu insns)\n", r[15], (unsigned long long)ninsn);
    va_end(ap);
    exit(1);
}

static inline uint32_t rd32(uint32_t a)
{
    if (a >= MEM_SIZE - 3) fatal("read32 %08x", a);
    if (a & 3) fatal("unaligned read32 %08x", a);
    return ((uint32_t)mem[a] << 24) | ((uint32_t)mem[a + 1] << 16) | ((uint32_t)mem[a + 2] << 8) | mem[a + 3];
}
static inline void wr32(uint32_t a, uint32_t v)
{
    if (a >= MEM_SIZE - 3 || a < 0x100) fatal("write32 %08x", a);
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

static void do_printf(void)
{
    char fmt[1024], out[4096], spec[32], tmp[512];
    uint32_t argn = 1;
    char *p, *o = out;

    read_str(r[0], fmt, sizeof(fmt));
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
static uint32_t pad_frames[256], pad_bits[256];
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

static void do_swi(uint32_t n)
{
    switch (n) {
    case 0: done = 1; break;
    case 1: do_printf(); break;
    case 2: {
        uint32_t size = (r[1] + 7) & ~7u;
        uint32_t a = heap;
        heap += size;
        if (heap > HEAP_END) fatal("out of heap");
        if (r[2] & 0x100u) memset(mem + a, r[2] & 0xFF, size);  /* MEMTYPE_FILL */
        else memset(mem + a, 0xA5, size);
        r[0] = a;
        break;
    }
    case 3: break;
    case 4: r[0] = usec_now(); break;
    case 5: r[0] = ROM_ADDR; break;
    case 6: r[0] = rom_size; break;
    case 7: do_dump(r[0], r[1], r[2], r[3]); break;
    case 8: r[0] = r[0] < 8 ? args[r[0]] : 0; break;
    case 9: r[0] = pad_at(r[0]); break;
    case 10: {
        uint32_t id = r[0] & 15;
        if (id == 15) { memset(jitcyc, 0, (MEM_SIZE / 4) * sizeof(float)); memset(prof, 0, ((img_end - LOAD_ADDR) / 4 + 1) * sizeof(double)); memset(pcount, 0, ((img_end - LOAD_ADDR) / 4 + 1) * sizeof(uint32_t)); prof_jit = 0; break; }
        if (id & 1) mark_t[id] = cycles();
        else { mark_sum[id] += cycles() - mark_t[id - 1]; mark_n[id]++; }
        break;
    }
    case 11: {
        FILE *f = fopen("code.bin", "wb");
        uint32_t k;
        if (f) {
            uint32_t hdr[4] = { r[0], r[1], r[2], r[3] };
            fwrite(hdr, 4, 4, f);
            fwrite(mem + r[0], 1, r[1] - r[0], f);
            for (k = 0; k < r[3]; k++) fwrite(mem + r[2] + 20 * k, 1, 20, f);
            fwrite(execmap + (r[0] >> 2), 1, (r[1] - r[0]) >> 2, f);
            fwrite(jitcyc + (r[0] >> 2), 4, (r[1] - r[0]) >> 2, f);
            fclose(f);
        }
        break;
    }
    default: fatal("unknown swi %x", n);
    }
}

/*--------------------------------------------------------------------------
 * Interpreter
 *------------------------------------------------------------------------*/

static void run(void)
{
    while (!done) {
        uint32_t pc = r[15];
        uint32_t op = rd32(pc);
        double c0 = 0;
        uint64_t s0 = cS, n0 = cN, i0 = cI;

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
        c0 = (cS - s0) * weightS + (cN - n0) * weightN + (cI - i0) * weightI;
        if (pc < img_end && pc >= LOAD_ADDR) { prof[(pc - LOAD_ADDR) >> 2] += c0; pcount[(pc - LOAD_ADDR) >> 2]++; }
        else { prof_jit += c0; execmap[pc >> 2] = 1; jitcyc[pc >> 2] += (float)c0; }
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
    for (i = 0; i < ne && i < 40; i++)
        fprintf(stderr, "  %-28s %12.0f cycles %6.2f%%\n", syms[e[i].i].name, e[i].c, 100.0 * e[i].c / total);
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    const char *img = "sim.bin", *romp = NULL, *sym = "sim.sym";
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
    }
    mem = calloc(1, MEM_SIZE);
    execmap = calloc(1, MEM_SIZE / 4);
    jitcyc = calloc(MEM_SIZE / 4, sizeof(float));
    f = fopen(img, "rb");
    if (!f) { perror(img); return 1; }
    n = fread(mem + LOAD_ADDR, 1, MEM_SIZE - LOAD_ADDR, f);
    fclose(f);
    img_end = LOAD_ADDR + ((n + 3) & ~3);
    prof = calloc((img_end - LOAD_ADDR) / 4 + 1, sizeof(double));
    pcount = calloc((img_end - LOAD_ADDR) / 4 + 1, sizeof(uint32_t));
    if (romp) {
        f = fopen(romp, "rb");
        if (!f) { perror(romp); return 1; }
        rom_size = fread(mem + ROM_ADDR, 1, 0x100000, f);
        fclose(f);
    }
    padenv = getenv("SIMPAD");
    if (padenv) {
        char *p = padenv;
        while (*p && npad < 256) {
            unsigned fr, bits;
            if (sscanf(p, "%u:%x", &fr, &bits) != 2) break;
            pad_frames[npad] = fr; pad_bits[npad] = bits; npad++;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    load_syms(sym);
    for (i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, "sim_start")) r[15] = syms[i].addr;
    if (!r[15]) r[15] = LOAD_ADDR;
    r[13] = STACK_TOP;
    r[10] = STACK_TOP - 0x10000;
    r[14] = 0;
    run();
    fprintf(stderr, "sim: %llu instructions, S %llu N %llu I %llu, %.0f us\n",
            (unsigned long long)ninsn, (unsigned long long)cS, (unsigned long long)cN,
            (unsigned long long)cI, cycles() * 0.08);
    for (i = 1; i < 15; i += 2)
        if (mark_n[i + 1]) fprintf(stderr, "mark %d: %u spans, %.0f us average\n", i, mark_n[i + 1], mark_sum[i + 1] * 0.08 / mark_n[i + 1]);
    if (getenv("SIMPROF")) report_profile(cycles());
    if (getenv("SIMDIS")) {
        const char *want = getenv("SIMDIS");
        int k;
        for (k = 0; k < nsyms; k++) if (!strcmp(syms[k].name, want)) {
            uint32_t a, end = k + 1 < nsyms ? syms[k + 1].addr : syms[k].addr + 256;
            for (a = syms[k].addr; a < end; a += 4)
                fprintf(stderr, "%08x %08x %10u\n", a, rd32(a), pcount[(a - LOAD_ADDR) >> 2]);
        }
    }
    return 0;
}
