/*
 * Check of the instruction decoder in assembly (src/z80jit_dec.s) against
 * the decoder it replaced, written in C (below, unchanged apart from the
 * names), run in the ARM60 model with "sim -dectest":
 * - jit_decode on every pair of opcode bytes, with operand bytes and
 *   addresses that exercise the displacements, the page ends (each byte
 *   read through its own page) and the wrap at $FFFF;
 * - jit_decode_block on random blocks over pages of every kind (fixed,
 *   paged slots, RAM with a mirror and the end of the address space as
 *   seams), against the block decoding loop of translate_block as it was:
 *   the number of instructions, the next address, the flags, the
 *   twenty-one words set per instruction and the instruction map.
 */

#include "z80jit_int.h"
#include "string.h"
#include "stdio.h"
#include "mem.h"

static uint32 ref_byte(const z80j_ctx *ctx, uint32 addr)
{
    addr &= 0xFFFFu;
    return *(const uint8 *)JIT_PTR(ctx->rtab[255 - (addr >> 8)] + addr);
}

static int32 ref_disp(uint32 v)
{
    return (int32)((v & 0xFFu) ^ 0x80u) - 0x80;
}

static const uint8 ref_t_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4,  4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4, 12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4,  7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4,  7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11,  5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11,  5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11,  5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11,  5, 6,10, 4,10, 0, 7,11
};

/* Flag read by each condition: NZ Z NC C PO PE P M. */
static const uint8 ref_cc_use[8] = {
    FL_Z, FL_Z, FL_C, FL_C, FL_PV, FL_PV, FL_S, FL_S
};

#define FL_INCDEC (FL_S | FL_Z | FL_H | FL_PV | FL_N)

static void ref_init_insn(jit_insn *in, uint32 pc)
{
    in->pc = pc;
    in->len = 1;
    in->pre = PRE_NONE;
    in->op = 0;
    in->d = 0;
    in->n = 0;
    in->t = 4;
    in->kind = K_NORMAL;
    in->cc = 0;
    in->target = 0;
    in->use = 0;
    in->def = 0;
    in->ei = 0;
    in->mem = 0;
}

static void ref_cb_flags(jit_insn *in)
{
    uint32 x = in->op >> 6;
    uint32 y = (in->op >> 3) & 7;

    if (x == 0) {
        in->def = FL_ALL;
        if (y == 2 || y == 3)
            in->use = FL_C;
    } else if (x == 1) {
        in->def = FL_INCDEC;
    }
}

/*
 * Unprefixed opcodes are decoded through a table of their properties,
 * built once from ref_main_info():
 *   bits 0-4 T-states, 5-8 kind, 9-14 flags read, 15-20 flags written,
 *   21-22 immediate bytes, 23 relative jump, 24 uses H/L, 25 uses (HL),
 *   26 EI, 27-29 condition.
 */
#define MI_T(i)      ((i) & 31u)
#define MI_KIND(i)   (((i) >> 5) & 15u)
#define MI_USE(i)    (((i) >> 9) & 63u)
#define MI_DEF(i)    (((i) >> 15) & 63u)
#define MI_NBYTES(i) (((i) >> 21) & 3u)
#define MI_REL       (1u << 23)
#define MI_HL        (1u << 24)
#define MI_MEM       (1u << 25)
#define MI_EI        (1u << 26)
#define MI_CC(i)     (((i) >> 27) & 7u)

static uint32 ref_dec_main[256];

