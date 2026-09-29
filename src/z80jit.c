/*
 * Z80 to ARM translator: decoding, analysis, block management and the C
 * side of the glue. See z80jit.h for the overall design and z80jit_emit.c
 * for the generated code.
 */

#include "z80jit_int.h"

#include "string.h"

/* The offsets used by z80jit_glue.s (from the global pointer &wtab[0]). */
#define GOFF(f) (offsetof(z80j_ctx, f) - offsetof(z80j_ctx, wtab))
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(global, offsetof(z80j_ctx, wtab) == 0x400);
CHECK_OFF(regs, GOFF(regs) == 0x400);
CHECK_OFF(ix, GOFF(ix) == 0x420);
CHECK_OFF(iff1, GOFF(iff1) == 0x444);
CHECK_OFF(halted, GOFF(halted) == 0x450);
CHECK_OFF(irq_line, GOFF(irq_line) == 0x454);
CHECK_OFF(nmi, GOFF(nmi) == 0x458);
CHECK_OFF(run_end, GOFF(run_end) == 0x45C);
CHECK_OFF(line, GOFF(line) == 0x460);
CHECK_OFF(event_line, GOFF(event_line) == 0x464);
CHECK_OFF(host_sl, GOFF(host_sl) == 0x468);
CHECK_OFF(exit_reason, GOFF(exit_reason) == 0x46C);
CHECK_OFF(exit_arg, GOFF(exit_arg) == 0x470);
CHECK_OFF(state, GOFF(state) == 0x474);
CHECK_OFF(machine, GOFF(machine) == 0x478);
CHECK_OFF(resume_host, GOFF(resume_host) == 0x48C);
CHECK_OFF(resume_pc, GOFF(resume_pc) == 0x490);
CHECK_OFF(idle, GOFF(idle) == 0x498);
CHECK_OFF(port_in, GOFF(port_in) == 0x49C);
CHECK_OFF(port_out, GOFF(port_out) == 0x4A0);
CHECK_OFF(port_outn, GOFF(port_outn) == 0x4A4);
CHECK_OFF(mdata, GOFF(mdata) == 0x4A8);
CHECK_OFF(slot_bank, GOFF(slot_bank) == 0x47C);
CHECK_OFF(page_kind, GOFF(page_kind) == 0x500);
CHECK_OFF(pzst, GOFF(pzst) == Z80J_PZST_OFF);
CHECK_OFF(fenc, GOFF(fenc) == Z80J_FENC_OFF);
CHECK_OFF(fdec, GOFF(fdec) == Z80J_FDEC_OFF);
CHECK_OFF(mram, GOFF(mram) == Z80J_MRAM_OFF);
CHECK_OFF(lookup, GOFF(lookup) == Z80J_TAB_OFF);

#define SEG_MAX_T   200     /* a segment always fits in a fresh scanline */
#define SCAN_INSNS  8       /* forward scan for the flags a target reads */
#define SCAN_DEPTH  1       /* conditional jumps followed by the scan */
#define BLOCK_WORDS 3072    /* free words required before a translation */
#define HASH(pc)    ((pc) & 1023u)

/* Translation scratch (not re-entrant, kept off the small task stack). */
static jit_insn jit_insns[MAX_INSNS];
static jit_stub jit_stubs[MAX_STUBS];
static uint32   jit_last_n;

/* Port tables of the generic handlers (machine C callbacks). */
static uint32   port_in_c[Z80J_PORTS];
static uint32   port_out_c[Z80J_PORTS];
static uint32   port_outn_c[Z80J_PORTS];

/*--------------------------------------------------------------------------
 * Z80 memory and pages
 *------------------------------------------------------------------------*/

uint32 jit_byte(const z80j_ctx *ctx, uint32 addr)
{
    addr &= 0xFFFFu;
    return *(const uint8 *)JIT_PTR(ctx->rtab[255 - (addr >> 8)] + addr);
}

static int32 jit_disp(uint32 v)
{
    return (int32)((v & 0xFFu) ^ 0x80u) - 0x80;
}

/* Identity of the code at pc: page kind, and bank for a paged slot. */
uint32 jit_block_key(const z80j_ctx *ctx, uint32 pc)
{
    uint32 kind = (uint32)ctx->page_kind[(pc >> 8) & 0xFFu] & Z80J_PAGE_KIND;

    if (kind == Z80J_PAGE_FIXED)
        return KEY_FIXED;
    if (kind >= Z80J_PAGE_SLOT(0) && kind <= Z80J_PAGE_SLOT(3))
        return (kind << 16) | (ctx->slot_bank[kind - 1] & 0xFFFFu);
    return KEY_RAM;
}

/*--------------------------------------------------------------------------
 * Decoder
 *------------------------------------------------------------------------*/

/* T-states of the unprefixed opcodes (conditional ones: not taken). */
static const uint8 t_main[256] = {
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
static const uint8 cc_use[8] = {
    FL_Z, FL_Z, FL_C, FL_C, FL_PV, FL_PV, FL_S, FL_S
};

#define FL_INCDEC (FL_S | FL_Z | FL_H | FL_PV | FL_N)

static void jit_init_insn(jit_insn *in, uint32 pc)
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

static void cb_flags(jit_insn *in)
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
 * built once from main_info():
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

static uint32 dec_main[256];

static uint32 main_info(uint32 op)
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
                use = cc_use[y - 4];
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
            use = cc_use[y];
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
            use = cc_use[y];
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
            use = cc_use[y];
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
    return (uint32)t_main[op] | (kind << 5) | (use << 9) | (def << 15) |
           (nbytes << 21) | bits | (cc << 27);
}

