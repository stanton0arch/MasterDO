/*
 * Z80 to ARM translator: decoding, analysis, block management and the C
 * side of the glue. See z80jit.h for the overall design and z80jit_emit.c
 * for the generated code.
 */

#include "z80jit_int.h"

#include "string.h"
#include "stdio.h"

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
CHECK_OFF(write_a, GOFF(write_a) == 0x4AC);
CHECK_OFF(wdata, GOFF(wdata) == 0x4B0);
CHECK_OFF(hot_tab, GOFF(hot_tab) == 0x4B4);
CHECK_OFF(hot_queue_at, GOFF(hot_queue_at) == 0x4B8);
CHECK_OFF(hot_sync_at, GOFF(hot_sync_at) == 0x4BC);
CHECK_OFF(ei_saved, GOFF(ei_saved) == 0x4C0);
CHECK_OFF(int_leave, GOFF(int_leave) == 0x4C4);
CHECK_OFF(int_runs, GOFF(int_runs) == 0x4C8);
CHECK_OFF(int_insns, GOFF(int_insns) == 0x4CC);
CHECK_OFF(hot_sync_gate, GOFF(hot_sync_gate) == 0x4D0);
CHECK_OFF(hot_force_at, GOFF(hot_force_at) == 0x4D4);
CHECK_OFF(chunk_mark, GOFF(chunk_mark) == 0x900);
CHECK_OFF(resume_tab, GOFF(resume_tab) == 0x4D8);
CHECK_OFF(event_a, GOFF(event_a) == 0x4DC);
CHECK_OFF(irq_count, GOFF(irq_count) == 0x4E0);
CHECK_OFF(resumed_count, GOFF(resumed_count) == 0x4E4);
CHECK_OFF(f2, GOFF(f2) == 0x428);
CHECK_OFF(bc2, GOFF(bc2) == 0x430);
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
#define BLOCK_WORDS 1024    /* free words required before a translation */
#define HASH(pc)    ((pc) & 1023u)

/* Translation scratch (not re-entrant, kept off the small task stack). */
static jit_insn jit_insns[MAX_INSNS];
static jit_stub jit_stubs[MAX_STUBS];
static uint32   jit_last_n;
/* Instruction index + 1 for each offset from the start of the block being
 * translated, tagged with the translation serial so that the map never
 * needs clearing (a block spans at most 256 bytes). */
static uint32   jit_pcmap[256];

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
    z80j_state *j = b->j;
    uint32 ckey = (pc & 0xFFFFu) | (b->key << 16);      /* pc and bank */
    uint32 ckind = b->key >> 16;                        /* page kind */
    uint32 slot = (pc ^ (pc >> 9)) & (JIT_SCAN_CACHE - 1);
    int32 k;
    uint32 v;

    for (k = 0; k < b->nmemo; k++) {
        if (b->memo_pc[k] == pc)
            return b->memo_val[k];
    }
    /* ROM code only: the scan stops at RAM anyway (unsafe), and a block
     * key of RAM is not cached. */
    if (b->key != KEY_RAM && j->scan_key[slot] == ckey && j->scan_kind[slot] == ckind) {
        v = j->scan_val[slot];
        j->stats.scan_hits++;
    } else {
        v = scan_needed(b, pc, SCAN_DEPTH);
        if (b->key != KEY_RAM) {
            j->scan_key[slot] = ckey;
            j->scan_kind[slot] = (uint8)ckind;
            j->scan_val[slot] = (uint8)v;
        }
    }
    if (b->nmemo < SCAN_MEMO) {
        b->memo_pc[b->nmemo] = pc;
        b->memo_val[b->nmemo] = v;
        b->nmemo++;
    }
    return v;
}

/* Z80 flags a producer can leave in the ARM flags (FU_*). */
/* Element of a port output run started by the ED opcode first: 1 for an
 * OUTI (an OUTD in a run of OUTD), 2 for an OUT (C),r on the run's
 * register *outc (set by the first one met), 0 when op ends the run. */
static uint32 run_elem(uint32 first, uint32 op, uint32 *outc)
{
    if (first == 0xAB)
        return op == 0xAB;
    if (op == 0xA3)
        return 1;
    if ((op == 0x79 || op == 0x51 || op == 0x59) && (*outc == 0 || *outc == op)) {
        *outc = op;
        return 2;
    }
    return 0;
}

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
static int32 busy_insn(const jit_insn *in, const uint32 *line_ports, uint32 *rd, uint32 *wr)
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
                if (y == 3 && (line_ports[(in->n & 0xFFu) >> 5] >> (in->n & 31u)) & 1u) {
                    *rd = 0;                        /* IN A,(n) from a line port */
                    *wr = BR_A;
                    return 2;
                }
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
 * change through an interrupt, so the loop runs until the next one
 * (returns 1). A loop that reads a line port computes the same thing on
 * every pass of a line, and waits for the next line (returns 2).
 */
static int32 busy_loop(const z80j_state *j, const jit_insn *ins, int32 n, int32 h, int32 e)
{
    uint32 written = 0;
    uint32 exposed = 0;
    uint32 rd;
    uint32 wr;
    int32 kind = 1;
    int32 r;
    int32 k;

    for (k = 0; k < n; k++) {
        if (k != e && ins[k].internal > h && ins[k].internal <= e)
            return 0;
    }
    for (k = h; k <= e; k++) {
        r = busy_insn(&ins[k], j->line_ports, &rd, &wr);
        if (r == 0)
            return 0;
        if (r == 2)
            kind = 2;
        exposed |= rd & ~written;
        written |= wr;
    }
    return (exposed & written) == 0 ? kind : 0;
}