static uint32 ref_main_info(uint32 op)
{
    uint32 x = op >> 6;
    uint32 y = (op >> 3) & 7;
    uint32 z = op & 7;
    uint32 p = y >> 1;
    uint32 q = y & 1;
    uint32 kind = K_NORMAL;
    uint32 use = 0;
    uint32 def = 0;
    uint32 cc = 0;
    uint32 nbytes = 0;      /* immediate operand bytes */
    uint32 bits = 0;        /* MI_REL, MI_HL, MI_MEM, MI_EI */

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 1) {                           /* EX AF,AF' */
                use = FL_ALL;
                def = FL_ALL;
            } else if (y == 2) {                    /* DJNZ */
                kind = K_DJNZ;
                bits = MI_REL;
            } else if (y == 3) {                    /* JR */
                kind = K_JR;
                bits = MI_REL;
            } else if (y >= 4) {                    /* JR cc */
                kind = K_JRCC;
                cc = y - 4;
                use = ref_cc_use[y - 4];
                bits = MI_REL;
            }
            break;
        case 1:
            if (q == 0)                             /* LD rr,nn */
                nbytes = 2;
            else                                    /* ADD HL,rr */
                def = FL_H | FL_N | FL_C;
            if (p == 2 || q == 1)
                bits = MI_HL;
            break;
        case 2:
            if (p >= 2)
                nbytes = 2;
            if (p == 2)
                bits = MI_HL;
            break;
        case 3:
            if (p == 2)
                bits = MI_HL;
            break;
        case 4:
        case 5:
            def = FL_INCDEC;
            if (y == 6)
                bits = MI_MEM;
            else if (y == 4 || y == 5)
                bits = MI_HL;
            break;
        case 6:
            nbytes = 1;
            if (y == 6)
                bits = MI_MEM;
            else if (y == 4 || y == 5)
                bits = MI_HL;
            break;
        default:
            switch (y) {
            case 0: case 1:                         /* RLCA RRCA */
                def = FL_H | FL_N | FL_C;
                break;
            case 2: case 3:                         /* RLA RRA */
                use = FL_C;
                def = FL_H | FL_N | FL_C;
                break;
            case 4:                                 /* DAA */
                use = FL_N | FL_H | FL_C;
                def = FL_S | FL_Z | FL_H | FL_PV | FL_C;
                break;
            case 5:                                 /* CPL */
                def = FL_H | FL_N;
                break;
            case 6:                                 /* SCF */
                def = FL_H | FL_N | FL_C;
                break;
            default:                                /* CCF */
                use = FL_C;
                def = FL_H | FL_N | FL_C;
                break;
            }
            break;
        }
        break;
    case 1:
        if (op == 0x76)
            kind = K_HALT;
        else if (y == 6 || z == 6)
            bits = MI_MEM;
        else if (y == 4 || y == 5 || z == 4 || z == 5)
            bits = MI_HL;
        break;
    case 2:
        use = (y == 1 || y == 3) ? FL_C : 0;
        def = FL_ALL;
        if (z == 6)
            bits = MI_MEM;
        else if (z == 4 || z == 5)
            bits = MI_HL;
        break;
    default:
        switch (z) {
        case 0:                                     /* RET cc */
            kind = K_RETCC;
            cc = y;
            use = ref_cc_use[y];
            break;
        case 1:
            if (q == 0) {                           /* POP */
                if (p == 2)
                    bits = MI_HL;
                if (p == 3)
                    def = FL_ALL;
            } else if (p == 0) {
                kind = K_RET;
            } else if (p == 2) {                    /* JP (HL) */
                kind = K_JPIND;
                bits = MI_HL;
            } else if (p == 3) {                    /* LD SP,HL */
                bits = MI_HL;
            }
            break;                                  /* p == 1: EXX */
        case 2:                                     /* JP cc,nn */
            kind = K_JPCC;
            cc = y;
            use = ref_cc_use[y];
            nbytes = 2;
            break;
        case 3:
            if (y == 0) {                           /* JP nn */
                kind = K_JP;
                nbytes = 2;
            } else if (y == 2 || y == 3) {          /* OUT (n),A / IN A,(n) */
                nbytes = 1;
            } else if (y == 4) {                    /* EX (SP),HL */
                bits = MI_HL;
            } else if (y == 7) {                    /* EI */
                bits = MI_EI;
            }
            break;
        case 4:                                     /* CALL cc,nn */
            kind = K_CALLCC;
            cc = y;
            use = ref_cc_use[y];
            nbytes = 2;
            break;
        case 5:
            if (q == 0) {                           /* PUSH */
                if (p == 2)
                    bits = MI_HL;
                if (p == 3)
                    use = FL_ALL;
            } else {                                /* CALL nn */
                kind = K_CALL;
                nbytes = 2;
            }
            break;
        case 6:                                     /* ALU A,n */
            nbytes = 1;
            use = (y == 1 || y == 3) ? FL_C : 0;
            def = FL_ALL;
            break;
        default:                                    /* RST */
            kind = K_RST;
            break;
        }
        break;
    }
    if (bits & MI_REL)
        nbytes = 1;
    return (uint32)ref_t_main[op] | (kind << 5) | (use << 9) | (def << 15) |
           (nbytes << 21) | bits | (cc << 27);
}