/* Unprefixed opcode at b[0], or the opcode after a DD / FD prefix (idx 1
 * or 2); the bytes that follow are at b[1..3]. */
static void decode_main(jit_insn *in, const uint8 *b, uint32 idx)
{
    uint32 op = b[0];
    uint32 info = dec_main[op];
    uint32 kind = MI_KIND(info);
    uint32 t = MI_T(info);
    uint32 nbytes = MI_NBYTES(info);
    uint32 a = 1;
    uint32 n = 0;

    in->pre = PRE_NONE;
    in->d = 0;
    if (idx != 0) {
        if (info & MI_MEM) {
            in->d = jit_disp(b[1]);
            a = 2;
            t = (op == 0x36) ? 19 : t + 12;
        } else if (info & MI_HL) {
            t += 4;
        } else {
            /* The prefix alone: a 4 T-state no-op, the opcode is decoded
             * as the next instruction. */
            jit_init_insn(in, in->pc);
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
        in->target = (in->pc + in->len + (uint32)jit_disp(n)) & 0xFFFFu;
    else if (kind == K_JP || kind == K_JPCC || kind == K_CALL || kind == K_CALLCC)
        in->target = n;
    else if (kind == K_RST)
        in->target = op & 0x38u;
    else
        in->target = 0;
}

/* ED-prefixed opcode at b[0]. */
static void decode_ed(jit_insn *in, const uint8 *b)
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

static void jit_decode(const z80j_ctx *c, uint32 pc, jit_insn *in)
{
    const uint8 *b;
    uint8 buf[4];
    uint32 op;

    /* The (up to) four bytes of the instruction. */
    if ((pc & 0xFFu) <= 0xFCu) {
        b = (const uint8 *)JIT_PTR(c->rtab[255 - (pc >> 8)] + pc);
    } else {
        buf[0] = (uint8)jit_byte(c, pc);
        buf[1] = (uint8)jit_byte(c, pc + 1);
        buf[2] = (uint8)jit_byte(c, pc + 2);
        buf[3] = (uint8)jit_byte(c, pc + 3);
        b = buf;
    }
    in->pc = pc;
    op = b[0];
    if (op == 0xCB) {
        jit_init_insn(in, pc);
        in->pre = PRE_CB;
        in->op = b[1];
        in->len = 2;
        in->t = ((in->op & 7) != 6) ? 8 : ((in->op >> 6) == 1) ? 12 : 15;
        in->mem = ((in->op & 7) == 6);
        cb_flags(in);
    } else if (op == 0xED) {
        jit_init_insn(in, pc);
        decode_ed(in, b + 1);
    } else if (op == 0xDD || op == 0xFD) {
        uint32 op2 = b[1];
        uint32 idx = (op == 0xDD) ? 1 : 2;

        if (op2 == 0xDD || op2 == 0xFD || op2 == 0xED) {
            /* A prefix followed by another prefix: 4 T-state no-op. */
            jit_init_insn(in, pc);
        } else if (op2 == 0xCB) {
            jit_init_insn(in, pc);
            in->pre = (idx == 1) ? PRE_DDCB : PRE_FDCB;
            in->d = jit_disp(b[2]);
            in->op = b[3];
            in->len = 4;
            in->t = ((in->op >> 6) == 1) ? 20 : 23;
            in->mem = 1;
            cb_flags(in);
        } else {
            decode_main(in, b + 1, idx);
        }
    } else {
        decode_main(in, b, 0);
    }
}

void z80j_insn_info(const z80j_ctx *ctx, uint32 pc, uint32 *len, uint32 *t,
                    uint32 *t_taken)
{
    jit_insn in;

    jit_decode(ctx, pc & 0xFFFFu, &in);
    *len = in.len;
    *t = in.t;
    switch (in.kind) {
    case K_JRCC: case K_DJNZ: case K_REP: *t_taken = in.t + 5; break;
    case K_CALLCC: *t_taken = in.t + 7; break;
    case K_RETCC: *t_taken = in.t + 6; break;
    default: *t_taken = in.t; break;
    }
}

/*--------------------------------------------------------------------------
 * Flag liveness
 *------------------------------------------------------------------------*/

/* Code whose content cannot change while the block is valid. */
static int32 scan_safe(const jit_block_ctx *b, uint32 pc)
{
    uint32 key = jit_block_key(b->j->ctx, pc);

    return key == KEY_FIXED || (key == b->key && KEY_IS_SLOT(key));
}

/*
 * Flags that code starting at pc may read before writing them. Follows
 * unconditional static transfers and, up to a depth, both arms of
 * conditional jumps; anything unknown counts as reading every flag.
 */
static uint32 scan_needed(jit_block_ctx *b, uint32 pc, int32 depth)
{
    uint32 defined = 0;
    uint32 needed = 0;
    int32 k;
    jit_insn in;

    for (k = 0; k < SCAN_INSNS; k++) {
        if (!scan_safe(b, pc))
            break;
        jit_decode(b->j->ctx, pc, &in);
        needed |= in.use & ~defined;
        defined |= in.def;
        if ((defined | needed) == FL_ALL)
            return needed;
        switch (in.kind) {
        case K_JR: case K_JP: case K_CALL: case K_RST:
            pc = in.target;
            continue;
        case K_JRCC: case K_JPCC: case K_CALLCC: case K_DJNZ: case K_REP:
            if (depth <= 0)
                return needed | (FL_ALL & ~defined);
            needed |= scan_needed(b, in.target, depth - 1) & ~defined;
            break;
        case K_NORMAL:
            break;
        default:                        /* RET, RET cc, JP (HL), HALT */
            return needed | (FL_ALL & ~defined);
        }
        pc = (pc + in.len) & 0xFFFFu;
    }
    return needed | (FL_ALL & ~defined);
}

static uint32 flags_needed(jit_block_ctx *b, uint32 pc)
{
    int32 k;
    uint32 v;

    for (k = 0; k < b->nmemo; k++) {
        if (b->memo_pc[k] == pc)
            return b->memo_val[k];
    }
    v = scan_needed(b, pc, SCAN_DEPTH);
    if (b->nmemo < SCAN_MEMO) {
        b->memo_pc[b->nmemo] = pc;
        b->memo_val[b->nmemo] = v;
        b->nmemo++;
    }
    return v;
}

/* Z80 flags a producer can leave in the ARM flags (FU_*). */
static uint32 fuse_mask(const jit_insn *in)
{
    uint32 x = in->op >> 6;
    uint32 y = (in->op >> 3) & 7;
    uint32 z = in->op & 7;

    switch (in->pre) {
    case PRE_NONE:
    case PRE_DD:
    case PRE_FD:
        if (x == 2 || (x == 3 && z == 6)) {         /* ALU */
            if (y <= 1)
                return FU_Z_EQ | FU_C_CS | FU_S_MI | FU_V_VS;
            if (y <= 3 || y == 7)
                return FU_Z_EQ | FU_C_CC | FU_S_MI | FU_V_VS;
            return FU_Z_EQ | FU_S_MI;
        }
        if (x == 0 && (z == 4 || z == 5) && y != 6)  /* INC / DEC r */
            return FU_Z_EQ | FU_S_MI | FU_V_VS;
        if (x == 0 && z == 1 && (y & 1))            /* ADD HL,rr */
            return FU_C_CS;
        return 0;
    case PRE_CB:
    case PRE_DDCB:
    case PRE_FDCB:
        return (x == 1) ? FU_Z_EQ : 0;              /* BIT */
    default:                                        /* ED */
        if (x == 1 && z == 4)                       /* NEG */
            return FU_Z_EQ | FU_C_CC | FU_S_MI | FU_V_VS;
        if (x == 1 && z == 2)                       /* SBC / ADC HL,rr */
            return (y & 1) ? (FU_Z_EQ | FU_S_MI | FU_V_VS | FU_C_CS) : (FU_Z_EQ | FU_S_MI | FU_V_VS | FU_C_CC);
        return 0;
    }
}

/* FU_* a condition can use. */
static uint32 cc_fuse_need(uint32 cc)
{
    switch (cc) {
    case 0: case 1: return FU_Z_EQ | FU_Z_CS;
    case 2: case 3: return FU_C_CS | FU_C_CC;
    case 4: case 5: return FU_V_VS;
    default:        return FU_S_MI;
    }
}

/*--------------------------------------------------------------------------
 * Busy-wait loops
 *------------------------------------------------------------------------*/

/* Registers for the busy-wait analysis; flags (FL_*) from bit 16 up. */
#define BR_A   0x0001u
#define BR_B   0x0002u
#define BR_C   0x0004u
#define BR_D   0x0008u
#define BR_E   0x0010u
#define BR_H   0x0020u
#define BR_L   0x0040u
#define BR_IXH 0x0080u
#define BR_IXL 0x0100u
#define BR_IYH 0x0200u
#define BR_IYL 0x0400u
#define BR_SP  0x0800u
#define BR_FL(f) ((uint32)(f) << 16)

/* 8-bit register r (0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 7 A); H and L stand for
 * the halves of IX / IY when idx is 1 / 2. */
static uint32 br_reg8(uint32 r, uint32 idx)
{
    switch (r) {
    case 0: return BR_B;
    case 1: return BR_C;
    case 2: return BR_D;
    case 3: return BR_E;
    case 4: return (idx == 1) ? BR_IXH : (idx == 2) ? BR_IYH : BR_H;
    case 5: return (idx == 1) ? BR_IXL : (idx == 2) ? BR_IYL : BR_L;
    default: return BR_A;
    }
}

/* Register pair p: BC, DE, HL (IX / IY when idx is 1 / 2), SP. */
static uint32 br_pair(uint32 p, uint32 idx)
{
    switch (p) {
    case 0: return BR_B | BR_C;
    case 1: return BR_D | BR_E;
    case 2: return (idx == 1) ? (BR_IXH | BR_IXL) : (idx == 2) ? (BR_IYH | BR_IYL) : (BR_H | BR_L);
    default: return BR_SP;
    }
}

/*
 * Registers and flags read and written by an instruction allowed in a
 * busy-wait loop: loads, register operations, flag tests and jumps. Any
 * instruction that writes memory, accesses a port, uses the stack (except
 * RET cc, which only leaves the loop), or changes the interrupt state
 * returns 0.
 */
static int32 busy_insn(const jit_insn *in, uint32 *rd, uint32 *wr)
{
    uint32 op = in->op;
    uint32 x = op >> 6;
    uint32 y = (op >> 3) & 7;
    uint32 z = op & 7;
    uint32 p = y >> 1;
    uint32 idx = (in->pre == PRE_DD || in->pre == PRE_DDCB) ? 1u :
                 (in->pre == PRE_FD || in->pre == PRE_FDCB) ? 2u : 0u;
    uint32 ri = in->mem ? 0 : idx;      /* IX / IY halves as 8-bit operands */
    uint32 addr = br_pair(2, idx);      /* address of (HL) / (IX+d) / (IY+d) */
    uint32 r = 0;
    uint32 w = 0;

    switch (in->pre) {
    case PRE_ED:
        return 0;
    case PRE_CB:
    case PRE_DDCB:
    case PRE_FDCB:
        if (x == 1) {                               /* BIT */
            r = in->mem ? addr : br_reg8(z, 0);
        } else {
            if (in->mem)
                return 0;
            r = w = br_reg8(z, 0);
        }
        break;
    default:
        switch (x) {
        case 0:
            switch (z) {
            case 0:
                if (y == 1 || y == 2)               /* EX AF,AF', DJNZ */
                    return 0;
                break;                              /* NOP, JR, JR cc */
            case 1:
                if (y & 1) {                        /* ADD HL,rr */
                    r = br_pair(2, idx) | br_pair(p, idx);
                    w = br_pair(2, idx);
                } else {                            /* LD rr,nn */
                    if (p == 3)
                        return 0;
                    w = br_pair(p, idx);
                }
                break;
            case 2:
                switch (y) {
                case 1: r = BR_B | BR_C; w = BR_A; break;   /* LD A,(BC) */
                case 3: r = BR_D | BR_E; w = BR_A; break;   /* LD A,(DE) */
                case 5: w = br_pair(2, idx); break;         /* LD HL,(nn) */
                case 7: w = BR_A; break;                    /* LD A,(nn) */
                default: return 0;                          /* stores */
                }
                break;
            case 3:                                 /* INC / DEC rr */
                if (p == 3)
                    return 0;
                r = w = br_pair(p, idx);
                break;
            case 4:
            case 5:                                 /* INC / DEC r */
                if (y == 6)
                    return 0;
                r = w = br_reg8(y, ri);
                break;
            case 6:                                 /* LD r,n */
                if (y == 6)
                    return 0;
                w = br_reg8(y, ri);
                break;
            default:
                if (y == 4)                         /* DAA */
                    return 0;
                if (y <= 3 || y == 5)               /* rotations of A, CPL */
                    r = w = BR_A;
                break;                              /* SCF, CCF */
            }
            break;
        case 1:
            if (op == 0x76 || y == 6)               /* HALT, stores */
                return 0;
            if (z == 6) {                           /* LD r,(HL) */
                r = addr;
                w = br_reg8(y, 0);
            } else {                                /* LD r,r' */
                r = br_reg8(z, ri);
                w = br_reg8(y, ri);
            }
            break;
        case 2:                                     /* ALU A,r */
            if (z == 7 && (y == 2 || y == 5))       /* SUB A, XOR A */
                r = 0;
            else
                r = BR_A | ((z == 6) ? addr : br_reg8(z, ri));
            if (y != 7)
                w = BR_A;
            break;
        default:
            switch (z) {
            case 0:                                 /* RET cc */
                r = BR_SP;
                break;
            case 2:                                 /* JP cc */
                break;
            case 3:
                if (y != 0)
                    return 0;
                break;                              /* JP */
            case 6:                                 /* ALU A,n */
                r = BR_A;
                if (y != 7)
                    w = BR_A;
                break;
            default:
                return 0;
            }
            break;
        }
        break;
    }
    *rd = r | BR_FL(in->use);
    *wr = w | BR_FL(in->def);
    return 1;
}

/*
 * Loop from ins[h] to the jump back ins[e]. It waits for an interrupt when
 * each pass computes the same thing: every instruction is allowed by
 * busy_insn, nothing it writes is read before being written in the same
 * pass, and no other jump enters it past its head. Memory can then only
 * change through an interrupt, so the loop runs until the next one.
 */
static int32 busy_loop(const jit_insn *ins, int32 n, int32 h, int32 e)
{
    uint32 written = 0;
    uint32 exposed = 0;
    uint32 rd;
    uint32 wr;
    int32 k;

    for (k = 0; k < n; k++) {
        if (k != e && ins[k].internal > h && ins[k].internal <= e)
            return 0;
    }
    for (k = h; k <= e; k++) {
        if (!busy_insn(&ins[k], &rd, &wr))
            return 0;
        exposed |= rd & ~written;
        written |= wr;
    }
    return (exposed & written) == 0;
}

/*--------------------------------------------------------------------------
 * Block management
 *------------------------------------------------------------------------*/

uint32 z80j_block_bytes(uint32 max_blocks)
{
    return max_blocks * (uint32)sizeof(z80j_block);
}

static z80j_block *find_block(z80j_state *j, uint32 pc, uint32 key)
{
    z80j_block *b;

    for (b = j->hash[HASH(pc)]; b != 0; b = b->next) {
        if (b->pc == pc && b->key == key)
            return b;
    }
    return 0;
}

static void unhash_block(z80j_state *j, z80j_block *blk)
{
    z80j_block **pp = &j->hash[HASH(blk->pc)];

    while (*pp != 0) {
        if (*pp == blk) {
            *pp = blk->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

void z80j_flush(z80j_state *j)
{
    z80j_fill_words(j->ctx->lookup, 0x10000u, j->glue.miss);
    memset(j->hash, 0, sizeof(j->hash));
    j->nblocks = 0;
    j->cur = j->code;
    j->generation++;
    j->ctx->resume_host = 0;
    j->stats.flushes++;
}

uint32 jit_link_host(z80j_state *j, uint32 target, uint32 from_key)
{
    uint32 key = jit_block_key(j->ctx, target);
    z80j_block *blk = find_block(j, target & 0xFFFFu, key);

    if (blk == 0)
        return 0;
    /* Fixed code, and code of the same bank of the same slot, cannot
     * have changed since the source block was entered. */
    if (key == KEY_FIXED || (key == from_key && KEY_IS_SLOT(key)))
        return JIT_ADDR(blk->body);
    return JIT_ADDR(blk->entry);
}

static z80j_block *translate_block(z80j_state *j, uint32 pc, uint32 *targets,
                                   uint32 max_targets, uint32 *ntargets)
{
    z80j_ctx *ctx = j->ctx;
    jit_block_ctx bc;
    jit_insn *ins = jit_insns;
    z80j_block *blk;
    uint32 addr = pc & 0xFFFFu;
    uint32 live;
    uint32 run;
    uint32 page;
    uint32 span;
    int32 n = 0;
    jit_insn *seg;
    jit_insn *p;
    jit_insn *end;
    int32 i;
    int32 k;

    if (j->code_end - j->cur < BLOCK_WORDS || j->nblocks >= j->max_blocks)
        z80j_flush(j);

    memset(&bc, 0, sizeof(bc));
    bc.j = j;
    bc.ins = ins;
    bc.stubs = jit_stubs;
    bc.key = jit_block_key(ctx, addr);

    /* Decode up to an unconditional transfer, the instruction after EI,
     * a page of another kind or the size limit. The key only needs to be
     * checked when a page boundary is crossed. */
    page = addr >> 8;
    for (;;) {
        jit_insn *in = &ins[n++];

        jit_decode(ctx, addr, in);
        in->internal = -1;
        in->busy = 0;
        in->run = 0;
        in->seg_start = 0;
        in->fused = 0;
        addr = (addr + in->len) & 0xFFFFu;
        if (n >= 2 && in[-1].ei) {
            bc.end_leave = 1;
            break;
        }
        if (IS_END(in->kind) || n == MAX_INSNS)
            break;
        if ((addr >> 8) != page) {
            page = addr >> 8;
            if (jit_block_key(ctx, addr) != bc.key)
                break;
        }
    }
    bc.n = n;
    bc.next_pc = addr;
    jit_last_n = (uint32)n;
    end = ins + n;

    /* Jump targets inside the block become segment starts. */
    span = (addr - pc) & 0xFFFFu;
    for (p = ins; p < end; p++) {
        uint32 kd = p->kind;
        jit_insn *q;

        if (!(IS_COND(kd) && kd != K_RETCC && kd != K_CALLCC) && kd != K_JR && kd != K_JP)
            continue;
        if (((p->target - pc) & 0xFFFFu) >= span)
            continue;
        for (q = ins, k = 0; q < end; q++, k++) {
            if (q->pc == p->target) {
                p->internal = k;
                q->seg_start = 1;
                break;
            }
        }
    }

    /* Jumps back that close a busy-wait loop. */
    for (p = ins, k = 0; p < end; p++, k++) {
        uint32 kd = p->kind;

        if (p->internal >= 0 && p->internal <= k &&
            (kd == K_JRCC || kd == K_JPCC || kd == K_JR || kd == K_JP) &&
            busy_loop(ins, n, p->internal, k)) {
            p->busy = 1;
            j->stats.busy_loops++;
        }
    }

    /* Segments: block start, after each conditional instruction, and
     * whenever the running total would exceed SEG_MAX_T. */
    ins[0].seg_start = 1;
    run = 0;
    seg = ins;
    for (p = ins; p < end; p++) {
        if (p > ins && IS_COND(p[-1].kind))
            p->seg_start = 1;
        if (!p->seg_start && run + p->t > SEG_MAX_T)
            p->seg_start = 1;
        if (p->seg_start) {
            seg = p;
            run = 0;
        }
        run += p->t;
        seg->seg_t = run;
    }
    run = 0;
    for (p = ins; p < end; p++) {
        if (p->seg_start)
            run = p->seg_t;
        run -= p->t;
        p->t_after = run;
    }

    /* Runs of OUTI (or of OUTD) inside a segment become one transfer. */
    for (p = ins; p < end; p++) {
        jit_insn *q;
        uint32 len = 1;

        if (p->run != 0 || p->pre != PRE_ED || (p->op != 0xA3 && p->op != 0xAB))
            continue;
        for (q = p + 1; q < end && !q->seg_start && q->pre == PRE_ED && q->op == p->op; q++)
            len++;
        if (len >= 2) {
            p->run = len;
            for (q = p + 1; len > 1; q++, len--)
                q->run = RUN_PART;
        }
    }

    /* Flag liveness, backwards from the block exit. */
    switch (end[-1].kind) {
    case K_JR: case K_JP: case K_CALL: case K_RST:
        live = flags_needed(&bc, end[-1].target);
        break;
    case K_RET: case K_RETN: case K_JPIND: case K_HALT:
        live = FL_ALL;
        break;
    default:
        live = flags_needed(&bc, bc.next_pc);
        break;
    }
    for (p = end - 1; p >= ins; p--) {
        p->live_after = live;
        if (IS_COND(p->kind)) {
            uint32 taken;
            if (p->kind == K_RETCC)
                taken = FL_ALL;
            else if (p->internal >= 0 && &ins[p->internal] > p)
                taken = ins[p->internal].live_before;
            else
                taken = flags_needed(&bc, p->target);
            live |= p->use | taken;
        } else {
            live = (live & ~p->def) | p->use;
        }
        p->live_before = live;
    }

    /* A producer whose only reader is the next conditional jump leaves
     * its result in the ARM flags. */
    for (p = ins; p + 1 < end; p++) {
        jit_insn *c = p + 1;
        uint32 fm;
        uint32 after;

        if (c->seg_start || c->kind == K_DJNZ || c->kind == K_REP || !IS_COND(c->kind))
            continue;
        fm = fuse_mask(p) & cc_fuse_need(c->cc);
        if (fm == 0)
            continue;
        if (c->kind == K_RETCC)
            after = FL_ALL;
        else if (c->internal >= 0 && &ins[c->internal] > c)
            after = ins[c->internal].live_before;
        else
            after = flags_needed(&bc, c->target);
        after |= c->live_after;
        if ((p->def & after) == 0) {
            p->fused = fm;
            j->stats.fused++;
        }
    }

    /* Report static exits. */
    if (ntargets != 0) {
        *ntargets = 0;
        for (i = 0; i < n && *ntargets < max_targets; i++) {
            uint32 kd = ins[i].kind;
            if ((IS_COND(kd) && kd != K_RETCC && ins[i].internal < 0) || IS_STATIC(kd))
                targets[(*ntargets)++] = ins[i].target;
            /* The return address of a call ending the block. */
            if ((kd == K_CALL || kd == K_RST) && *ntargets < max_targets)
                targets[(*ntargets)++] = (ins[i].pc + ins[i].len) & 0xFFFFu;
        }
        if (*ntargets < max_targets && !IS_END(ins[n - 1].kind))
            targets[(*ntargets)++] = bc.next_pc;
    }

    /* Code. */
    blk = &j->blocks[j->nblocks];
    blk->pc = pc & 0xFFFFu;
    blk->key = bc.key;
    blk->entry = j->cur;
    bc.entry_check = (bc.key == KEY_FIXED) ? 0 : (bc.key == KEY_RAM) ? 2 : 1;
    j->error = 0;
    emit_block_code(&bc);
    if (j->error)
        return 0;
    blk->body = ins[0].host;
    j->nblocks++;
    blk->next = j->hash[HASH(blk->pc)];
    j->hash[HASH(blk->pc)] = blk;
    ctx->lookup[blk->pc] = JIT_ADDR(blk->entry);

    j->stats.translations++;
    j->stats.insns += (uint32)n;
    j->stats.code_bytes = (uint32)(j->cur - j->code) * 4;
    return blk;
}

static uint32 jit_abort(z80j_state *j, uint32 pc)
{
    j->ctx->exit_reason = Z80J_EXIT_ABORT;
    j->ctx->exit_arg = pc & 0xFFFFu;
    return j->glue.abort;
}

/* Generated code for pc under the current mapping, translated if needed. */
static uint32 host_for(z80j_state *j, uint32 pc)
{
    z80j_block *blk;

    pc &= 0xFFFFu;
    blk = find_block(j, pc, jit_block_key(j->ctx, pc));
    if (blk == 0)
        blk = translate_block(j, pc, 0, 0, 0);
    if (blk == 0)
        return jit_abort(j, pc);
    j->ctx->lookup[pc] = JIT_ADDR(blk->entry);
    return JIT_ADDR(blk->entry);
}

uint32 z80j_translate(z80j_state *j, uint32 pc)
{
    return host_for(j, pc);
}

uint32 z80j_retranslate(z80j_state *j, uint32 pc)
{
    z80j_block *old;
    uint32 generation = j->generation;
    uint32 host;

    pc &= 0xFFFFu;
    old = find_block(j, pc, KEY_RAM);
    if (old != 0)
        unhash_block(j, old);
    j->stats.retranslations++;
    host = host_for(j, pc);
    /* Links into the stale block now lead to the new one. */
    if (old != 0 && host != j->glue.abort && j->generation == generation)
        *old->entry = arm_branch(0xEu, 0, JIT_ADDR(old->entry), host);
    return host;
}

uint32 z80j_link(z80j_state *j, uint32 data)
{
    uint32 *d = (uint32 *)JIT_PTR(data);
    uint32 target = d[0] & 0xFFFFu;
    uint32 site = d[1];
    uint32 from_key = d[2];
    uint32 generation = j->generation;
    uint32 host = jit_link_host(j, target, from_key);

    if (host == 0) {
        host = host_for(j, target);
        if (host == j->glue.abort || j->generation != generation)
            return host;
        host = jit_link_host(j, target, from_key);
    }
    /* The call to the linker becomes a plain branch, and so does the
     * conditional jump that led to it. */
    d[-1] = arm_branch(0xEu, 0, data - 4, host);
    if (site != 0)
        patch_b((uint32 *)JIT_PTR(site), host);
    return host;
}

uint32 z80j_prepare(z80j_state *j, uint32 pc, uint32 *targets, uint32 max_targets,
                    uint32 *ntargets)
{
    pc &= 0xFFFFu;
    *ntargets = 0;
    if (find_block(j, pc, jit_block_key(j->ctx, pc)) != 0)
        return 0;
    if (translate_block(j, pc, targets, max_targets, ntargets) == 0)
        return 0;
    return jit_last_n;
}

/*--------------------------------------------------------------------------
 * Machine side: memory from C, interrupts, events, ports
 *------------------------------------------------------------------------*/

static void mem_write8(z80j_state *j, uint32 addr, uint32 v)
{
    z80j_ctx *ctx = j->ctx;
    uint32 e;

    addr &= 0xFFFFu;
    e = ctx->wtab[addr >> 8];
    if (e != 0) {
        *(uint8 *)JIT_PTR(e + addr) = (uint8)v;
    } else {
        z80j_machine *m = (z80j_machine *)JIT_PTR(ctx->machine);
        m->write(m, addr, v & 0xFFu);
    }
}

static void push16(z80j_state *j, uint32 v)
{
    z80j_ctx *ctx = j->ctx;
    uint32 sp = ((ctx->regs[7] >> 16) - 2) & 0xFFFFu;

    ctx->regs[7] = sp << 16;
    mem_write8(j, sp, v & 0xFFu);
    mem_write8(j, sp + 1, (v >> 8) & 0xFFu);
}

void z80j_push_slow(z80j_state *j, uint32 sp, uint32 value)
{
    sp = (sp >> 16) & 0xFFFFu;
    mem_write8(j, sp, value & 0xFFu);
    mem_write8(j, sp + 1, (value >> 8) & 0xFFu);
}

static void call_event(z80j_state *j)
{
    z80j_machine *m = (z80j_machine *)JIT_PTR(j->ctx->machine);

    m->event(m);
}

/* Takes a pending interrupt on the registers of the context; returns
 * nonzero when one was taken. */
static uint32 take_interrupt(z80j_state *j)
{
    z80j_ctx *ctx = j->ctx;
    uint32 vector;
    uint32 t;

    if (ctx->nmi) {
        ctx->nmi = 0;
        ctx->iff1 = 0;
        vector = 0x66;
        t = 11;
        j->stats.nmis++;
    } else if (ctx->irq_line && ctx->iff1) {
        ctx->iff1 = 0;
        ctx->iff2 = 0;
        j->stats.interrupts++;
        if (ctx->im == 2) {
            uint32 v = ((ctx->i & 0xFFu) << 8) | 0xFFu;
            vector = jit_byte(ctx, v) | (jit_byte(ctx, v + 1) << 8);
            t = 19;
        } else {
            vector = 0x38;              /* IM 1, or IM 0 with RST 38h */
            t = 13;
        }
    } else {
        return 0;
    }
    ctx->halted = 0;
    ctx->resume_host = 0;
    push16(j, ctx->regs[6] & 0xFFFFu);
    ctx->regs[5] -= t * CYCLE;
    ctx->regs[6] = vector;
    return 1;
}

/* Starts the next stretch of execution: up to the event line, or to the
 * end of the run if it comes first. */
static void stretch_start(z80j_ctx *ctx)
{
    uint32 lines = ctx->run_end - ctx->line;
    uint32 ev = ctx->event_line - ctx->line;

    if (ev != 0 && ev < lines)
        lines = ev;
    if (lines > MAX_STRETCH)
        lines = MAX_STRETCH;
    ctx->regs[5] += lines * (LINE_CYCLES * CYCLE);
    ctx->line += lines;
}

/* At the boundary of two stretches: runs the machine event, takes an
 * interrupt, skips the stretches spent in HALT, and returns the code to
 * run next, or 0 when the run is over. */
static uint32 boundary(z80j_state *j)
{
    z80j_ctx *ctx = j->ctx;
    uint32 host;

    for (;;) {
        if (ctx->line == ctx->run_end)
            return 0;
        if (ctx->line == ctx->event_line)
            call_event(j);
        if (take_interrupt(j)) {
            stretch_start(ctx);
            return host_for(j, ctx->regs[6]);
        }
        stretch_start(ctx);
        if (!ctx->halted)
            break;
        ctx->idle += ctx->regs[5];      /* the whole stretch goes by in HALT */
        ctx->regs[5] = 0;
    }
    host = ctx->resume_host;
    ctx->resume_host = 0;
    if (host != 0 && ctx->resume_pc == (ctx->regs[6] & 0xFFFFu))
        return host;
    return host_for(j, ctx->regs[6]);
}

uint32 z80j_entry(z80j_state *j, int32 lines)
{
    z80j_ctx *ctx = j->ctx;

    ctx->run_end = ctx->line + (uint32)(lines > 0 ? lines : 1);
    return boundary(j);
}

uint32 z80j_stretch_end(z80j_state *j)
{
    return boundary(j);
}

uint32 z80j_leave(z80j_state *j)
{
    z80j_ctx *ctx = j->ctx;
    uint32 ev = ctx->line - ctx->event_line;

    /* The machine may have moved its event line into the current stretch:
     * the stretch then ends there. */
    if (ev != 0 && ev < MAX_STRETCH && ev * (LINE_CYCLES * CYCLE) < ctx->regs[5] &&
        (int32)ctx->regs[5] > 0) {
        ctx->regs[5] -= ev * (LINE_CYCLES * CYCLE);
        ctx->line = ctx->event_line;
    }
    take_interrupt(j);
    return host_for(j, ctx->regs[6]);
}

uint32 z80j_io_write(z80j_state *j, uint32 addr, uint32 value)
{
    z80j_machine *m = (z80j_machine *)JIT_PTR(j->ctx->machine);

    return m->write(m, addr & 0xFFFFu, value & 0xFFu);
}

/*--------------------------------------------------------------------------
 * Set-up
 *------------------------------------------------------------------------*/

void z80j_reset(z80j_ctx *ctx)
{
    memset(ctx->regs, 0, sizeof(ctx->regs));
    ctx->regs[1] = 0xFF000000u;         /* A */
    ctx->regs[0] = ctx->fdec[0xFF];     /* F */
    ctx->regs[7] = 0xFFFF0000u;         /* SP */
    ctx->ix = 0xFFFF0000u;
    ctx->iy = 0xFFFF0000u;
    ctx->a2 = 0;
    ctx->f2 = 0;
    ctx->bc2 = 0;
    ctx->de2 = 0;
    ctx->hl2 = 0;
    ctx->i = 0;
    ctx->r = 0;
    ctx->iff1 = 0;
    ctx->iff2 = 0;
    ctx->im = 0;
    ctx->halted = 0;
    ctx->irq_line = 0;
    ctx->nmi = 0;
    ctx->resume_host = 0;
}

void z80j_init(z80j_state *j, z80j_ctx *ctx, uint32 *code, uint32 code_words,
               void *block_mem, uint32 max_blocks, const z80j_glue *glue)
{
    uint32 v;

    memset(j, 0, sizeof(*j));
    j->ctx = ctx;
    j->code = code;
    j->code_end = code + code_words;
    j->cur = code;
    j->blocks = (z80j_block *)block_mem;
    j->max_blocks = max_blocks;
    memcpy(&j->glue, glue, sizeof(j->glue));

    for (v = 0; v < 256; v++) {
        uint32 par = v;
        uint32 f = 0;
        uint32 e = 0;
        uint32 d = 0;

        par ^= par >> 4;
        par ^= par >> 2;
        par ^= par >> 1;
        if ((par & 1) == 0)
            f |= PSR_P;
        if (v == 0)
            f |= PSR_Z;
        if (v & 0x80)
            f |= PSR_S;
        ctx->pzst[v] = (uint8)f;

        /* internal -> F */
        if (v & PSR_S) e |= 0x80;
        if (v & PSR_Z) e |= 0x40;
        if (v & PSR_Y) e |= 0x20;
        if (v & PSR_H) e |= 0x10;
        if (v & PSR_X) e |= 0x08;
        if (v & PSR_V) e |= 0x04;
        if (v & PSR_n) e |= 0x02;
        if (v & PSR_C) e |= 0x01;
        ctx->fenc[v] = (uint8)e;

        /* F -> internal */
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

    if (dec_main[0] == 0) {
        for (v = 0; v < 256; v++)
            dec_main[v] = main_info(v);
    }
    for (v = 0; v < Z80J_PORTS; v++) {
        port_in_c[v] = JIT_ADDR(z80j_port_in_c);
        port_out_c[v] = JIT_ADDR(z80j_port_out_c);
        port_outn_c[v] = JIT_ADDR(z80j_port_outn_loop);
    }
    ctx->port_in = JIT_ADDR(port_in_c);
    ctx->port_out = JIT_ADDR(port_out_c);
    ctx->port_outn = JIT_ADDR(port_outn_c);
    ctx->mdata = 0;

    ctx->state = JIT_ADDR(j);
    ctx->exit_reason = Z80J_EXIT_LINES;
    ctx->line = 0;
    ctx->event_line = 0xFFFFFFFFu;
    z80j_flush(j);
    j->stats.flushes = 0;
    z80j_reset(ctx);
}

void z80j_default_glue(z80j_glue *glue)
{
    glue->link = JIT_ADDR(z80j_glue_link);
    glue->miss = JIT_ADDR(z80j_glue_miss);
    glue->seg_timeout = JIT_ADDR(z80j_glue_seg_timeout);
    glue->leave = JIT_ADDR(z80j_glue_leave);
    glue->halt = JIT_ADDR(z80j_glue_halt);
    glue->abort = JIT_ADDR(z80j_glue_abort);
    glue->io_in = JIT_ADDR(z80j_glue_io_in);
    glue->io_out = JIT_ADDR(z80j_glue_io_out);
    glue->io_outn = JIT_ADDR(z80j_glue_io_outn);
    glue->write = JIT_ADDR(z80j_glue_write);
    glue->verify = JIT_ADDR(z80j_glue_verify);
    glue->daa = JIT_ADDR(z80j_glue_daa);
    glue->push_slow = JIT_ADDR(z80j_glue_push_slow);
}