/*--------------------------------------------------------------------------
 * Block management
 *------------------------------------------------------------------------*/

uint32 z80j_block_bytes(uint32 max_blocks)
{
    return max_blocks * (uint32)sizeof(z80j_block);
}

uint32 z80j_link_bytes(uint32 max_links)
{
    return max_links * 4u;
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

static uint32 zone_of(const z80j_state *j, const uint32 *host)
{
    return (uint32)(host - j->code) / j->zone_words;
}

static uint32 chunk_of(const z80j_state *j, const uint32 *host)
{
    return (uint32)(host - j->code) >> j->chunk_shift;
}

/* Chunks of zone z run within the last ageing rounds, and in *freshest
 * the age of the one run last. */
#define LIVE_AGE 2
static uint32 zone_live(const z80j_state *j, uint32 z, uint32 *freshest)
{
    uint32 c0 = (z * j->zone_words) >> j->chunk_shift;
    uint32 c1 = ((z + 1) * j->zone_words) >> j->chunk_shift;
    uint32 c;
    uint32 live = 0;
    uint32 fresh = 255;

    for (c = c0; c < c1; c++) {
        uint32 age = j->chunk_age[c];

        if (age < LIVE_AGE)
            live++;
        if (age < fresh)
            fresh = age;
    }
    *freshest = fresh;
    return live;
}

static void free_block(z80j_state *j, z80j_block *blk)
{
    j->zone_blocks[zone_of(j, blk->entry)]--;
    blk->entry = 0;
    blk->next = j->free_blocks;
    j->free_blocks = blk;
    j->nblocks--;
}

/* Target of the branch instruction at site. */
static uint32 branch_target(const uint32 *site)
{
    uint32 w = *site;
    uint32 off = (w & 0x00FFFFFFu) << 2;

    if (off & 0x02000000u)
        off |= 0xFC000000u;
    return JIT_ADDR(site) + 8 + off;
}

/*
 * Empties zone z: its blocks lose their descriptors and their lookup
 * entries, and every direct branch into the zone recorded in the link
 * log becomes a call to the linker again (a trampoline left by a
 * retranslation becomes a jump to the translator).
 */
static void evict_zone(z80j_state *j, uint32 z)
{
    z80j_ctx *ctx = j->ctx;
    uint32 lo = JIT_ADDR(j->code + z * j->zone_words);
    uint32 hi = lo + j->zone_words * 4;
    uint32 k;
    uint32 n;
    uint32 freed = 0;

    if (j->zone_blocks[z] == 0)
        return;                         /* nothing to evict: no scan */
    for (k = 0; k < j->max_blocks; k++) {
        z80j_block *blk = &j->blocks[k];
        uint32 e = JIT_ADDR(blk->entry);

        if (e >= lo && e < hi) {
            unhash_block(j, blk);
            ctx->lookup[blk->pc] = j->glue.miss;
            /* Interpreted again until it proves hot, in spare time. */
            if (j->hot != 0 && j->queue_at != 0)
                j->hot[blk->pc] = (uint8)(j->queue_at - 1);
            free_block(j, blk);
            freed++;
        }
    }
    if (freed == 0)
        return;
    j->stats.evicted += freed;
    {
        uint32 fresh;

        j->stats.evicted_live += zone_live(j, z, &fresh);
    }
    for (k = 0; k < Z80J_RESUMES; k++) {
        if (j->resume[k].host >= lo && j->resume[k].host < hi)
            j->resume[k].host = 0;
    }
    n = 0;
    for (k = 0; k < j->nlinks; k++) {
        uint32 e = j->links[k];
        uint32 *site;

        if (e & 1) {
            site = (uint32 *)JIT_PTR(e & ~1u);
            if (e - 1 >= lo && e - 1 < hi)
                continue;
            if (branch_target(site) >= lo && branch_target(site) < hi) {
                /* The trampoline of a retranslated RAM block: its target
                 * goes, so it jumps to the translator with the block's
                 * address (kept in the word after it by the entry check
                 * it replaced). */
                uint32 pc = site[1] & 0xFFFFu;
                site[0] = 0xE3A00C00u | (pc >> 8);              /* mov r0,#pc & 0xFF00 */
                site[1] = 0xE3800000u | (pc & 0xFFu);           /* orr r0,r0,#pc & 0xFF */
                site[2] = arm_branch(0xEu, 0, JIT_ADDR(site + 2), j->glue.miss);
                j->stats.unlinked++;
                continue;
            }
        } else {
            uint32 *d = (uint32 *)JIT_PTR(e);
            site = d - 1;
            if (e >= lo && e < hi)
                continue;
            if (branch_target(site) >= lo && branch_target(site) < hi) {
                *site = arm_branch(0xEu, 1, JIT_ADDR(site), j->glue.link);
                if (d[1] != 0)
                    patch_b((uint32 *)JIT_PTR(d[1]), JIT_ADDR(site));
                j->stats.unlinked++;
                continue;
            }
        }
        j->links[n++] = e;
    }
    j->nlinks = n;
    ctx->resume_host = 0;
    j->generation++;
    j->stats.evictions++;
}

/* The zone an area takes next: an empty one, else the one whose code
 * has run the least recently (the fewest live chunks, then the oldest
 * last run, then the oldest fill), whichever area owns it. The zones
 * the areas are filling are not candidates; with a single zone the area
 * takes it again (a flush). */
static uint32 pick_zone(z80j_state *j, const z80j_area *a)
{
    uint32 best = a->zone;
    uint32 best_score = 0xFFFFFFFFu;
    uint32 best_fill = 0;
    uint32 z;

    for (z = 0; z < j->nzones; z++) {
        uint32 score;
        uint32 fresh;

        if (z == j->area[0].zone || (j->area[1].count != 0 && z == j->area[1].zone))
            continue;
        if (j->zone_blocks[z] == 0)
            score = 0;
        else
            score = 1 + (zone_live(j, z, &fresh) << 8) + (255 - fresh);
        if (score < best_score || (score == best_score && j->zone_fill[z] < best_fill)) {
            best = z;
            best_score = score;
            best_fill = j->zone_fill[z];
        }
    }
    return best;
}

static void zone_forget(z80j_state *j, uint32 z);

static void area_take(z80j_state *j, z80j_area *a, uint32 z)
{
    uint32 me = (uint32)(a - j->area);
    uint32 owner = j->zone_area[z];

    if (owner != me) {
        if (owner < 2)
            j->area[owner].count--;
        a->count++;
    }
    j->zone_area[z] = (uint8)me;
    j->zone_fill[z] = j->frame_no;
    a->zone = z;
    a->cur = j->code + z * j->zone_words;
    a->end = a->cur + j->zone_words;
    zone_forget(j, z);
}

/* Moves an area on to another zone, emptied first if it holds code. */
static void next_zone(z80j_state *j, z80j_area *a)
{
    uint32 z = pick_zone(j, a);

    area_take(j, a, z);
    evict_zone(j, z);
    if (j->nzones == 1)
        j->stats.flushes++;
}

/* Turns the run marks of the chunks of zone z into ages: a chunk whose
 * mark was set since the last round is live (age 0), the others age by
 * one round. */
static void age_zone(z80j_state *j, uint32 z)
{
    z80j_ctx *ctx = j->ctx;
    uint32 c0 = (z * j->zone_words) >> j->chunk_shift;
    uint32 c1 = ((z + 1) * j->zone_words) >> j->chunk_shift;
    uint32 c;

    for (c = c0; c < c1; c++) {
        if (ctx->chunk_mark[c] != 0) {
            ctx->chunk_mark[c] = 0;
            j->chunk_age[c] = 0;
        } else if (j->chunk_age[c] < 255) {
            j->chunk_age[c]++;
        }
    }
}

/* The marks of the chunks the zone holds are cleared with the zone: its
 * new code starts with no run. */
static void zone_forget(z80j_state *j, uint32 z)
{
    uint32 c0 = (z * j->zone_words) >> j->chunk_shift;
    uint32 c1 = ((z + 1) * j->zone_words) >> j->chunk_shift;
    uint32 c;

    for (c = c0; c < c1; c++) {
        j->ctx->chunk_mark[c] = 0;
        j->chunk_age[c] = 255;
    }
}

void z80j_flush(z80j_state *j)
{
    uint32 k;
    uint32 nhot = j->area[1].count;
    uint32 c;

    z80j_fill_words(j->ctx->lookup, 0x10000u, j->glue.miss);
    memset(j->hash, 0, sizeof(j->hash));
    memset(j->seen, 0, sizeof(j->seen));
    memset(j->flaky, 0, sizeof(j->flaky));
    memset(j->zone_blocks, 0, sizeof(j->zone_blocks));
    memset(j->scan_kind, 0xFF, sizeof(j->scan_kind));
    j->free_blocks = 0;
    for (k = j->max_blocks; k > 0; k--) {
        z80j_block *blk = &j->blocks[k - 1];
        blk->entry = 0;
        blk->next = j->free_blocks;
        j->free_blocks = blk;
    }
    j->nblocks = 0;
    j->nlinks = 0;
    /* The cold area starts at the first zone, the hot one at the last;
     * the zones between are taken as code comes. */
    memset(j->zone_area, 0xFF, sizeof(j->zone_area));
    memset(j->zone_fill, 0, sizeof(j->zone_fill));
    for (c = 0; c < Z80J_MARK_CHUNKS; c++) {
        j->ctx->chunk_mark[c] = 0;
        j->chunk_age[c] = 255;
    }
    j->area[0].count = 0;
    j->area[1].count = 0;
    area_take(j, &j->area[0], 0);
    if (nhot != 0 && j->nzones >= 3)
        area_take(j, &j->area[1], j->nzones - 1);
    j->cur = j->area[0].cur;
    j->zone_end = j->area[0].end;
    j->nqueue = 0;
    memset(j->queued, 0, sizeof(j->queued));
    memset(j->resume, 0, sizeof(j->resume));
    j->generation++;
    j->ctx->resume_host = 0;
    j->stats.flushes++;
}

uint32 jit_link_host(z80j_state *j, uint32 target, uint32 from_key, uint32 from_site)
{
    uint32 key = jit_block_key(j->ctx, target);
    z80j_block *blk = find_block(j, target & 0xFFFFu, key);

    if (blk == 0)
        return 0;
    /* Fixed code, and code of the same bank of the same slot, cannot
     * have changed since the source block was entered: the link skips
     * the entry check, and, from the same zone, the run mark too (the
     * zone is marked by whatever entered it from outside). */
    if (key == KEY_FIXED || (key == from_key && KEY_IS_SLOT(key))) {
        if (zone_of(j, (const uint32 *)JIT_PTR(from_site)) == zone_of(j, blk->body))
            return JIT_ADDR(blk->body + 1);
        return JIT_ADDR(blk->body);
    }
    return JIT_ADDR(blk->entry);
}

void jit_link_stub(z80j_state *j, uint32 *d, uint32 host)
{
    if (j->nlinks >= j->max_links) {
        j->stats.link_full++;
        return;
    }
    d[-1] = arm_branch(0xEu, 0, JIT_ADDR(d - 1), host);
    if (d[1] != 0)
        patch_b((uint32 *)JIT_PTR(d[1]), host);
    j->links[j->nlinks++] = JIT_ADDR(d);
}

/* Queues a hot address for translation in spare time (a bitmap tells
 * the addresses already queued); when the queue is full, the address
 * replaces the first entry found colder than itself, if any. */
static void queue_hot(z80j_state *j, uint32 pc)
{
    uint32 bit;
    uint32 k;

    pc &= 0xFFFFu;
    bit = (uint32)1 << (pc & 31);
    if (j->queued[pc >> 5] & bit)
        return;
    if (j->nqueue < Z80J_QUEUE) {
        j->queue[j->nqueue++] = pc;
        j->queued[pc >> 5] |= bit;
        return;
    }
    for (k = 0; k < Z80J_QUEUE; k++) {
        uint32 old = j->queue[k];

        if (j->hot[old] < j->hot[pc]) {
            j->queued[old >> 5] &= ~((uint32)1 << (old & 31));
            j->queue[k] = pc;
            j->queued[pc >> 5] |= bit;
            return;
        }
    }
    j->stats.queue_full++;
}

static uint32 host_force(z80j_state *j, uint32 pc);

/* Translations at once: one is allowed when the instructions the frame
 * has interpreted have paid for it (their estimated cost covers the time
 * already spent plus the usual cost of a translation), or, up to the
 * floor per frame, when the frame still has room for it before the
 * time it reserves after the emulation. When refused, the interpreter
 * is told from how many more interpreted instructions on it may ask
 * again (hot_sync_gate). */
static uint32 sync_allowed(z80j_state *j)
{
    z80j_ctx *ctx = j->ctx;
    uint32 interpreted = ctx->int_insns - j->int_insns0;
    uint32 paid = (interpreted * j->int_cost) >> 4;
    uint32 need = j->sync_spent_us + j->est_us;

    if (j->sync_at == 0) {
        ctx->hot_sync_gate = 0xFFFFFFFFu;
        return 0;
    }
    if (need <= paid)
        return 1;
    if (j->sync_spent_us < j->sync_floor_us) {
        uint32 elapsed = (j->clock != 0) ? j->clock() - j->frame_start_us : 0;

        if (elapsed + j->est_us + j->reserve_us <= j->frame_us)
            return 1;
    }
    j->stats.sync_refused++;
    if (j->int_cost != 0)
        ctx->hot_sync_gate = ctx->int_insns + ((need - paid) * 16) / j->int_cost + 1;
    else
        ctx->hot_sync_gate = 0xFFFFFFFFu;
    return 0;
}

/* Translates pc now and keeps the running average of the cost. */
static uint32 timed_translate(z80j_state *j, uint32 pc, uint32 *dt)
{
    uint32 t0 = (j->clock != 0) ? j->clock() : 0;
    uint32 host = host_force(j, pc);

    *dt = (j->clock != 0) ? j->clock() - t0 : j->est_us;
    j->est_us = (j->est_us * 3 + *dt) >> 2;
    return host;
}

static uint32 sync_translate(z80j_state *j, uint32 pc)
{
    uint32 dt;
    uint32 host = timed_translate(j, pc, &dt);

    j->sync_spent_us += dt;
    j->stats.sync_us += dt;
    j->stats.sync++;
    return host;
}

uint32 z80j_spare_fits(const z80j_state *j, uint32 elapsed_us, uint32 margin_us)
{
    return elapsed_us + j->est_us + (j->est_us >> 1) + margin_us <= j->frame_us;
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
    uint32 serial;
    z80j_area *area;
    int32 n = 0;
    jit_insn *seg;
    jit_insn *p;
    jit_insn *end;
    int32 i;
    int32 k;

    /* Area: hot for a block translated before (evicted since), cold
     * otherwise; the zone is emptied when it cannot take another block
     * (an eviction may also be needed for a descriptor). */
    {
        uint32 bit = (uint32)1 << (addr & 31);
        uint32 hot = (j->seen[addr >> 5] & bit) != 0 && j->area[1].count != 0;

        if (hot)
            j->stats.promoted++;
        else if (j->seed_hot && j->area[1].count != 0)
            hot = 1;
        j->seen[addr >> 5] |= bit;
        area = &j->area[hot];
        if (area->end - area->cur < BLOCK_WORDS || j->free_blocks == 0)
            next_zone(j, area);
        j->cur = area->cur;
        j->zone_end = area->end;
    }
    bc.mark_off = (uint32)GOFF(chunk_mark) + 4 * chunk_of(j, j->cur);

    bc.j = j;
    bc.ins = ins;
    bc.stubs = jit_stubs;
    bc.nstubs = 0;
    bc.end_leave = 0;
    bc.has_dyn = 0;
    bc.nmemo = 0;
    bc.key = jit_block_key(ctx, addr);
    serial = (j->stats.translations + 1) << 8;

    /* Decode up to an unconditional transfer, a page of another kind or
     * the size limit. The instruction after EI gets an interrupt check,
     * unless it is a transfer or a conditional instruction: the block
     * then ends after it and is left, so that a pending interrupt is
     * taken. The key only needs to be checked when a page boundary is
     * crossed. */
    page = addr >> 8;
    for (;;) {
        jit_insn *in = &ins[n++];

        jit_pcmap[(addr - pc) & 0xFFu] = serial | (uint32)n;
        jit_decode(ctx, addr, in);
        in->internal = -1;
        in->busy = 0;
        in->irq_check = 0;
        in->run = 0;
        in->seg_start = 0;
        in->fused = 0;
        in->dyn = 0;
        addr = (addr + in->len) & 0xFFFFu;
        /* The entry check of a RAM block reads the memory holding it as
         * consecutive host words from the one holding its first byte
         * (z80j_glue_verify): the block must not reach a page whose host
         * memory does not follow (the mirror of the system RAM, the end
         * of the address space). An instruction crossing such a seam
         * ends the block before it; alone, it is left to the interpreter. */
        if (bc.key == KEY_RAM &&
            (ctx->rtab[255 - (((addr - 1) & 0xFFFFu) >> 8)] != ctx->rtab[255 - (pc >> 8)] ||
             ((addr - 1) & 0xFFFFu) < (pc & 0xFFFFu))) {
            j->stats.seams++;
            if (n == 1) {
                j->error = 5;
                return 0;
            }
            n--;
            addr = in->pc;
            break;
        }
        /* JP and CALL in RAM read their target when they run: a jump
         * vector whose operand the game rewrites (an interrupt
         * trampoline, typically) keeps its translation. */
        if (bc.key == KEY_RAM && (in->kind == K_JP || in->kind == K_JPCC ||
                                  in->kind == K_CALL || in->kind == K_CALLCC)) {
            in->dyn = 1;
            bc.has_dyn = 1;
        }
        /* So do the absolute addresses of LD A,(nn), LD (nn),A,
         * LD HL,(nn) and LD (nn),HL: a handler stepping through a table
         * by patching its own operand keeps its translation too. */
        if (bc.key == KEY_RAM && in->pre == PRE_NONE &&
            (in->op == 0x3A || in->op == 0x32 || in->op == 0x2A || in->op == 0x22)) {
            in->dyn = 1;
            bc.has_dyn = 1;
        }
        if (n >= 2 && in[-1].ei) {
            /* A pending interrupt is taken after the instruction that
             * follows EI; when that instruction is a transfer or a
             * conditional, the check comes right after EI instead (the
             * interrupt is then taken one instruction early, before the
             * transfer, as the hardware does for a HALT). */
            if (in->kind != K_NORMAL)
                in[-1].irq_check = 1;
            else
                in->irq_check = 1;
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
        uint32 off;
        uint32 m;

        if (!(IS_COND(kd) && kd != K_RETCC && kd != K_CALLCC) && kd != K_JR && kd != K_JP)
            continue;
        if (p->dyn)
            continue;
        off = (p->target - pc) & 0xFFFFu;
        if (off >= span)
            continue;
        m = jit_pcmap[off];
        if ((m & ~0xFFu) == serial) {
            k = (int32)(m & 0xFFu) - 1;
            p->internal = k;
            ins[k].seg_start = 1;
        }
    }

    /* Jumps back that close a busy-wait loop. */
    for (p = ins, k = 0; p < end; p++, k++) {
        uint32 kd = p->kind;

        if (p->internal >= 0 && p->internal <= k &&
            (kd == K_JRCC || kd == K_JPCC || kd == K_JR || kd == K_JP) &&
            (p->busy = (uint32)busy_loop(j, ins, n, p->internal, k)) != 0) {
            j->stats.busy_loops++;
            if (p->busy == 2)
                j->stats.line_loops++;
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

    /* Runs of port outputs inside a segment become one transfer: OUTD
     * alone, or OUTI mixed with OUT (C),r for one register r among A, D
     * and E, which the run does not change (a byte written between the
     * bytes read from memory, as in an upload of three bit planes and a
     * constant fourth one). A run stops after the instruction that
     * follows EI, whose interrupt check comes after the run. */
    for (p = ins; p < end; p++) {
        jit_insn *q;
        uint32 len = 0;
        uint32 mask = 0;
        uint32 outc = 0;
        uint32 e;

        if (p->run != 0 || p->pre != PRE_ED)
            continue;
        e = run_elem(p->op, p->op, &outc);
        for (q = p; e != 0; ) {
            if (e == 2)
                mask |= (uint32)1 << len;
            len++;
            if (q->irq_check || len == Z80J_RUN_MAX)
                break;
            q++;
            if (q >= end || q->seg_start || q->pre != PRE_ED)
                break;
            e = run_elem(p->op, q->op, &outc);
        }
        if (len >= 2) {
            p->run = len | (mask << 8) | (outc << 24);
            for (q = p + 1; q < p + len; q++)
                q->run = RUN_PART;
        }
    }

    /* An OUTI (OUTD) closed by JR NZ or JP NZ back to it sends B bytes,
     * as OTIR (OTDR) does: the passes go out in one run when the stretch
     * has room for all of them (emit_out_loop). */
    for (p = ins, k = 0; p + 1 < end; p++, k++) {
        jit_insn *c = p + 1;

        if (p->run == 0 && p->pre == PRE_ED && (p->op == 0xA3 || p->op == 0xAB) &&
            !p->irq_check && (c->kind == K_JRCC || c->kind == K_JPCC) && c->cc == 0 &&
            c->internal == k && !c->dyn && !c->busy && !c->irq_check)
            p->run = RUN_LOOP;
    }

    /* Flag liveness, backwards from the block exit. */
    switch (end[-1].kind) {
    case K_JR: case K_JP: case K_CALL: case K_RST:
        live = end[-1].dyn ? FL_ALL : flags_needed(&bc, end[-1].target);
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
            if (p->kind == K_RETCC || p->dyn)
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

        if (c->seg_start || c->kind == K_DJNZ || c->kind == K_REP || !IS_COND(c->kind) ||
            p->irq_check)
            continue;
        fm = fuse_mask(p) & cc_fuse_need(c->cc);
        if (fm == 0)
            continue;
        if (c->kind == K_RETCC || c->dyn)
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
            if (ins[i].dyn)
                continue;
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
    blk = j->free_blocks;
    blk->pc = pc & 0xFFFFu;
    blk->key = bc.key;
    blk->entry = j->cur;
    bc.entry_check = (bc.key == KEY_FIXED) ? 0 : (bc.key == KEY_RAM) ? 2 : 1;
    j->error = 0;
    emit_block_code(&bc);
    if (j->error == 4) {
        /* The zone was too full for this block: emit it again in the
         * next one. */
        next_zone(j, area);
        blk = j->free_blocks;
        blk->pc = pc & 0xFFFFu;
        blk->key = bc.key;
        blk->entry = j->cur = area->cur;
        j->zone_end = area->end;
        bc.mark_off = (uint32)GOFF(chunk_mark) + 4 * chunk_of(j, j->cur);
        j->error = 0;
        emit_block_code(&bc);
    }
    if (j->error) {
        printf("Z80 translator: block at $%04lx not translated (error %ld, %ld instructions)\n",
               (unsigned long)blk->pc, (long)j->error, (long)n);
        blk->entry = 0;
        return 0;
    }
    area->cur = j->cur;
    j->free_blocks = blk->next;
    j->nblocks++;
    j->zone_blocks[zone_of(j, blk->entry)]++;
    blk->body = bc.body;
    blk->next = j->hash[HASH(blk->pc)];
    j->hash[HASH(blk->pc)] = blk;
    ctx->lookup[blk->pc] = JIT_ADDR(blk->entry);

    j->stats.translations++;
    if (bc.has_dyn)
        j->stats.dyn_blocks++;
    j->stats.insns += (uint32)n;
    j->stats.code_bytes += (uint32)(j->cur - blk->entry) * 4;
    return blk;
}

static uint32 take_interrupt(z80j_state *j);
static uint32 boundary(z80j_state *j);

static uint32 jit_abort(z80j_state *j, uint32 pc)
{
    j->ctx->exit_reason = Z80J_EXIT_ABORT;
    j->ctx->exit_arg = pc & 0xFFFFu;
    return j->glue.abort;
}

/* Generated code for pc under the current mapping, translated now. */
static uint32 host_force(z80j_state *j, uint32 pc)
{
    z80j_block *blk;

    pc &= 0xFFFFu;
    blk = find_block(j, pc, jit_block_key(j->ctx, pc));
    if (blk == 0) {
        j->error = 0;
        blk = translate_block(j, pc, 0, 0, 0);
    }
    if (blk == 0) {
        /* Not translatable (a RAM instruction across a seam): interpreted
         * for good when there is an interpreter. */
        if (j->error == 5 && j->hot != 0) {
            j->hot[pc] = 255;
            return j->glue.interp;
        }
        return jit_abort(j, pc);
    }
    j->ctx->lookup[pc] = JIT_ADDR(blk->entry);
    return JIT_ADDR(blk->entry);
}

/* Code to run for pc: its translation when it exists, else, with the
 * interpreter, the interpreter entry until the address has proven hot
 * (queued in the meantime), else a fresh translation. */
static uint32 host_for(z80j_state *j, uint32 pc)
{
    z80j_block *blk;
    uint32 host;
    uint32 key;

    pc &= 0xFFFFu;
    /* Fixed and RAM code have one key: their lookup entry, when set,
     * is the block wanted (a RAM block checks its bytes on entry). A
     * paged slot may hold the entry of another bank, whose entry check
     * would come straight back here: those go through the hash. */
    key = jit_block_key(j->ctx, pc);
    host = j->ctx->lookup[pc];
    if (host != j->glue.miss && !KEY_IS_SLOT(key))
        return host;
    /* The return from an interrupt taken at a segment check: the block
     * goes on from there, under the mapping it was interrupted with. */
    {
        const z80j_resume *e = &j->resume[Z80J_RESUME_SLOT(pc)];

        if (e->host != 0 && e->pc == pc && e->key == key) {
            j->stats.resumed++;
            return e->host;
        }
    }
    blk = find_block(j, pc, key);
    if (blk != 0) {
        j->ctx->lookup[pc] = JIT_ADDR(blk->entry);
        return JIT_ADDR(blk->entry);
    }

    if (j->hot != 0) {
        uint32 n = j->hot[pc];

        if (n == 255)
            return j->glue.interp;      /* parked: interpreted for good */
        j->hot[pc] = (uint8)++n;
        if (j->queue_at == 0)
            return j->glue.interp;      /* interpretation only */
        if (n < j->sync_at || ((j->force_at == 0 || n < j->force_at) && !sync_allowed(j))) {
            if (n >= j->queue_at)
                queue_hot(j, pc);
            return j->glue.interp;
        }
        return sync_translate(j, pc);
    }
    return host_force(j, pc);
}

uint32 z80j_prefetch(z80j_state *j)
{
    while (j->nqueue != 0) {
        uint32 best = 0;
        uint32 pc;
        uint32 k;

        for (k = 1; k < j->nqueue; k++) {
            if (j->hot[j->queue[k]] > j->hot[j->queue[best]])
                best = k;
        }
        pc = j->queue[best];
        j->queue[best] = j->queue[--j->nqueue];
        j->queued[pc >> 5] &= ~((uint32)1 << (pc & 31));
        if (find_block(j, pc, jit_block_key(j->ctx, pc)) != 0)
            continue;
        {
            uint32 t0 = (j->clock != 0) ? j->clock() : 0;

            j->error = 0;
            if (translate_block(j, pc, 0, 0, 0) != 0)
                j->stats.prefetched++;
            else if (j->error == 5)
                j->hot[pc] = 255;       /* not translatable: interpreted for good */
            if (j->clock != 0)
                j->est_us = (j->est_us * 3 + (j->clock() - t0)) >> 2;
        }
        return 1;
    }
    return 0;
}

void z80j_set_interp(z80j_state *j, uint8 *hot, uint32 queue_at, uint32 sync_at)
{
    j->hot = hot;
    j->queue_at = queue_at;
    j->sync_at = sync_at;
    if (hot != 0)
        memset(hot, 0, 0x10000);
    j->ctx->hot_tab = JIT_ADDR(hot);
    j->ctx->hot_queue_at = queue_at;
    j->ctx->hot_sync_at = sync_at;
    j->ctx->ei_saved = 0;
    j->ctx->int_leave = 0;
    j->ctx->hot_sync_gate = 0;
    j->nqueue = 0;
    memset(j->queued, 0, sizeof(j->queued));
}

void z80j_set_line_port(z80j_state *j, uint32 port)
{
    j->line_ports[(port & 0xFFu) >> 5] |= (uint32)1 << (port & 31u);
}

void z80j_set_force(z80j_state *j, uint32 force_at)
{
    j->force_at = force_at;
    j->ctx->hot_force_at = force_at;
}

void z80j_set_budget(z80j_state *j, uint32 (*clock)(void), uint32 floor_us,
                     uint32 int_cost, uint32 frame_us)
{
    j->clock = clock;
    j->sync_floor_us = floor_us;
    j->int_cost = int_cost;
    j->frame_us = frame_us;
    if (j->est_us == 0)
        j->est_us = 2000;
}

void z80j_frame_start(z80j_state *j, uint32 start_us, uint32 reserve_us)
{
    j->frame_no++;
    age_zone(j, j->frame_no % j->nzones);
    j->sync_spent_us = 0;
    j->int_insns0 = j->ctx->int_insns;
    j->frame_start_us = start_us;
    j->reserve_us = reserve_us;
    j->ctx->hot_sync_gate = 0;
}

uint32 z80j_translate(z80j_state *j, uint32 pc)
{
    return host_for(j, pc);
}

/* The machine may have moved its event line into the current stretch
 * (the stretch then ends there) or raised an interrupt line: taken now. */
static void leave_check(z80j_state *j)
{
    z80j_ctx *ctx = j->ctx;
    uint32 ev = ctx->line - ctx->event_line;

    if (ev != 0 && ev < MAX_STRETCH && ev * (LINE_CYCLES * CYCLE) < ctx->regs[5] &&
        (int32)ctx->regs[5] > 0) {
        ctx->regs[5] -= ev * (LINE_CYCLES * CYCLE);
        ctx->line = ctx->event_line;
    }
    take_interrupt(j);
}

/* The interpreter counted an entry of regs[6] up to a threshold: the
 * address is translated at once when it is hot enough and the frame
 * still allows it, else queued for spare time and interpreted on. */
uint32 z80j_hot(z80j_state *j)
{
    uint32 pc = j->ctx->regs[6] & 0xFFFFu;

    if (j->queue_at == 0 || j->hot[pc] == 255) {
        j->ctx->hot_sync_gate = 0xFFFFFFFFu;    /* interpretation only, or parked */
        return j->glue.interp;
    }
    if (j->hot[pc] >= j->sync_at &&
        ((j->force_at != 0 && j->hot[pc] >= j->force_at) || sync_allowed(j)))
        return sync_translate(j, pc);
    queue_hot(j, pc);
    return j->glue.interp;
}

/* Does the RAM block still describe the code at its address? Its entry
 * check holds the address, the length and the bytes it was made from. */
/* The entry check of a RAM block, as z80j_glue_verify makes it: the host
 * words from the one holding the first byte of the block, against the
 * bytes and the mask recorded after the call. */
static uint32 block_bytes_match(const z80j_ctx *ctx, const z80j_block *blk)
{
    const uint32 *h = blk->entry;
    uint32 pc = h[1];
    uint32 nwords = h[2];
    const uint32 *mem = (const uint32 *)JIT_PTR((ctx->rtab[255 - (pc >> 8)] + pc) & ~3u);
    const uint32 *bytes = h + 3;
    const uint32 *mask = bytes + nwords;
    uint32 k;

    if ((h[0] & 0x0F000000u) != 0x0B000000u)    /* not a bl: a trampoline */
        return 0;
    for (k = 0; k < nwords; k++) {
        if (((mem[k] ^ bytes[k]) & mask[k]) != 0)
            return 0;
    }
    return 1;
}

uint32 z80j_retranslate(z80j_state *j, uint32 pc)
{
    z80j_block *old;
    uint32 generation = j->generation;
    uint32 host;

    pc &= 0xFFFFu;
    old = find_block(j, pc, KEY_RAM);
    /* A stale entry reached through an old link (its trampoline gone
     * with an eviction) while the current translation is valid. */
    if (old != 0 && block_bytes_match(j->ctx, old)) {
        j->ctx->lookup[pc] = JIT_ADDR(old->entry);
        return JIT_ADDR(old->entry);
    }
    if (old != 0) {
        unhash_block(j, old);
        j->ctx->lookup[pc] = j->glue.miss;  /* host_for must not return the stale entry */
    }
    j->stats.retranslations++;
    /* Code rewritten since its translation starts again as cold; code
     * found rewritten a second time (a handler patched on every line,
     * for instance) is interpreted from then on: its counter is parked
     * where the interpreter's thresholds never match. */
    if (j->hot != 0) {
        uint32 bit = (uint32)1 << (pc & 31);

        if (j->flaky[pc >> 5] & bit) {
            j->hot[pc] = 255;
        } else {
            j->flaky[pc >> 5] |= bit;
            j->hot[pc] = 0;
        }
    }
    host = host_for(j, pc);
    if (host == j->glue.interp) {
        j->ctx->regs[6] = pc;
        return 0;
    }
    /* Links into the stale block now lead to the new one, through a
     * trampoline that an eviction of the new block undoes. */
    if (old != 0 && host != j->glue.abort && j->generation == generation) {
        if (j->nlinks < j->max_links) {
            *old->entry = arm_branch(0xEu, 0, JIT_ADDR(old->entry), host);
            j->links[j->nlinks++] = JIT_ADDR(old->entry) | 1u;
        } else {
            j->stats.link_full++;
        }
    }
    return host;
}

uint32 z80j_link(z80j_state *j, uint32 data)
{
    uint32 *d = (uint32 *)JIT_PTR(data);
    uint32 target = d[0] & 0xFFFFu;
    uint32 site = d[1];
    uint32 from_key = d[2];
    uint32 generation = j->generation;
    uint32 host = jit_link_host(j, target, from_key, data);

    if (host == 0) {
        host = host_for(j, target);
        if (host == j->glue.interp) {
            j->ctx->regs[6] = target;
            return 0;
        }
        if (host == j->glue.abort || j->generation != generation)
            return host;
        /* A return resumed inside a block is run, not linked. */
        if (jit_link_host(j, target, from_key, data) == 0)
            return host;
        host = jit_link_host(j, target, from_key, data);
    }
    /* The call to the linker becomes a plain branch, and so does the
     * conditional jump that led to it. */
    (void)site;
    jit_link_stub(j, d, host);
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
    uint32 sp1 = (sp + 1) & 0xFFFFu;
    uint32 e0 = ctx->wtab[sp >> 8];
    uint32 e1 = ctx->wtab[sp1 >> 8];

    ctx->regs[7] = sp << 16;
    if (e0 != 0 && e1 != 0) {
        *(uint8 *)JIT_PTR(e0 + sp) = (uint8)v;
        *(uint8 *)JIT_PTR(e1 + sp1) = (uint8)(v >> 8);
    } else {
        mem_write8(j, sp, v & 0xFFu);
        mem_write8(j, sp1, (v >> 8) & 0xFFu);
    }
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
    if (ctx->resume_host != 0 && ctx->resume_pc == (ctx->regs[6] & 0xFFFFu)) {
        uint32 key = jit_block_key(ctx, ctx->resume_pc);

        /* Not for code in RAM: the handler may rewrite it, and only the
         * entry check of a block notices. */
        if (key != KEY_RAM) {
            z80j_resume *e = &j->resume[Z80J_RESUME_SLOT(ctx->resume_pc)];

            e->pc = ctx->resume_pc;
            e->host = ctx->resume_host;
            e->key = key;
        }
    }
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
    leave_check(j);
    return host_for(j, j->ctx->regs[6]);
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
               uint32 nzones, uint32 nhot, void *block_mem, uint32 max_blocks,
               uint32 *link_mem, uint32 max_links, const z80j_glue *glue)
{
    uint32 v;

    memset(j, 0, sizeof(*j));
    j->ctx = ctx;
    j->code = code;
    j->code_end = code + code_words;
    j->nzones = (nzones != 0) ? nzones : 1;
    j->zone_words = code_words / j->nzones;
    j->area[1].count = (nhot != 0 && j->nzones >= 3) ? 1 : 0;
    /* Chunks of at least 512 words, as many as the marks allow. */
    j->chunk_shift = 9;
    while ((code_words >> j->chunk_shift) > Z80J_MARK_CHUNKS)
        j->chunk_shift++;
    j->blocks = (z80j_block *)block_mem;
    j->max_blocks = max_blocks;
    j->links = link_mem;
    j->max_links = (link_mem != 0) ? max_links : 0;
    memcpy(&j->glue, glue, sizeof(j->glue));
    ctx->resume_tab = JIT_ADDR(j->resume);
    ctx->irq_count = 0;
    ctx->resumed_count = 0;

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
    ctx->write_a = JIT_ADDR(z80j_write_generic);
    ctx->wdata = 0;
    ctx->hot_tab = 0;
    ctx->ei_saved = 0;
    ctx->int_leave = 0;
    ctx->int_runs = 0;
    ctx->int_insns = 0;
    ctx->hot_sync_gate = 0;
    ctx->hot_force_at = 0;

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
    glue->interp = JIT_ADDR(z80j_glue_interp);
}