/* Unprefixed opcode at b[0], or the opcode after a DD / FD prefix (idx 1
 * or 2); the bytes that follow are at b[1..3]. */
static void ref_decode_main(jit_insn *in, const uint8 *b, uint32 idx)
{
    uint32 op = b[0];
    uint32 info = ref_dec_main[op];
    uint32 kind = MI_KIND(info);
    uint32 t = MI_T(info);
    uint32 nbytes = MI_NBYTES(info);
    uint32 a = 1;
    uint32 n = 0;

    in->pre = PRE_NONE;
    in->d = 0;
    if (idx != 0) {
        if (info & MI_MEM) {
            in->d = ref_disp(b[1]);
            a = 2;
            t = (op == 0x36) ? 19 : t + 12;
        } else if (info & MI_HL) {
            t += 4;
        } else {
            /* The prefix alone: a 4 T-state no-op, the opcode is decoded
             * as the next instruction. */
            ref_init_insn(in, in->pc);
            return;
        }
        in->pre = (idx == 1) ? PRE_DD : PRE_FD;
    }
    in->op = op;
    in->t = t;
    in->kind = kind;
    in->cc = MI_CC(info);
    in->use = MI_USE(info);
    in->def = MI_DEF(info);
    in->ei = (info & MI_EI) != 0;
    in->mem = (info & MI_MEM) != 0;
    if (nbytes == 1)
        n = b[a];
    else if (nbytes == 2)
        n = b[a] | ((uint32)b[a + 1] << 8);
    in->n = n;
    in->len = a + nbytes + (idx != 0);
    if (info & MI_REL)
        in->target = (in->pc + in->len + (uint32)ref_disp(n)) & 0xFFFFu;
    else if (kind == K_JP || kind == K_JPCC || kind == K_CALL || kind == K_CALLCC)
        in->target = n;
    else if (kind == K_RST)
        in->target = op & 0x38u;
    else
        in->target = 0;
}

/* ED-prefixed opcode at b[0]. */
static void ref_decode_ed(jit_insn *in, const uint8 *b)
{
    uint32 op = b[0];
    uint32 x = op >> 6;
    uint32 y = (op >> 3) & 7;
    uint32 z = op & 7;

    in->pre = PRE_ED;
    in->op = op;
    in->len = 2;
    in->t = 8;

    if (x == 1) {
        switch (z) {
        case 0:                                     /* IN r,(C) */
            in->t = 12;
            in->def = FL_INCDEC;
            break;
        case 1:                                     /* OUT (C),r */
            in->t = 12;
            break;
        case 2:                                     /* SBC / ADC HL,rr */
            in->t = 15;
            in->use = FL_C;
            in->def = FL_ALL;
            break;
        case 3:                                     /* LD (nn),rr / LD rr,(nn) */
            in->t = 20;
            in->len = 4;
            in->n = b[1] | ((uint32)b[2] << 8);
            break;
        case 4:                                     /* NEG */
            in->def = FL_ALL;
            break;
        case 5:                                     /* RETN / RETI */
            in->t = 14;
            in->kind = K_RETN;
            break;
        case 6:                                     /* IM */
            break;
        default:
            if (y <= 1) {                           /* LD I,A / LD R,A */
                in->t = 9;
            } else if (y <= 3) {                    /* LD A,I / LD A,R */
                in->t = 9;
                in->use = FL_C;
                in->def = FL_INCDEC;
            } else if (y <= 5) {                    /* RRD / RLD */
                in->t = 18;
                in->def = FL_INCDEC;
            }
            break;
        }
    } else if (x == 2 && z <= 3 && y >= 4) {        /* block instructions */
        in->t = 16;
        if (z == 0)
            in->def = FL_H | FL_PV | FL_N;
        else if (z == 1)
            in->def = FL_INCDEC;
        else
            in->def = FL_Z | FL_N;
        if (y >= 6) {
            in->kind = K_REP;
            in->target = in->pc;
        }
    }
}

static void ref_decode(const z80j_ctx *c, uint32 pc, jit_insn *in)
{
    const uint8 *b;
    uint8 buf[4];
    uint32 op;

    /* The (up to) four bytes of the instruction. */
    if ((pc & 0xFFu) <= 0xFCu) {
        b = (const uint8 *)JIT_PTR(c->rtab[255 - (pc >> 8)] + pc);
    } else {
        buf[0] = (uint8)ref_byte(c, pc);
        buf[1] = (uint8)ref_byte(c, pc + 1);
        buf[2] = (uint8)ref_byte(c, pc + 2);
        buf[3] = (uint8)ref_byte(c, pc + 3);
        b = buf;
    }
    in->pc = pc;
    op = b[0];
    if (op == 0xCB) {
        ref_init_insn(in, pc);
        in->pre = PRE_CB;
        in->op = b[1];
        in->len = 2;
        in->t = ((in->op & 7) != 6) ? 8 : ((in->op >> 6) == 1) ? 12 : 15;
        in->mem = ((in->op & 7) == 6);
        ref_cb_flags(in);
    } else if (op == 0xED) {
        ref_init_insn(in, pc);
        ref_decode_ed(in, b + 1);
    } else if (op == 0xDD || op == 0xFD) {
        uint32 op2 = b[1];
        uint32 idx = (op == 0xDD) ? 1 : 2;

        if (op2 == 0xDD || op2 == 0xFD || op2 == 0xED) {
            /* A prefix followed by another prefix: 4 T-state no-op. */
            ref_init_insn(in, pc);
        } else if (op2 == 0xCB) {
            ref_init_insn(in, pc);
            in->pre = (idx == 1) ? PRE_DDCB : PRE_FDCB;
            in->d = ref_disp(b[2]);
            in->op = b[3];
            in->len = 4;
            in->t = ((in->op >> 6) == 1) ? 20 : 23;
            in->mem = 1;
            ref_cb_flags(in);
        } else {
            ref_decode_main(in, b + 1, idx);
        }
    } else {
        ref_decode_main(in, b, 0);
    }
}


/* The block decoding loop of translate_block before z80jit_dec.s. */
static int32 ref_decode_block(const z80j_ctx *ctx, jit_insn *ins, uint32 pc, uint32 key,
                              uint32 *next, uint32 *flags)
{
    uint32 addr = pc & 0xFFFFu;
    uint32 page = addr >> 8;
    int32 n = 0;

    *flags = 0;
    for (;;) {
        jit_insn *in = &ins[n++];

        ref_decode(ctx, addr, in);
        in->internal = -1;
        in->busy = 0;
        in->irq_check = 0;
        in->run = 0;
        in->seg_start = 0;
        in->fused = 0;
        in->dyn = 0;
        addr = (addr + in->len) & 0xFFFFu;
        if (key == KEY_RAM &&
            (ctx->rtab[255 - (((addr - 1) & 0xFFFFu) >> 8)] != ctx->rtab[255 - (pc >> 8)] ||
             ((addr - 1) & 0xFFFFu) < (pc & 0xFFFFu))) {
            *flags |= JIT_DEC_SEAM;
            if (n == 1)
                return 0;
            n--;
            addr = in->pc;
            break;
        }
        if (key == KEY_RAM && (in->kind == K_JP || in->kind == K_JPCC ||
                               in->kind == K_CALL || in->kind == K_CALLCC)) {
            in->dyn = 1;
            *flags |= JIT_DEC_DYN;
        }
        if (key == KEY_RAM && in->pre == PRE_NONE &&
            (in->op == 0x3A || in->op == 0x32 || in->op == 0x2A || in->op == 0x22)) {
            in->dyn = 1;
            *flags |= JIT_DEC_DYN;
        }
        if (n >= 2 && in[-1].ei) {
            if (in->kind != K_NORMAL)
                in[-1].irq_check = 1;
            else
                in->irq_check = 1;
        }
        if (IS_END(in->kind) || n == MAX_INSNS)
            break;
        if ((addr >> 8) != page) {
            page = addr >> 8;
            if (jit_block_key(ctx, addr) != key)
                break;
        }
    }
    *next = addr;
    return n;
}

static uint32 seed = 12345;

static uint32 rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static uint8 *host_of(const z80j_ctx *c, uint32 addr)
{
    addr &= 0xFFFFu;
    return (uint8 *)(c->rtab[255 - (addr >> 8)] + addr);
}

/* Page p of the Z80 at host page hp of the 64 KiB buffer. */
static void map_page(z80j_ctx *c, uint8 *mem, uint32 p, uint32 hp)
{
    c->rtab[255 - p] = (uint32)(mem + hp * 256) - p * 256;
}

static jit_insn ra[MAX_INSNS];
static jit_insn rb[MAX_INSNS];

static uint32 words_differ(const jit_insn *a, const jit_insn *b, uint32 nw)
{
    const uint32 *x = (const uint32 *)a;
    const uint32 *y = (const uint32 *)b;
    uint32 k;

    for (k = 0; k < nw; k++) {
        if (x[k] != y[k])
            return k + 1;
    }
    return 0;
}

static void fill(jit_insn *r, uint32 n, uint32 v)
{
    uint32 *w = (uint32 *)r;
    uint32 k;

    for (k = 0; k < n * (sizeof(jit_insn) / 4); k++)
        w[k] = v;
}

/* Bytes biased towards the instructions the block loop treats apart. */
static uint32 code_byte(void)
{
    static const uint8 special[16] = {
        0xFB, 0xC3, 0xCD, 0x18, 0xC2, 0xDC, 0x3A, 0x32,
        0x2A, 0x22, 0xDD, 0xFD, 0xED, 0xCB, 0x00, 0x7E
    };
    uint32 r = rnd();

    if ((r & 3) == 0)
        return special[(r >> 2) & 15];
    return (r >> 6) & 0xFF;
}

static void report(const char *what, uint32 pc, uint32 bytes, uint32 k, const jit_insn *a,
                   const jit_insn *b)
{
    const uint32 *x = (const uint32 *)a;
    const uint32 *y = (const uint32 *)b;

    printf("Decoder test: %s differs at $%04lx (bytes %08lx), word %lu: C %08lx, assembly %08lx\n",
           what, (unsigned long)pc, (unsigned long)bytes, (unsigned long)k,
           (unsigned long)x[k], (unsigned long)y[k]);
}

void dec_test(void)
{
    static const uint32 pcs[12] = {
        0x1000, 0x1001, 0x1002, 0x1003, 0x10FC, 0x10FD, 0x10FE, 0x10FF,
        0xFFFE, 0xFFFF, 0x7FFD, 0x3FFF
    };
    static const uint32 ops[4] = { 0x0000, 0x7F80, 0x807F, 0xFFFF };
    z80j_ctx *c = (z80j_ctx *)AllocMem(sizeof(z80j_ctx), MEMTYPE_DRAM | MEMTYPE_FILL);
    z80j_state *j = (z80j_state *)AllocMem(sizeof(z80j_state), MEMTYPE_DRAM | MEMTYPE_FILL);
    uint32 *code = (uint32 *)AllocMem(4096, MEMTYPE_DRAM);
    void *descs = AllocMem((int32)z80j_block_bytes(4), MEMTYPE_DRAM);
    uint8 *mem = (uint8 *)AllocMem(0x10000, MEMTYPE_DRAM | MEMTYPE_FILL);
    z80j_glue glue;
    uint32 cases = 0;
    uint32 bad = 0;
    uint32 blocks = 0;
    uint32 insns = 0;
    uint32 seams = 0;
    uint32 dyns = 0;
    uint32 ei_checks = 0;
    uint32 keys_cut = 0;
    uint32 v;
    uint32 p;
    uint32 round;

    if (c == 0 || j == 0 || code == 0 || descs == 0 || mem == 0) {
        printf("Decoder test: no memory\n");
        return;
    }
    z80j_default_glue(&glue);
    z80j_init(j, c, code, 1024, 1, 0, descs, 4, 0, 0, &glue);     /* the opcode table */
    for (v = 0; v < 256; v++)
        ref_dec_main[v] = ref_main_info(v);

    /* Every pair of opcode bytes; pages scattered so that an instruction
     * at a page end reads its last bytes from elsewhere. */
    for (p = 0; p < 256; p++)
        map_page(c, mem, p, (p * 37 + 11) & 0xFF);
    for (v = 0; v < 0x10000; v++) {
        uint32 k;

        for (k = 0; k < 4; k++) {
            uint32 pc = pcs[(v + k * 5) % 12];
            uint32 bytes = (v << 16) | ops[k];
            uint32 w;

            *host_of(c, pc) = (uint8)(bytes >> 24);
            *host_of(c, pc + 1) = (uint8)(bytes >> 16);
            *host_of(c, pc + 2) = (uint8)(bytes >> 8);
            *host_of(c, pc + 3) = (uint8)bytes;
            fill(ra, 1, 0xDEADBEEFu);
            fill(rb, 1, 0x5A5A5A5Au);
            ref_decode(c, pc, &ra[0]);
            jit_decode(&rb[0], pc, c);
            cases++;
            w = words_differ(&ra[0], &rb[0], 14);
            if (w != 0 || ((uint32 *)&rb[0])[14] != 0x5A5A5A5Au) {
                if (bad++ < 10)
                    report("instruction", pc, bytes, w != 0 ? w - 1 : 14, &ra[0], &rb[0]);
            }
        }
    }
    printf("Decoder test: %lu instructions, %lu differences\n", (unsigned long)cases,
           (unsigned long)bad);

    /* Random blocks. Pages: 0-3F fixed, 40-7F slot 1, 80-BF slot 2,
     * C0-FF RAM with E0-FF mirroring C0-DF; a few odd kinds and the
     * read-fixed / write-fixed bits; layouts changing per round. */
    for (round = 0; round < 40; round++) {
        uint32 r;

        for (p = 0; p < 256; p++) {
            uint32 kind;

            if (p < 0x40)
                kind = Z80J_PAGE_FIXED;
            else if (p < 0x80)
                kind = Z80J_PAGE_SLOT(1);
            else if (p < 0xC0)
                kind = Z80J_PAGE_SLOT(2);
            else
                kind = Z80J_PAGE_RAM;
            r = rnd();
            if ((r & 15) == 0)
                kind = (r >> 4) & 15;
            if ((r & 0x300) == 0)
                kind |= Z80J_PAGE_RFIXED | Z80J_PAGE_WFIXED;
            c->page_kind[p] = (uint8)kind;
            if (p >= 0xE0 && (round & 1))
                map_page(c, mem, p, p - 0x20);
            else
                map_page(c, mem, p, (round & 2) ? p : ((p * 37 + round) & 0xFF));
        }
        for (v = 0; v < 4; v++)
            c->slot_bank[v] = rnd() & ((round & 4) ? 0xFFFFFu : 0x1Fu);
        for (v = 0; v < 0x10000; v++)
            mem[v] = (uint8)code_byte();
        for (v = 0; v < 1500; v++) {
            uint32 pc = rnd() & 0xFFFFu;
            uint32 key;
            uint32 na, nb_;
            uint32 nexta, fa;
            uint32 serial = ((round * 1500 + v) + 1) << 8;
            jit_dec_block d;
            uint32 k;
            uint32 w;

            if ((v & 7) == 0)
                pc = (pc & 0xFF00u) | (0xF0u + (pc & 15u));     /* near a page end */
            if ((v & 31) == 1)
                pc = 0xDFF0u + (pc & 15u);                      /* near the mirror */
            if ((v & 31) == 2)
                pc = 0xFFF0u + (pc & 15u);                      /* near the wrap */
            key = jit_block_key(c, pc);
            fill(ra, MAX_INSNS, 0xDEADBEEFu);
            fill(rb, MAX_INSNS, 0x5A5A5A5Au);
            na = (uint32)ref_decode_block(c, ra, pc, key, &nexta, &fa);
            d.ins = rb;
            d.pc = pc;
            d.ctx = c;
            d.key = key;
            d.serial = serial;
            d.next_pc = 0;
            d.flags = 0;
            nb_ = (uint32)jit_decode_block(&d);
            blocks++;
            insns += na;
            if (fa & JIT_DEC_SEAM)
                seams++;
            if (fa & JIT_DEC_DYN)
                dyns++;
            if (nb_ != na || (na != 0 && d.next_pc != nexta) ||
                (d.flags & (JIT_DEC_SEAM | JIT_DEC_DYN)) != fa) {
                if (bad++ < 10)
                    printf("Decoder test: block at $%04lx (key %08lx): C %lu instructions to $%04lx, "
                           "flags %lx; assembly %lu to $%04lx, flags %lx\n",
                           (unsigned long)pc, (unsigned long)key, (unsigned long)na,
                           (unsigned long)nexta, (unsigned long)fa, (unsigned long)nb_,
                           (unsigned long)d.next_pc, (unsigned long)d.flags);
                continue;
            }
            if (na == 0)
                continue;                       /* a lone instruction across a seam */
            if (na < MAX_INSNS && !IS_END(ra[na - 1].kind) && (fa & JIT_DEC_SEAM) == 0 &&
                jit_block_key(c, nexta) != key)
                keys_cut++;
            for (k = 0; k < na; k++) {
                if (ra[k].irq_check)
                    ei_checks++;
                w = words_differ(&ra[k], &rb[k], 21);
                if (w != 0) {
                    if (bad++ < 10)
                        report("block record", ra[k].pc, k, w - 1, &ra[k], &rb[k]);
                    break;
                }
                if (jit_dec_tab[256 + (ra[k].pc & 0xFFu)] != (serial | (k + 1))) {
                    if (bad++ < 10)
                        printf("Decoder test: map entry of $%04lx: %08lx, expected %08lx\n",
                               (unsigned long)ra[k].pc,
                               (unsigned long)jit_dec_tab[256 + (ra[k].pc & 0xFFu)],
                               (unsigned long)(serial | (k + 1)));
                    break;
                }
            }
        }
    }
    printf("Decoder test: %lu blocks, %lu instructions, %lu cut at a seam, %lu with operands read "
           "at run time, %lu interrupt checks after EI, %lu ended by a page of another key; "
           "%lu differences in all\n",
           (unsigned long)blocks, (unsigned long)insns, (unsigned long)seams,
           (unsigned long)dyns, (unsigned long)ei_checks, (unsigned long)keys_cut,
           (unsigned long)bad);
}
