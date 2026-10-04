/*
 * Z80 to ARM translator: ARM code generation.
 *
 * Generated code conventions (see z80jit_int.h):
 *   r0-r2, r12, lr  scratch (r0-r2, r12 and lr are lost across a memory
 *                   write, which may call the machine)
 *   r3  F (internal format)          r4  A in bits 24-31
 *   r5  BC, r6 DE, r7 HL, r9 IX, r11 SP: value in bits 16-31, low bits 0
 *   r8  T-states left in the scanline, times CYCLE
 *   r10 global pointer (&ctx->wtab[0])
 * A Z80 memory address is handled in the same "bits 16-31" form, so that
 * (HL), (DE), (IX+d) and SP share one read and one write sequence:
 *   read:  page entry below the global pointer, then LDRB
 *   write: page entry above it, STRB when nonzero, else the machine
 * 8-bit operands are described by a register and a left shift that puts
 * the value in bits 24-31 with zeros below.
 */

#include "z80jit_int.h"


/* Offset of a context field from the global pointer. */
#define GOFF(f) ((int32)(offsetof(z80j_ctx, f) - offsetof(z80j_ctx, wtab)))

/* ARM condition codes. */
#define C_EQ 0x0u
#define C_NE 0x1u
#define C_CS 0x2u
#define C_CC 0x3u
#define C_MI 0x4u
#define C_PL 0x5u
#define C_VS 0x6u
#define C_VC 0x7u
#define C_AL 0xEu

/* ARM data-processing opcodes. */
#define OP_AND 0x0u
#define OP_EOR 0x1u
#define OP_SUB 0x2u
#define OP_RSB 0x3u
#define OP_ADD 0x4u
#define OP_ADC 0x5u
#define OP_SBC 0x6u
#define OP_TST 0x8u
#define OP_TEQ 0x9u
#define OP_CMP 0xAu
#define OP_CMN 0xBu
#define OP_ORR 0xCu
#define OP_MOV 0xDu
#define OP_BIC 0xEu
#define OP_MVN 0xFu

/* Shift types. */
#define SH_LSL 0u
#define SH_LSR 1u
#define SH_ASR 2u
#define SH_ROR 3u

/*--------------------------------------------------------------------------
 * ARM encoding
 *------------------------------------------------------------------------*/

static __inline void emit(z80j_state *j, uint32 w)
{
    *j->cur++ = w;
}

#define IMM_NONE 0xFFFFFFFFu

/* Encodes value as an 8-bit immediate rotated right by an even amount;
 * returns the 12-bit field, or IMM_NONE. */
static uint32 arm_imm(uint32 value)
{
    uint32 v;
    uint32 tz;
    uint32 k;

    if (value < 256)
        return value;
    /* The usual cases: a byte in bits 8-15, 16-23 or 24-31. */
    if ((value & 0xFFFF00FFu) == 0)
        return (12u << 8) | (value >> 8);
    if ((value & 0xFF00FFFFu) == 0)
        return (8u << 8) | (value >> 16);
    if ((value & 0x00FFFFFFu) == 0)
        return (4u << 8) | (value >> 24);
    /* A byte shifted left by another even amount (k = 0), or wrapping
     * around bit 31, which is contiguous once rotated left by 8 (k = 1). */
    for (k = 0; k < 2; k++) {
        v = (k == 0) ? value : ((value << 8) | (value >> 24));
        tz = 0;
        if ((v & 0xFFFFu) == 0) { v >>= 16; tz = 16; }
        if ((v & 0xFFu) == 0) { v >>= 8; tz += 8; }
        if ((v & 0xFu) == 0) { v >>= 4; tz += 4; }
        if ((v & 0x3u) == 0) { v >>= 2; tz += 2; }
        if (v < 256)
            return ((((k == 0 ? 16u : 4u) - tz / 2) & 15u) << 8) | v;
    }
    return IMM_NONE;
}

static __inline void dp_i(z80j_state *j, uint32 cond, uint32 op, uint32 s,
                          uint32 rd, uint32 rn, uint32 value)
{
    uint32 enc = value;

    if (value >= 256) {
        /* The usual forms of the generated code: a byte in bits 16-23 or
         * 24-31 (register pairs and A). */
        if ((value & 0xFF00FFFFu) == 0)
            enc = (8u << 8) | (value >> 16);
        else if ((value & 0x00FFFFFFu) == 0)
            enc = (4u << 8) | (value >> 24);
        else
            enc = arm_imm(value);
        if (enc == IMM_NONE)
            j->error = 1;               /* immediate not encodable */
    }
    emit(j, (cond << 28) | (1u << 25) | (op << 21) | (s << 20) |
            (rn << 16) | (rd << 12) | enc);
}

static __inline void dp_r(z80j_state *j, uint32 cond, uint32 op, uint32 s,
                 uint32 rd, uint32 rn, uint32 rm, uint32 sh, uint32 amount)
{
    emit(j, (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) |
            ((amount & 31u) << 7) | (sh << 5) | rm);
}

/* Compare-type instructions always set the flags and have no Rd. */
static __inline void cmp_i(z80j_state *j, uint32 cond, uint32 op, uint32 rn, uint32 value)
{
    dp_i(j, cond, op, 1, 0, rn, value);
}

static __inline void cmp_r(z80j_state *j, uint32 cond, uint32 op, uint32 rn, uint32 rm,
                  uint32 sh, uint32 amount)
{
    dp_r(j, cond, op, 1, 0, rn, rm, sh, amount);
}

static __inline void mov_r(z80j_state *j, uint32 rd, uint32 rm, uint32 sh, uint32 amount)
{
    dp_r(j, C_AL, OP_MOV, 0, rd, 0, rm, sh, amount);
}

/* Load / store with an immediate offset (pre-indexed, no write-back). */
static __inline void mem_i(z80j_state *j, uint32 cond, uint32 load, uint32 byte,
                  uint32 rd, uint32 rn, int32 off)
{
    uint32 up = 1;

    if (off < 0) {
        up = 0;
        off = -off;
    }
    if (off > 4095)
        j->error = 2;                   /* offset out of range */
    emit(j, (cond << 28) | (1u << 26) | (1u << 24) | (up << 23) | (byte << 22) |
            (load << 20) | (rn << 16) | (rd << 12) | ((uint32)off & 0xFFFu));
}

/* Load / store with a shifted register offset added (pre-indexed). */
static __inline void mem_r(z80j_state *j, uint32 cond, uint32 load, uint32 byte,
                  uint32 rd, uint32 rn, uint32 rm, uint32 sh, uint32 amount)
{
    emit(j, (cond << 28) | (3u << 25) | (1u << 24) | (1u << 23) | (byte << 22) |
            (load << 20) | (rn << 16) | (rd << 12) | ((amount & 31u) << 7) |
            (sh << 5) | rm);
}

static __inline void ldr_g(z80j_state *j, uint32 rd, int32 off)
{
    mem_i(j, C_AL, 1, 0, rd, RG, off);
}

static __inline void str_g(z80j_state *j, uint32 rd, int32 off)
{
    mem_i(j, C_AL, 0, 0, rd, RG, off);
}

uint32 arm_branch(uint32 cond, uint32 link, uint32 from, uint32 to)
{
    return (cond << 28) | (5u << 25) | (link << 24) | (((to - from - 8) >> 2) & 0x00FFFFFFu);
}

void emit_b(z80j_state *j, uint32 cond, uint32 link, uint32 to)
{
    emit(j, arm_branch(cond, link, JIT_ADDR(j->cur), to));
}

/* Retargets an emitted branch, keeping its condition and link bit. */
void patch_b(uint32 *site, uint32 to)
{
    uint32 from = JIT_ADDR(site);

    *site = (*site & 0xFF000000u) | (((to - from - 8) >> 2) & 0x00FFFFFFu);
}

static void emit_mrs(z80j_state *j, uint32 rd)
{
    emit(j, (C_AL << 28) | 0x010F0000u | (rd << 12));
}

/* LDM / STM rn,{list} without write-back: increment after (db = 0) or
 * decrement before (db = 1, the words below rn). */
static void emit_ldm(z80j_state *j, uint32 rn, uint32 list, uint32 db)
{
    emit(j, (C_AL << 28) | (db ? 0x09100000u : 0x08900000u) | (rn << 16) | list);
}

static void emit_stm(z80j_state *j, uint32 rn, uint32 list, uint32 db)
{
    emit(j, (C_AL << 28) | (db ? 0x09000000u : 0x08800000u) | (rn << 16) | list);
}

/* rd = value (32 bits), one to four instructions. */
static void emit_mov32(z80j_state *j, uint32 rd, uint32 value)
{
    int32 first = 1;
    uint32 k;

    if (arm_imm(value) != IMM_NONE) {
        dp_i(j, C_AL, OP_MOV, 0, rd, 0, value);
        return;
    }
    if (arm_imm(~value) != IMM_NONE) {
        dp_i(j, C_AL, OP_MVN, 0, rd, 0, ~value);
        return;
    }
    for (k = 0; k < 32; k += 8) {
        uint32 part = value & ((uint32)0xFF << k);
        if (part == 0)
            continue;
        dp_i(j, C_AL, first ? OP_MOV : OP_ORR, 0, rd, rd, part);
        first = 0;
    }
    if (first)
        dp_i(j, C_AL, OP_MOV, 0, rd, 0, 0);
}

/* rd = value << 16 (register pair form). */
static void emit_mov_pair(z80j_state *j, uint32 rd, uint32 value)
{
    emit_mov32(j, rd, (value & 0xFFFFu) << 16);
}

/*--------------------------------------------------------------------------
 * Stubs
 *------------------------------------------------------------------------*/

static jit_stub *new_stub(jit_block_ctx *b, uint32 kind)
{
    jit_stub *s = &b->stubs[b->nstubs++];

    if (b->nstubs > MAX_STUBS)
        b->j->error = 3;                /* too many stubs */
    s->kind = kind;
    s->site = b->j->cur;
    s->pc = 0;
    s->t = 0;
    s->index = 0;
    s->reg = 0;
    s->resume = 0;
    return s;
}

/* Placeholder branch patched when the stubs are emitted. */
static void emit_stub_branch(jit_block_ctx *b, uint32 cond, uint32 link, uint32 kind,
                             uint32 pc, uint32 t, uint32 reg)
{
    jit_stub *s = new_stub(b, kind);

    s->pc = pc;
    s->t = t;
    s->reg = reg;
    emit(b->j, (cond << 28) | (5u << 25) | (link << 24));
}

/*--------------------------------------------------------------------------
 * Memory
 *------------------------------------------------------------------------*/

/* r0 = byte at the Z80 address held in ra (bits 16-31). Uses r0 only. */
static void emit_read(z80j_state *j, uint32 ra)
{
    dp_r(j, C_AL, OP_MVN, 0, R0, 0, ra, SH_LSR, 24);
    mem_r(j, C_AL, 1, 0, R0, RG, R0, SH_LSL, 2);
    mem_r(j, C_AL, 1, 1, R0, R0, ra, SH_LSR, 16);
}

/* Writes r0 to the Z80 address held in ra (bits 16-31). A special page
 * goes through the machine; the stub gives it the address of the next
 * instruction, where to go if the machine asks to leave the block (never
 * for a repeated instruction). */
static void emit_write(jit_block_ctx *b, const jit_insn *in, uint32 ra)
{
    z80j_state *j = b->j;
    uint32 next = (in->kind == K_REP) ? 0xFFFFFFFFu : ((in->pc + in->len) & 0xFFFFu);

    mov_r(j, R1, ra, SH_LSR, 24);
    mem_r(j, C_AL, 1, 0, R1, RG, R1, SH_LSL, 2);
    cmp_i(j, C_AL, OP_TEQ, R1, 0);
    mem_r(j, C_NE, 0, 1, R0, R1, ra, SH_LSR, 16);
    emit_stub_branch(b, C_EQ, 1, STUB_WRITE, next, in->t_after, ra);
}

/* Offset from the global pointer of the host byte of a Z80 address whose
 * mapping never changes, or -1. */
static int32 fixed_offset(const z80j_state *j, uint32 addr, int32 write)
{
    const z80j_ctx *ctx = j->ctx;
    uint32 page = (addr >> 8) & 0xFFu;
    uint32 kind = ctx->page_kind[page];
    uint32 host;
    uint32 g = JIT_ADDR(&ctx->wtab[0]);

    if (write) {
        if (!(kind & Z80J_PAGE_WFIXED) || ctx->wtab[page] == 0)
            return -1;
        host = ctx->wtab[page] + addr;
    } else {
        if (!(kind & Z80J_PAGE_RFIXED))
            return -1;
        host = ctx->rtab[255 - page] + addr;
    }
    if (host < g || host - g >= 0x100000u)
        return -1;
    return (int32)(host - g);
}

/* Base register and offset (<= 4095) for a fixed host offset. */
static uint32 fixed_base(z80j_state *j, int32 *off)
{
    if ((*off & ~0xFFF) == 0)
        return RG;
    dp_i(j, C_AL, OP_ADD, 0, R1, RG, (uint32)*off & ~0xFFFu);
    *off &= 0xFFF;
    return R1;
}

static void emit_read_const(z80j_state *j, uint32 addr)
{
    int32 off = fixed_offset(j, addr & 0xFFFFu, 0);

    if (off >= 0) {
        uint32 base = fixed_base(j, &off);
        mem_i(j, C_AL, 1, 1, R0, base, off);
    } else {
        emit_mov_pair(j, RAD, addr);
        emit_read(j, RAD);
    }
}

/* Writes r0 to a constant Z80 address. */
static void emit_write_const(jit_block_ctx *b, const jit_insn *in, uint32 addr)
{
    z80j_state *j = b->j;
    int32 off = fixed_offset(j, addr & 0xFFFFu, 1);

    if (off >= 0) {
        uint32 base = fixed_base(j, &off);
        mem_i(j, C_AL, 0, 1, R0, base, off);
    } else {
        emit_mov_pair(j, RAD, addr);
        emit_write(b, in, RAD);
    }
}

/* Register (bits 16-31) holding the address of the memory operand. */
static uint32 emit_mem_addr(z80j_state *j, const jit_insn *in)
{
    uint32 base;
    int32 d = in->d;

    switch (in->pre) {
    case PRE_DD:
    case PRE_DDCB:
        base = RIX;
        break;
    case PRE_FD:
    case PRE_FDCB:
        ldr_g(j, RAD, GOFF(iy));
        base = RAD;
        break;
    default:
        return RHL;
    }
    if (d > 0)
        dp_i(j, C_AL, OP_ADD, 0, RAD, base, (uint32)d << 16);
    else if (d < 0)
        dp_i(j, C_AL, OP_SUB, 0, RAD, base, (uint32)(-d) << 16);
    else
        return base;
    return RAD;
}

/* Pushes the 16-bit value in r0 (uses r1, r2). */
static void emit_push(z80j_state *j)
{
    dp_i(j, C_AL, OP_SUB, 0, RSP, RSP, 0x00020000u);
    mov_r(j, R1, RSP, SH_LSR, 24);
    mem_r(j, C_AL, 1, 0, R1, RG, R1, SH_LSL, 2);
    cmp_i(j, C_AL, OP_TEQ, R1, 0);
    dp_r(j, C_NE, OP_ADD, 0, R1, R1, RSP, SH_LSR, 16);
    mem_i(j, C_NE, 0, 1, R0, R1, 0);
    dp_r(j, C_NE, OP_MOV, 0, R2, 0, R0, SH_LSR, 8);
    mem_i(j, C_NE, 0, 1, R2, R1, 1);
    emit_b(j, C_EQ, 1, j->glue.push_slow);
}

/* Pops 16 bits: low byte in r2, high byte in r1 (uses r0). */
static void emit_pop(z80j_state *j)
{
    dp_r(j, C_AL, OP_MVN, 0, R0, 0, RSP, SH_LSR, 24);
    mem_r(j, C_AL, 1, 0, R1, RG, R0, SH_LSL, 2);
    mem_r(j, C_AL, 1, 1, R2, R1, RSP, SH_LSR, 16);
    dp_i(j, C_AL, OP_ADD, 0, RSP, RSP, 0x00010000u);
    mem_r(j, C_AL, 1, 1, R1, R1, RSP, SH_LSR, 16);
    dp_i(j, C_AL, OP_ADD, 0, RSP, RSP, 0x00010000u);
}

/*--------------------------------------------------------------------------
 * Registers
 *------------------------------------------------------------------------*/

/* 8-bit operand: value in bits 24-31 of reg << lsl, zeros below; or an
 * immediate. */
typedef struct {
    uint32 reg;
    uint32 lsl;
    uint32 is_imm;
    uint32 imm;
} opnd;

/* Z80 register index: 0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 7 A. In a DD/FD
 * instruction without memory operand, H and L are the halves of IX/IY. */
static uint32 idx_of(const jit_insn *in)
{
    if (in->mem)
        return 0;
    if (in->pre == PRE_DD)
        return 1;
    if (in->pre == PRE_FD)
        return 2;
    return 0;
}

/* ARM register holding the pair of an 8-bit register (IY: loaded in tmp). */
static uint32 pair_of(z80j_state *j, uint32 r, uint32 idx, uint32 tmp)
{
    switch (r) {
    case 0: case 1: return RBC;
    case 2: case 3: return RDE;
    case 4: case 5:
        if (idx == 1)
            return RIX;
        if (idx == 2) {
            ldr_g(j, tmp, GOFF(iy));
            return tmp;
        }
        return RHL;
    default:
        return RA;
    }
}

/* Operand for register r; may use scratch. */
static opnd get8(z80j_state *j, uint32 r, uint32 idx, uint32 scratch)
{
    opnd o;
    uint32 rp;

    o.is_imm = 0;
    o.imm = 0;
    if (r == 7) {
        o.reg = RA;
        o.lsl = 0;
        return o;
    }
    rp = pair_of(j, r, idx, scratch);
    if ((r & 1) == 0) {                         /* high byte */
        dp_i(j, C_AL, OP_AND, 0, scratch, rp, 0xFF000000u);
        o.reg = scratch;
        o.lsl = 0;
    } else {                                    /* low byte */
        o.reg = rp;
        o.lsl = 8;
    }
    return o;
}

static opnd imm8(uint32 v)
{
    opnd o;

    o.reg = 0;
    o.lsl = 0;
    o.is_imm = 1;
    o.imm = v & 0xFFu;
    return o;
}

static opnd reg_low(uint32 reg)
{
    opnd o;

    o.reg = reg;
    o.lsl = 24;
    o.is_imm = 0;
    o.imm = 0;
    return o;
}

/* Data-processing instruction with an 8-bit operand (optionally << 4). */
static void dp_o(z80j_state *j, uint32 cond, uint32 op, uint32 s, uint32 rd,
                 uint32 rn, opnd o, uint32 extra)
{
    if (o.is_imm)
        dp_i(j, cond, op, s, rd, rn, (o.imm << 24) << extra);
    else
        dp_r(j, cond, op, s, rd, rn, o.reg, SH_LSL, o.lsl + extra);
}

/* r0 = operand value 0-255. */
static void to_low(z80j_state *j, opnd o)
{
    if (o.is_imm) {
        dp_i(j, C_AL, OP_MOV, 0, R0, 0, o.imm);
    } else if (o.lsl == 24) {
        if (o.reg != R0)
            mov_r(j, R0, o.reg, SH_LSL, 0);
    } else if (o.lsl == 0) {
        mov_r(j, R0, o.reg, SH_LSR, 24);
    } else {
        mov_r(j, R0, o.reg, SH_LSL, o.lsl);
        mov_r(j, R0, R0, SH_LSR, 24);
    }
}

/* Register r = operand (clean value in bits 24-31 of reg << lsl). */
static void set8(z80j_state *j, uint32 r, uint32 idx, opnd o)
{
    uint32 rp;
    uint32 iy = 0;

    if (r == 7) {
        if (o.is_imm)
            dp_i(j, C_AL, OP_MOV, 0, RA, 0, o.imm << 24);
        else
            mov_r(j, RA, o.reg, SH_LSL, o.lsl);
        return;
    }
    if ((r == 4 || r == 5) && idx == 2) {
        ldr_g(j, RLR, GOFF(iy));
        rp = RLR;
        iy = 1;
    } else {
        rp = pair_of(j, r, idx, RLR);
    }
    if ((r & 1) == 0) {                         /* high byte */
        if (o.is_imm && o.imm == 0) {
            dp_i(j, C_AL, OP_BIC, 0, rp, rp, 0xFF000000u);
        } else if (o.is_imm) {
            dp_i(j, C_AL, OP_AND, 0, rp, rp, 0x00FF0000u);
            dp_i(j, C_AL, OP_ORR, 0, rp, rp, o.imm << 24);
        } else {
            dp_i(j, C_AL, OP_AND, 0, rp, rp, 0x00FF0000u);
            dp_r(j, C_AL, OP_ORR, 0, rp, rp, o.reg, SH_LSL, o.lsl);
        }
    } else {                                    /* low byte */
        if (!o.is_imm && o.lsl == 8) {
            mov_r(j, R1, o.reg, SH_LSL, 8);
            o.reg = R1;
            o.lsl = 0;
        }
        if (o.is_imm && o.imm == 0) {
            dp_i(j, C_AL, OP_BIC, 0, rp, rp, 0x00FF0000u);
        } else {
            dp_i(j, C_AL, OP_AND, 0, rp, rp, 0xFF000000u);
            if (o.is_imm)
                dp_i(j, C_AL, OP_ORR, 0, rp, rp, o.imm << 16);
            else if (o.lsl == 24)
                dp_r(j, C_AL, OP_ORR, 0, rp, rp, o.reg, SH_LSL, 16);
            else
                dp_r(j, C_AL, OP_ORR, 0, rp, rp, o.reg, SH_LSR, 8);
        }
    }
    if (iy)
        str_g(j, RLR, GOFF(iy));
}

/* Index register of a DD/FD instruction (IY loaded in tmp). */
static uint32 idx_reg(z80j_state *j, const jit_insn *in, uint32 tmp)
{
    if (in->pre == PRE_DD)
        return RIX;
    if (in->pre == PRE_FD) {
        ldr_g(j, tmp, GOFF(iy));
        return tmp;
    }
    return RHL;
}

static void idx_store(z80j_state *j, const jit_insn *in, uint32 reg)
{
    if (in->pre == PRE_FD)
        str_g(j, reg, GOFF(iy));
}

/* Register pair rp[p]: BC DE HL(IX/IY) SP; IY is loaded in tmp. */
static uint32 rp_reg(z80j_state *j, const jit_insn *in, uint32 p, uint32 tmp)
{
    switch (p) {
    case 0: return RBC;
    case 1: return RDE;
    case 2: return idx_reg(j, in, tmp);
    default: return RSP;
    }
}

/*--------------------------------------------------------------------------
 * Flags
 *------------------------------------------------------------------------*/

static int32 flags_live(const jit_insn *in)
{
    return (in->def & in->live_after) != 0;
}

/* S, Z and P of the byte in bits 24-31 of reg; keeps the other bits
 * given in keep. */
static void flags_szp(z80j_state *j, uint32 reg, uint32 keep, uint32 set)
{
    dp_i(j, C_AL, OP_ADD, 0, R2, RG, Z80J_PZST_OFF);
    if (keep != 0) {
        mem_r(j, C_AL, 1, 1, R2, R2, reg, SH_LSR, 24);
        dp_i(j, C_AL, OP_AND, 0, RF, RF, keep);
        dp_r(j, C_AL, OP_ORR, 0, RF, RF, R2, SH_LSL, 0);
    } else {
        mem_r(j, C_AL, 1, 1, RF, R2, reg, SH_LSR, 24);
    }
    if (set != 0)
        dp_i(j, C_AL, OP_ORR, 0, RF, RF, set);
}

/*--------------------------------------------------------------------------
 * 8-bit arithmetic
 *------------------------------------------------------------------------*/

/* A = A op operand for the eight ALU operations. */
static void emit_alu(z80j_state *j, const jit_insn *in, uint32 y, opnd o)
{
    uint32 full = flags_live(in) && !in->fused;
    uint32 s = (full || in->fused) ? 1u : 0u;

    /* The half-carry check reads the operand after A has changed. */
    if (full && !o.is_imm && o.reg == RA && (y == 0 || y == 2)) {
        mov_r(j, R0, RA, SH_LSL, 0);
        o.reg = R0;
        o.lsl = 0;
    }

    switch (y) {
    case 0:                                         /* ADD */
        if (full)
            mov_r(j, R1, RA, SH_LSL, 4);
        dp_o(j, C_AL, OP_ADD, s, RA, RA, o, 0);
        if (full) {
            emit_mrs(j, RF);
            mov_r(j, RF, RF, SH_LSR, 28);
            if (o.is_imm)
                cmp_i(j, C_AL, OP_CMN, R1, o.imm << 28);
            else
                cmp_r(j, C_AL, OP_CMN, R1, o.reg, SH_LSL, o.lsl + 4);
            dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_H);
        }
        break;
    case 1:                                         /* ADC */
        to_low(j, o);
        dp_r(j, C_AL, OP_MOV, 1, R1, 0, RF, SH_LSR, 2);
        dp_i(j, C_CS, OP_SUB, 0, R0, R0, 0x100);
        if (full) {
            dp_r(j, C_AL, OP_EOR, 0, RF, R0, RA, SH_LSR, 24);
            dp_r(j, C_AL, OP_ADC, 1, RA, RA, R0, SH_ROR, 8);
            emit_mrs(j, R0);
            dp_r(j, C_AL, OP_EOR, 0, RF, RF, RA, SH_LSR, 24);
            dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_H);
            dp_r(j, C_AL, OP_ORR, 0, RF, RF, R0, SH_LSR, 28);
        } else {
            dp_r(j, C_AL, OP_ADC, s, RA, RA, R0, SH_ROR, 8);
        }
        break;
    case 2:                                         /* SUB */
    case 7:                                         /* CP */
        if (y == 7 && !full && !in->fused)
            break;
        if (full)
            mov_r(j, R1, RA, SH_LSL, 4);
        if (y == 2)
            dp_o(j, C_AL, OP_SUB, s, RA, RA, o, 0);
        else
            dp_o(j, C_AL, OP_CMP, 1, 0, RA, o, 0);
        if (full) {
            emit_mrs(j, RF);
            mov_r(j, RF, RF, SH_LSR, 28);
            dp_i(j, C_AL, OP_EOR, 0, RF, RF, PSR_C | PSR_n);
            if (o.is_imm)
                cmp_i(j, C_AL, OP_CMP, R1, o.imm << 28);
            else
                cmp_r(j, C_AL, OP_CMP, R1, o.reg, SH_LSL, o.lsl + 4);
            dp_i(j, C_CC, OP_ORR, 0, RF, RF, PSR_H);
        }
        break;
    case 3:                                         /* SBC */
        to_low(j, o);
        if (full) {
            dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
            dp_r(j, C_AL, OP_SUB, 1, R0, R0, RF, SH_LSL, 7);
            dp_r(j, C_AL, OP_EOR, 0, RF, R0, RA, SH_LSR, 24);
            dp_r(j, C_AL, OP_SBC, 1, RA, RA, R0, SH_ROR, 8);
            emit_mrs(j, R0);
            dp_r(j, C_AL, OP_EOR, 0, RF, RF, RA, SH_LSR, 24);
            dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_H);
            dp_r(j, C_AL, OP_ORR, 0, RF, RF, R0, SH_LSR, 28);
            dp_i(j, C_AL, OP_EOR, 0, RF, RF, PSR_C | PSR_n);
        } else {
            dp_i(j, C_AL, OP_AND, 0, R1, RF, PSR_C);
            dp_r(j, C_AL, OP_SUB, 1, R0, R0, R1, SH_LSL, 7);
            dp_r(j, C_AL, OP_SBC, s, RA, RA, R0, SH_ROR, 8);
        }
        break;
    default:                                        /* AND XOR OR */
        dp_o(j, C_AL, (y == 4) ? OP_AND : (y == 5) ? OP_EOR : OP_ORR, s, RA, RA, o, 0);
        if (full)
            flags_szp(j, RA, 0, (y == 4) ? PSR_H : 0);
        break;
    }
    if (full)
        j->stats.full_flags++;
}

/* INC / DEC of the clean value in bits 24-31 of reg. */
static void emit_incdec(z80j_state *j, const jit_insn *in, uint32 reg, uint32 dec)
{
    if (flags_live(in) && !in->fused) {
        if (!dec) {
            dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
            dp_i(j, C_AL, OP_ADD, 1, reg, reg, 0x01000000u);
            dp_i(j, C_MI, OP_ORR, 0, RF, RF, PSR_S);
            dp_i(j, C_VS, OP_ORR, 0, RF, RF, PSR_V);
            dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_Z);
            cmp_i(j, C_AL, OP_TST, reg, 0x0F000000u);
            dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_H);
        } else {
            dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_n | PSR_H | PSR_S | PSR_V | PSR_Z);
            cmp_i(j, C_AL, OP_TST, reg, 0x0F000000u);
            dp_i(j, C_NE, OP_BIC, 0, RF, RF, PSR_H);
            dp_i(j, C_AL, OP_SUB, 1, reg, reg, 0x01000000u);
            dp_i(j, C_PL, OP_BIC, 0, RF, RF, PSR_S);
            dp_i(j, C_VC, OP_BIC, 0, RF, RF, PSR_V);
            dp_i(j, C_NE, OP_BIC, 0, RF, RF, PSR_Z);
        }
        j->stats.full_flags++;
    } else {
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, in->fused ? 1u : 0u, reg, reg, 0x01000000u);
    }
}

/*--------------------------------------------------------------------------
 * Rotations, shifts and bits (value in bits 24-31 of r0, ARM C = bit out)
 *------------------------------------------------------------------------*/

static void emit_rot(z80j_state *j, uint32 y)
{
    switch (y) {
    case 0:                                         /* RLC */
        dp_r(j, C_AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1);
        dp_i(j, C_CS, OP_ORR, 0, R0, R0, 0x01000000u);
        break;
    case 1:                                         /* RRC */
        dp_r(j, C_AL, OP_MOV, 1, R1, 0, R0, SH_LSR, 25);
        dp_i(j, C_CS, OP_ORR, 0, R1, R1, 0x80);
        mov_r(j, R0, R1, SH_LSL, 24);
        break;
    case 2:                                         /* RL */
        cmp_i(j, C_AL, OP_TST, RF, PSR_C);
        dp_i(j, C_NE, OP_ORR, 0, R0, R0, 0x00800000u);
        dp_r(j, C_AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1);
        break;
    case 3:                                         /* RR */
        mov_r(j, R1, R0, SH_LSR, 24);
        cmp_i(j, C_AL, OP_TST, RF, PSR_C);
        dp_i(j, C_NE, OP_ORR, 0, R1, R1, 0x100);
        dp_r(j, C_AL, OP_MOV, 1, R1, 0, R1, SH_LSR, 1);
        mov_r(j, R0, R1, SH_LSL, 24);
        break;
    case 4:                                         /* SLA */
        dp_r(j, C_AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1);
        break;
    case 5:                                         /* SRA */
        dp_r(j, C_AL, OP_MOV, 1, R1, 0, R0, SH_ASR, 25);
        mov_r(j, R0, R1, SH_LSL, 24);
        break;
    case 6:                                         /* SLL */
        dp_r(j, C_AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1);
        dp_i(j, C_AL, OP_ORR, 0, R0, R0, 0x01000000u);
        break;
    default:                                        /* SRL */
        dp_r(j, C_AL, OP_MOV, 1, R1, 0, R0, SH_LSR, 25);
        mov_r(j, R0, R1, SH_LSL, 24);
        break;
    }
}

/* Flags of a CB rotation: S Z P of r0, H = N = 0, C = ARM C. */
static void emit_rot_flags(z80j_state *j, const jit_insn *in)
{
    if (!flags_live(in))
        return;
    dp_i(j, C_AL, OP_ADD, 0, R2, RG, Z80J_PZST_OFF);
    mem_r(j, C_AL, 1, 1, RF, R2, R0, SH_LSR, 24);
    dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
    j->stats.full_flags++;
}

/* BIT b on bit position pos of reg. */
static void emit_bit(z80j_state *j, const jit_insn *in, uint32 reg, uint32 pos, uint32 b)
{
    if (in->fused) {
        cmp_i(j, C_AL, OP_TST, reg, (uint32)1 << pos);
    } else if (flags_live(in)) {
        dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
        dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_H);
        cmp_i(j, C_AL, OP_TST, reg, (uint32)1 << pos);
        dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_Z | PSR_P);
        if (b == 7)
            dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_S);
        j->stats.full_flags++;
    }
}

/* Bit position of bit b of 8-bit register r in its ARM register. */
static uint32 bit_pos(uint32 r, uint32 b)
{
    if (r == 7 || (r & 1) == 0)
        return 24 + b;
    return 16 + b;
}

static void emit_cb(jit_block_ctx *b, jit_insn *in)
{
    z80j_state *j = b->j;
    uint32 x = in->op >> 6;
    uint32 y = (in->op >> 3) & 7;
    uint32 z = in->op & 7;
    uint32 idx = (in->pre == PRE_DDCB) ? 1 : (in->pre == PRE_FDCB) ? 2 : 0;
    uint32 ra;

    if (in->mem) {
        ra = emit_mem_addr(j, in);
        if (x == 1) {                               /* BIT b,(HL) */
            if (in->fused || flags_live(in)) {
                emit_read(j, ra);
                emit_bit(j, in, R0, y, y);
            }
            return;
        }
        emit_read(j, ra);
        if (x == 0) {
            mov_r(j, R0, R0, SH_LSL, 24);
            emit_rot(j, y);
            emit_rot_flags(j, in);
            if (idx != 0 && z != 6) {               /* undocumented copy */
                opnd r;
                r.reg = R0;
                r.lsl = 0;
                r.is_imm = 0;
                r.imm = 0;
                set8(j, z, 0, r);
            }
            mov_r(j, R0, R0, SH_LSR, 24);
        } else {
            dp_i(j, C_AL, (x == 2) ? OP_BIC : OP_ORR, 0, R0, R0, (uint32)1 << y);
            if (idx != 0 && z != 6)
                set8(j, z, 0, reg_low(R0));
        }
        emit_write(b, in, ra);
        return;
    }

    if (x == 0) {
        opnd o = get8(j, z, 0, R0);
        if (o.reg != R0 || o.lsl != 0)
            mov_r(j, R0, o.reg, SH_LSL, o.lsl);
        emit_rot(j, y);
        emit_rot_flags(j, in);
        {
            opnd r;
            r.reg = R0;
            r.lsl = 0;
            r.is_imm = 0;
            r.imm = 0;
            set8(j, z, 0, r);
        }
    } else if (x == 1) {
        emit_bit(j, in, pair_of(j, z, 0, R0), bit_pos(z, y), y);
    } else {
        dp_i(j, C_AL, (x == 2) ? OP_BIC : OP_ORR, 0, pair_of(j, z, 0, R0),
             pair_of(j, z, 0, R0), (uint32)1 << bit_pos(z, y));
    }
}

/*--------------------------------------------------------------------------
 * Control flow
 *------------------------------------------------------------------------*/

/* Transfer to a static Z80 address from the end of the block. */
static void emit_exit(jit_block_ctx *b, uint32 target)
{
    z80j_state *j = b->j;
    uint32 host;
    uint32 *d;

    target &= 0xFFFFu;
    if (b->end_leave) {
        emit_mov32(j, R0, target);
        dp_i(j, C_AL, OP_MOV, 0, R1, 0, 0);
        emit_b(j, C_AL, 0, j->glue.leave);
        return;
    }
    /* A link stub even when the target is known: the direct branch must
     * be undone if the target is evicted. */
    emit_b(j, C_AL, 1, j->glue.link);
    d = j->cur;
    emit(j, target);
    emit(j, 0);
    emit(j, b->key);
    host = jit_link_host(j, target, b->key, JIT_ADDR(d));
    if (host != 0)
        jit_link_stub(j, d, host);
}

/* Jump to the address in r0 through the lookup table. */
static void emit_dynamic(jit_block_ctx *b)
{
    z80j_state *j = b->j;

    if (b->end_leave) {
        dp_i(j, C_AL, OP_MOV, 0, R1, 0, 0);
        emit_b(j, C_AL, 0, j->glue.leave);
        return;
    }
    dp_i(j, C_AL, OP_ADD, 0, R1, RG, Z80J_TAB_OFF);
    mem_r(j, C_AL, 1, 0, RPC, R1, R0, SH_LSL, 2);
}

/* r0 = the 16-bit operand of a JP / CALL in RAM, read from memory
 * (uses r0-r2 and r12). */
static void emit_load_target(z80j_state *j, const jit_insn *in)
{
    uint32 a = (in->pc + in->len - 2) & 0xFFFFu;

    emit_read_const(j, (a + 1) & 0xFFFFu);
    mov_r(j, R2, R0, SH_LSL, 8);
    emit_read_const(j, a);
    dp_r(j, C_AL, OP_ORR, 0, R0, R0, R2, SH_LSL, 0);
}

/* RAD = the 16-bit address operand of an instruction in RAM, read from
 * memory, in the "bits 16-31" form of the memory sequences (uses r0-r2
 * and r12). */
static void emit_dyn_addr(z80j_state *j, const jit_insn *in)
{
    emit_load_target(j, in);
    mov_r(j, RAD, R0, SH_LSL, 16);
}

/* Transfer of a JP / CALL whose target is read when it runs. */
static void emit_dyn_exit(jit_block_ctx *b, const jit_insn *in)
{
    emit_load_target(b->j, in);
    emit_dynamic(b);
}

/* ARM condition that holds when the Z80 condition cc is true: from the
 * ARM flags of a fused producer, or after a TST emitted here. */
static uint32 emit_cond(z80j_state *j, const jit_insn *prev, uint32 cc)
{
    static const uint32 mask[8] = { PSR_Z, PSR_Z, PSR_C, PSR_C, PSR_V, PSR_V, PSR_S, PSR_S };
    uint32 fu = (prev != 0) ? prev->fused : 0;
    uint32 t = cc & 1;              /* 1: flag set is true */

    if (fu & FU_Z_EQ) return t ? C_EQ : C_NE;
    if (fu & FU_Z_CS) return t ? C_CS : C_CC;
    if (fu & FU_C_CS) return t ? C_CS : C_CC;
    if (fu & FU_C_CC) return t ? C_CC : C_CS;
    if (fu & FU_V_VS) return t ? C_VS : C_VC;
    if (fu & FU_S_MI) return t ? C_MI : C_PL;
    cmp_i(j, C_AL, OP_TST, RF, mask[cc & 7]);
    return t ? C_NE : C_EQ;
}

/* Jump back of a busy-wait loop: the T-states left in the stretch are
 * counted as idle and dropped, so that the segment check at the loop head
 * ends the stretch (the loop resumes there if no interrupt comes). */
static void emit_idle(z80j_state *j, uint32 cond)
{
    mem_i(j, cond, 1, 0, R0, RG, GOFF(idle));
    dp_r(j, cond, OP_ADD, 0, R0, R0, RCYC, SH_LSL, 0);
    mem_i(j, cond, 0, 0, R0, RG, GOFF(idle));
    dp_i(j, cond, OP_MOV, 0, RCYC, 0, 0);
}

/* Conditional jump taken path: extra T-states, then the branch. */
static void emit_jump_cond(jit_block_ctx *b, uint32 cond, const jit_insn *in,
                           int32 index, uint32 extra)
{
    z80j_state *j = b->j;

    if (in->busy) {
        emit_idle(j, cond);
        emit_b(j, cond, 0, JIT_ADDR(b->ins[in->internal].host));
        return;
    }
    if (extra != 0)
        dp_i(j, cond, OP_SUB, 0, RCYC, RCYC, extra * CYCLE);
    if (in->internal >= 0 && in->internal <= index) {
        emit_b(j, cond, 0, JIT_ADDR(b->ins[in->internal].host));
        return;
    }
    if (in->internal > index) {
        jit_stub *s = new_stub(b, STUB_FORWARD);
        s->index = in->internal;
        emit(j, (cond << 28) | (5u << 25));
        return;
    }
    if (in->dyn) {
        jit_stub *s = new_stub(b, STUB_DYN);
        s->index = index;
        emit(j, (cond << 28) | (5u << 25));
        return;
    }
    {
        jit_stub *s = new_stub(b, STUB_LINK);
        s->pc = in->target;
        emit(j, (cond << 28) | (5u << 25));
    }
}

/* Leaves the block when the machine asked for it (r0 != 0). */
static void emit_leave_check(jit_block_ctx *b, const jit_insn *in)
{
    cmp_i(b->j, C_AL, OP_TEQ, R0, 0);
    emit_stub_branch(b, C_NE, 0, STUB_LEAVE, (in->pc + in->len) & 0xFFFFu, in->t_after, 0);
}

/*--------------------------------------------------------------------------
 * ED instructions
 *------------------------------------------------------------------------*/

/* 8-bit register value 0-255 (possibly with higher bits) in r0. */
static void get_low(z80j_state *j, uint32 r)
{
    switch (r) {
    case 0: mov_r(j, R0, RBC, SH_LSR, 24); break;
    case 1: mov_r(j, R0, RBC, SH_LSR, 16); break;
    case 2: mov_r(j, R0, RDE, SH_LSR, 24); break;
    case 3: mov_r(j, R0, RDE, SH_LSR, 16); break;
    case 4: mov_r(j, R0, RHL, SH_LSR, 24); break;
    case 5: mov_r(j, R0, RHL, SH_LSR, 16); break;
    case 6: dp_i(j, C_AL, OP_MOV, 0, R0, 0, 0); break;
    default: mov_r(j, R0, RA, SH_LSR, 24); break;
    }
}

/* Port C in r1. */
static void port_c(z80j_state *j)
{
    mov_r(j, R1, RBC, SH_LSR, 16);
    dp_i(j, C_AL, OP_AND, 0, R1, R1, 0xFF);
}

/* Handler of a constant port in one of the context's port tables. */
static uint32 port_handler(uint32 table, uint32 port)
{
    return ((const uint32 *)JIT_PTR(table))[port & 0xFFu];
}

/* r2 = T-states (times CYCLE) left in the scanline at the end of the
 * instruction: the segment check has already taken the rest of the
 * segment. */
static void time_left(z80j_state *j, const jit_insn *in)
{
    if (in->t_after != 0)
        dp_i(j, C_AL, OP_ADD, 0, R2, RCYC, in->t_after * CYCLE);
    else
        mov_r(j, R2, RCYC, SH_LSL, 0);
}

/*
 * Fast path of a port output loop (OTIR, OTDR, or OUTI / OUTD closed by
 * JR NZ or JP NZ back to it, the last instruction of the loop being tail),
 * at its head, after the segment check: when the stretch has room for
 * the B - 1 further passes of t_pass T-states each, the B bytes go out
 * in one run (B = 0: 256) and B, HL, the flags and the T-states end as
 * after the last pass; the bytes are spaced as in a run of OUTI, which
 * only matters for colour RAM writes during the active display. Returns
 * the site of the branch past the loop, to be patched; the regular code
 * of one pass follows (the stretch is about to end).
 */
static uint32 *emit_out_loop_fast(jit_block_ctx *b, const jit_insn *in,
                                  const jit_insn *tail, uint32 t_pass)
{
    z80j_state *j = b->j;
    uint32 dec = (in->op & 0x08u) != 0;
    uint32 *slow;
    uint32 *skip;

    dp_r(j, C_AL, OP_MOV, 1, R0, 0, RBC, SH_LSR, 24);
    dp_i(j, C_EQ, OP_MOV, 0, R0, 0, 256);
    dp_i(j, C_AL, OP_SUB, 0, RAD, R0, 1);
    dp_i(j, C_AL, OP_MOV, 0, R1, 0, t_pass * CYCLE);
    emit(j, (C_AL << 28) | (R2 << 16) | (RAD << 8) | 0x90u | R1);     /* mul r2,r1,r12 */
    cmp_r(j, C_AL, OP_CMP, RCYC, R2, SH_LSL, 0);
    slow = j->cur;
    emit(j, (C_CC << 28) | (5u << 25));
    dp_r(j, C_AL, OP_SUB, 0, RCYC, RCYC, R2, SH_LSL, 0);
    if (dec)
        dp_i(j, C_AL, OP_RSB, 0, R0, R0, 0);
    port_c(j);
    time_left(j, in);
    emit_b(j, C_AL, 1, j->glue.io_outn);
    mov_r(j, RAD, R0, SH_LSL, 0);
    dp_r(j, C_AL, OP_MOV, 1, R0, 0, RBC, SH_LSR, 24);
    dp_i(j, C_EQ, OP_MOV, 0, R0, 0, 256);
    dp_r(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, R0, SH_LSL, 16);
    dp_i(j, C_AL, OP_BIC, 0, RBC, RBC, 0xFF000000u);
    dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_Z | PSR_n);
    cmp_i(j, C_AL, OP_TEQ, RAD, 0);
    emit_stub_branch(b, C_NE, 0, STUB_LEAVE, (tail->pc + tail->len) & 0xFFFFu, tail->t_after, 0);
    skip = j->cur;
    emit(j, (C_AL << 28) | (5u << 25));
    patch_b(slow, JIT_ADDR(j->cur));
    return skip;
}

static void emit_block_insn(jit_block_ctx *b, jit_insn *in, int32 index)
{
    z80j_state *j = b->j;
    uint32 y = (in->op >> 3) & 7;
    uint32 z = in->op & 7;
    uint32 dec = y & 1;
    uint32 rep = y >= 6;
    uint32 step = 0x00010000u;
    uint32 live = flags_live(in);
    uint32 *skip = 0;

    switch (z) {
    case 0:                                         /* LDI LDD LDIR LDDR */
        emit_read(j, RHL);
        emit_write(b, in, RDE);
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, step);
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RDE, RDE, step);
        dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, step);
        if (live) {
            dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n | PSR_V);
            cmp_i(j, C_AL, OP_TEQ, RBC, 0);
            dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_V);
        }
        if (rep) {
            cmp_i(j, C_AL, OP_TEQ, RBC, 0);
            emit_jump_cond(b, C_NE, in, index, 5);
        }
        break;
    case 1:                                         /* CPI CPD CPIR CPDR */
        emit_read(j, RHL);
        mov_r(j, R1, RA, SH_LSL, 4);
        cmp_r(j, C_AL, OP_CMP, RA, R0, SH_LSL, 24);
        dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
        dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_n);
        dp_i(j, C_MI, OP_ORR, 0, RF, RF, PSR_S);
        dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_Z);
        cmp_r(j, C_AL, OP_CMP, R1, R0, SH_LSL, 28);
        dp_i(j, C_CC, OP_ORR, 0, RF, RF, PSR_H);
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, step);
        dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, step);
        cmp_i(j, C_AL, OP_TEQ, RBC, 0);
        dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_V);
        if (rep) {
            uint32 from = JIT_ADDR(j->cur);
            cmp_i(j, C_AL, OP_TST, RF, PSR_Z);
            emit(j, arm_branch(C_NE, 0, from + 4, from + 20));   /* found: stop */
            cmp_i(j, C_AL, OP_TEQ, RBC, 0);
            emit_jump_cond(b, C_NE, in, index, 5);
        }
        break;
    case 2:                                         /* INI IND INIR INDR */
        port_c(j);
        time_left(j, in);
        emit_b(j, C_AL, 1, j->glue.io_in);
        emit_write(b, in, RHL);
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, step);
        dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, 0x01000000u);
        if (live || rep) {
            dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_Z);
            dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_n);
            cmp_i(j, C_AL, OP_TST, RBC, 0xFF000000u);
            dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_Z);
        }
        if (rep)
            emit_jump_cond(b, C_NE, in, index, 5);
        break;
    default:                                        /* OUTI OUTD OTIR OTDR */
        if (rep)
            skip = emit_out_loop_fast(b, in, in, in->seg_t + 5);
        emit_read(j, RHL);
        dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, 0x01000000u);
        port_c(j);
        time_left(j, in);
        emit_b(j, C_AL, 1, j->glue.io_out);
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, step);
        if (live || rep) {
            dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_Z);
            dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_n);
            cmp_i(j, C_AL, OP_TST, RBC, 0xFF000000u);
            dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_Z);
        }
        if (rep) {
            emit_jump_cond(b, C_NE, in, index, 5);
            patch_b(skip, JIT_ADDR(j->cur));
        }
        break;
    }
}

/* A run of port outputs to port C in one call. A run of OUTI (OUTD):
 * B decreases by the run length, the bytes go out, then HL moves. A mixed
 * run (OUTI and OUT (C),r) passes its description and the byte of r in
 * r0 (Z80J_RUN_MIXED); B and HL move by its OUTI only. The flags are
 * those of the last OUTI (OUT (C),r leaves them), and the block is left
 * after the run when a handler asked for it. */
static void emit_out_run(jit_block_ctx *b, jit_insn *in)
{
    z80j_state *j = b->j;
    uint32 n = in->run & 0xFFu;
    uint32 mask = (in->run >> 8) & 0xFFFFu;
    uint32 outc = in->run >> 24;
    jit_insn *last = in + (n - 1);
    uint32 dec = (in->op == 0xAB);
    uint32 nmem = n;
    uint32 k;

    for (k = 0; k < n; k++)
        nmem -= (mask >> k) & 1u;
    if (nmem != 0)
        dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, nmem << 24);
    if (mask == 0) {
        port_c(j);
        if (dec)
            dp_i(j, C_AL, OP_MVN, 0, R0, 0, n - 1);     /* -n */
        else
            dp_i(j, C_AL, OP_MOV, 0, R0, 0, n);
        time_left(j, last);
    } else {
        emit_mov32(j, R0, Z80J_RUN_MIXED | (mask << 13) | (n << 8));
        if (outc == 0x79) {                             /* A */
            dp_r(j, C_AL, OP_ORR, 0, R0, R0, RA, SH_LSR, 24);
        } else if (outc == 0x51) {                      /* D */
            dp_r(j, C_AL, OP_ORR, 0, R0, R0, RDE, SH_LSR, 24);
        } else {                                        /* E */
            mov_r(j, R2, RDE, SH_LSL, 8);
            dp_r(j, C_AL, OP_ORR, 0, R0, R0, R2, SH_LSR, 24);
        }
        port_c(j);
        time_left(j, in);
    }
    emit_b(j, C_AL, 1, j->glue.io_outn);
    if (nmem != 0) {
        dp_i(j, C_AL, dec ? OP_SUB : OP_ADD, 0, RHL, RHL, nmem << 16);
        if (flags_live(last)) {
            dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_Z);
            dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_n);
            cmp_i(j, C_AL, OP_TST, RBC, 0xFF000000u);
            dp_i(j, C_EQ, OP_ORR, 0, RF, RF, PSR_Z);
        }
    }
    emit_leave_check(b, last);
}

static void emit_ed(jit_block_ctx *b, jit_insn *in, int32 index)
{
    z80j_state *j = b->j;
    uint32 x = in->op >> 6;
    uint32 y = (in->op >> 3) & 7;
    uint32 z = in->op & 7;
    uint32 p = y >> 1;
    uint32 q = y & 1;
    uint32 live = flags_live(in);
    uint32 rr;

    if (in->run == RUN_LOOP) {
        /* The JR NZ / JP NZ that follows closes the loop: the fast path
         * goes past it (patched by emit_block_code). */
        const jit_insn *c = in + 1;

        b->skip_site = emit_out_loop_fast(b, in, c, in->seg_t + (c->kind == K_JRCC ? 5u : 0u));
        b->skip_to = index + 2;
        emit_block_insn(b, in, index);
        return;
    }
    if (in->run != 0) {
        if (in->run != RUN_PART)
            emit_out_run(b, in);
        return;
    }
    if (x == 2) {
        if (z <= 3 && y >= 4)
            emit_block_insn(b, in, index);
        return;
    }
    if (x != 1)
        return;                                     /* NOP */

    switch (z) {
    case 0:                                         /* IN r,(C) */
        port_c(j);
        time_left(j, in);
        emit_b(j, C_AL, 1, j->glue.io_in);
        if (y != 6)
            set8(j, y, 0, reg_low(R0));
        if (live) {
            dp_i(j, C_AL, OP_ADD, 0, R1, RG, Z80J_PZST_OFF);
            mem_r(j, C_AL, 1, 1, R1, R1, R0, SH_LSL, 0);
            dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
            dp_r(j, C_AL, OP_ORR, 0, RF, RF, R1, SH_LSL, 0);
        }
        break;
    case 1:                                         /* OUT (C),r */
        get_low(j, y);
        port_c(j);
        time_left(j, in);
        emit_b(j, C_AL, 1, j->glue.io_out);
        emit_leave_check(b, in);
        break;
    case 2:                                         /* SBC / ADC HL,rr */
        rr = rp_reg(j, in, p, R2);
        if (q == 0) {                               /* SBC */
            if (live && !in->fused) {
                dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
                dp_r(j, C_AL, OP_SUB, 1, R1, RF, RF, SH_LSL, 1);
                dp_r(j, C_AL, OP_ORR, 0, R0, rr, R1, SH_LSR, 16);
                mov_r(j, R1, RHL, SH_LSL, 4);
                dp_r(j, C_AL, OP_SBC, 1, RHL, RHL, R0, SH_LSL, 0);
                emit_mrs(j, RF);
                mov_r(j, RF, RF, SH_LSR, 28);
                dp_i(j, C_AL, OP_EOR, 0, RF, RF, PSR_C | PSR_n);
                cmp_r(j, C_AL, OP_CMP, R1, R0, SH_LSL, 4);
                dp_i(j, C_CC, OP_ORR, 0, RF, RF, PSR_H);
                j->stats.full_flags++;
            } else {
                dp_i(j, C_AL, OP_AND, 0, R1, RF, PSR_C);
                dp_r(j, C_AL, OP_SUB, 1, R1, R1, R1, SH_LSL, 1);
                dp_r(j, C_AL, OP_ORR, 0, R0, rr, R1, SH_LSR, 16);
                dp_r(j, C_AL, OP_SBC, in->fused ? 1u : 0u, RHL, RHL, R0, SH_LSL, 0);
            }
        } else {                                    /* ADC */
            dp_r(j, C_AL, OP_MOV, 1, R1, 0, RF, SH_LSR, 2);
            mov_r(j, R0, rr, SH_LSL, 0);
            dp_i(j, C_CS, OP_ORR, 0, R0, R0, 0xFF00);
            dp_i(j, C_CS, OP_ORR, 0, R0, R0, 0xFF);
            if (live && !in->fused) {
                dp_r(j, C_AL, OP_EOR, 0, R2, RHL, rr, SH_LSL, 0);
                dp_r(j, C_AL, OP_ADC, 1, RHL, RHL, R0, SH_LSL, 0);
                emit_mrs(j, RF);
                mov_r(j, RF, RF, SH_LSR, 28);
                dp_r(j, C_AL, OP_EOR, 0, R2, R2, RHL, SH_LSL, 0);
                cmp_i(j, C_AL, OP_TST, R2, 0x10000000u);
                dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_H);
                j->stats.full_flags++;
            } else {
                dp_r(j, C_AL, OP_ADC, in->fused ? 1u : 0u, RHL, RHL, R0, SH_LSL, 0);
            }
        }
        break;
    case 3:                                         /* LD (nn),rr / LD rr,(nn) */
        if (q == 0) {
            rr = rp_reg(j, in, p, R2);
            mov_r(j, R0, rr, SH_LSR, 16);
            emit_write_const(b, in, in->n);
            rr = rp_reg(j, in, p, R2);
            mov_r(j, R0, rr, SH_LSR, 24);
            emit_write_const(b, in, in->n + 1);
        } else {
            emit_read_const(j, in->n);
            mov_r(j, R2, R0, SH_LSL, 0);
            emit_read_const(j, in->n + 1);
            rr = (p == 0) ? RBC : (p == 1) ? RDE : (p == 2) ? RHL : RSP;
            mov_r(j, rr, R2, SH_LSL, 16);
            dp_r(j, C_AL, OP_ORR, 0, rr, rr, R0, SH_LSL, 24);
        }
        break;
    case 4:                                         /* NEG */
        if (live && !in->fused) {
            mov_r(j, R1, RA, SH_LSL, 4);
            dp_i(j, C_AL, OP_RSB, 1, RA, RA, 0);
            emit_mrs(j, RF);
            mov_r(j, RF, RF, SH_LSR, 28);
            dp_i(j, C_AL, OP_EOR, 0, RF, RF, PSR_C | PSR_n);
            cmp_i(j, C_AL, OP_TEQ, R1, 0);
            dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_H);
            j->stats.full_flags++;
        } else {
            dp_i(j, C_AL, OP_RSB, in->fused ? 1u : 0u, RA, RA, 0);
        }
        break;
    case 5:                                         /* RETN / RETI */
        ldr_g(j, R0, GOFF(iff2));
        str_g(j, R0, GOFF(iff1));
        emit_pop(j);
        dp_r(j, C_AL, OP_ORR, 0, R0, R2, R1, SH_LSL, 8);
        dp_i(j, C_AL, OP_MOV, 0, R1, 0, 0);
        emit_b(j, C_AL, 0, j->glue.leave);
        break;
    case 6:                                         /* IM */
        dp_i(j, C_AL, OP_MOV, 0, R0, 0, (y & 3) == 2 ? 1u : (y & 3) == 3 ? 2u : 0u);
        str_g(j, R0, GOFF(im));
        break;
    default:
        switch (y) {
        case 0:                                     /* LD I,A */
            mov_r(j, R0, RA, SH_LSR, 24);
            str_g(j, R0, GOFF(i));
            break;
        case 1:                                     /* LD R,A */
            mov_r(j, R0, RA, SH_LSR, 24);
            str_g(j, R0, GOFF(r));
            break;
        case 2:                                     /* LD A,I */
        case 3:                                     /* LD A,R */
            if (y == 2) {
                ldr_g(j, R0, GOFF(i));
            } else {
                /* R is not counted per instruction: derive a changing
                 * value from the scanline and the cycle counter. */
                ldr_g(j, R0, GOFF(line));
                ldr_g(j, R1, GOFF(r));
                dp_r(j, C_AL, OP_ADD, 0, R0, R0, R0, SH_LSL, 3);
                dp_r(j, C_AL, OP_ADD, 0, R0, R0, RCYC, SH_LSR, 10);
                dp_r(j, C_AL, OP_ADD, 0, R0, R0, R1, SH_LSL, 0);
                dp_i(j, C_AL, OP_AND, 0, R0, R0, 0x7F);
                dp_i(j, C_AL, OP_AND, 0, R1, R1, 0x80);
                dp_r(j, C_AL, OP_ORR, 0, R0, R0, R1, SH_LSL, 0);
            }
            mov_r(j, RA, R0, SH_LSL, 24);
            if (live) {
                dp_i(j, C_AL, OP_ADD, 0, R1, RG, Z80J_PZST_OFF);
                mem_r(j, C_AL, 1, 1, R1, R1, R0, SH_LSL, 0);
                dp_i(j, C_AL, OP_AND, 0, R1, R1, PSR_S | PSR_Z);
                dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
                dp_r(j, C_AL, OP_ORR, 0, RF, RF, R1, SH_LSL, 0);
                ldr_g(j, R0, GOFF(iff2));
                cmp_i(j, C_AL, OP_TEQ, R0, 0);
                dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_V);
            }
            break;
        case 4:                                     /* RRD */
        case 5:                                     /* RLD */
            emit_read(j, RHL);
            mov_r(j, R1, RA, SH_LSR, 24);
            if (y == 5) {
                dp_i(j, C_AL, OP_AND, 0, R2, R1, 0x0F);
                dp_r(j, C_AL, OP_ORR, 0, R2, R2, R0, SH_LSL, 4);
                dp_i(j, C_AL, OP_AND, 0, R1, R1, 0xF0);
                dp_r(j, C_AL, OP_ORR, 0, R1, R1, R0, SH_LSR, 4);
            } else {
                dp_i(j, C_AL, OP_AND, 0, R2, R1, 0x0F);
                mov_r(j, R2, R2, SH_LSL, 4);
                dp_r(j, C_AL, OP_ORR, 0, R2, R2, R0, SH_LSR, 4);
                dp_i(j, C_AL, OP_AND, 0, R1, R1, 0xF0);
                dp_i(j, C_AL, OP_AND, 0, R0, R0, 0x0F);
                dp_r(j, C_AL, OP_ORR, 0, R1, R1, R0, SH_LSL, 0);
            }
            mov_r(j, RA, R1, SH_LSL, 24);
            dp_i(j, C_AL, OP_AND, 0, R0, R2, 0xFF);
            if (live) {
                dp_i(j, C_AL, OP_ADD, 0, R1, RG, Z80J_PZST_OFF);
                mem_r(j, C_AL, 1, 1, R1, R1, RA, SH_LSR, 24);
                dp_i(j, C_AL, OP_AND, 0, RF, RF, PSR_C);
                dp_r(j, C_AL, OP_ORR, 0, RF, RF, R1, SH_LSL, 0);
            }
            emit_write(b, in, RHL);
            break;
        default:                                    /* NOP */
            break;
        }
        break;
    }
}

/*--------------------------------------------------------------------------
 * Unprefixed and DD / FD instructions
 *------------------------------------------------------------------------*/

static void emit_main(jit_block_ctx *b, jit_insn *in, int32 index)
{
    z80j_state *j = b->j;
    uint32 op = in->op;
    uint32 x = op >> 6;
    uint32 y = (op >> 3) & 7;
    uint32 z = op & 7;
    uint32 p = y >> 1;
    uint32 q = y & 1;
    uint32 idx = idx_of(in);
    uint32 live = flags_live(in);
    const jit_insn *prev = (index > 0) ? &b->ins[index - 1] : 0;
    uint32 ra;
    uint32 rr;
    uint32 rh;
    opnd o;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0:                                 /* NOP */
                break;
            case 1:                                 /* EX AF,AF' */
                /* F' and A' are the two words below BC' in the context,
                 * in the order of r3 and r4 (their own offset is not an
                 * ARM immediate). */
                dp_i(j, C_AL, OP_ADD, 0, RAD, RG, (uint32)GOFF(bc2));
                emit_ldm(j, RAD, (1u << R0) | (1u << R1), 1);
                emit_stm(j, RAD, (1u << RF) | (1u << RA), 1);
                mov_r(j, RF, R0, SH_LSL, 0);
                mov_r(j, RA, R1, SH_LSL, 0);
                break;
            case 2:                                 /* DJNZ */
                dp_i(j, C_AL, OP_SUB, 0, RBC, RBC, 0x01000000u);
                cmp_i(j, C_AL, OP_TST, RBC, 0xFF000000u);
                emit_jump_cond(b, C_NE, in, index, 5);
                break;
            case 3:                                 /* JR */
                if (in->internal >= 0) {
                    if (in->busy)
                        emit_idle(j, C_AL);
                    emit_b(j, C_AL, 0, JIT_ADDR(b->ins[in->internal].host));
                } else {
                    emit_exit(b, in->target);
                }
                break;
            default:                                /* JR cc */
                emit_jump_cond(b, emit_cond(j, prev, y - 4), in, index, 5);
                break;
            }
            break;
        case 1:
            if (q == 0) {                           /* LD rr,nn */
                if (p == 2 && in->pre == PRE_FD) {
                    emit_mov_pair(j, R0, in->n);
                    str_g(j, R0, GOFF(iy));
                } else {
                    emit_mov_pair(j, rp_reg(j, in, p, R2), in->n);
                }
            } else {                                /* ADD HL,rr */
                rh = idx_reg(j, in, RLR);
                rr = (p == 2) ? rh : rp_reg(j, in, p, R2);
                if (live && !in->fused) {
                    if (rr == rh) {
                        dp_r(j, C_AL, OP_ADD, 1, rh, rh, rh, SH_LSL, 0);
                        dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_C | PSR_H | PSR_n);
                        dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                        cmp_i(j, C_AL, OP_TST, rh, 0x10000000u);
                        dp_i(j, C_NE, OP_ORR, 0, RF, RF, PSR_H);
                    } else {
                        mov_r(j, R1, rh, SH_LSL, 4);
                        dp_r(j, C_AL, OP_ADD, 1, rh, rh, rr, SH_LSL, 0);
                        dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_C | PSR_H | PSR_n);
                        dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                        cmp_r(j, C_AL, OP_CMN, R1, rr, SH_LSL, 4);
                        dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_H);
                    }
                    j->stats.full_flags++;
                } else {
                    dp_r(j, C_AL, OP_ADD, in->fused ? 1u : 0u, rh, rh, rr, SH_LSL, 0);
                }
                idx_store(j, in, rh);
            }
            break;
        case 2:
            switch (y) {
            case 0:                                 /* LD (BC),A */
            case 2:                                 /* LD (DE),A */
                mov_r(j, R0, RA, SH_LSR, 24);
                emit_write(b, in, (y == 0) ? RBC : RDE);
                break;
            case 1:                                 /* LD A,(BC) */
            case 3:                                 /* LD A,(DE) */
                emit_read(j, (y == 1) ? RBC : RDE);
                mov_r(j, RA, R0, SH_LSL, 24);
                break;
            case 4:                                 /* LD (nn),HL */
                if (in->dyn) {
                    /* Address read when the instruction runs: the
                     * generic write sequence, twice (its stub may lose
                     * r0-r2 and r12: the address is read again). */
                    emit_dyn_addr(j, in);
                    mov_r(j, R0, RHL, SH_LSR, 16);
                    emit_write(b, in, RAD);
                    emit_dyn_addr(j, in);
                    dp_i(j, C_AL, OP_ADD, 0, RAD, RAD, 0x00010000u);
                    mov_r(j, R0, RHL, SH_LSR, 24);
                    emit_write(b, in, RAD);
                    break;
                }
                rh = idx_reg(j, in, R2);
                mov_r(j, R0, rh, SH_LSR, 16);
                emit_write_const(b, in, in->n);
                rh = idx_reg(j, in, R2);
                mov_r(j, R0, rh, SH_LSR, 24);
                emit_write_const(b, in, in->n + 1);
                break;
            case 5:                                 /* LD HL,(nn) */
                if (in->dyn) {
                    emit_dyn_addr(j, in);
                    emit_read(j, RAD);
                    mov_r(j, RHL, R0, SH_LSL, 16);
                    dp_i(j, C_AL, OP_ADD, 0, RAD, RAD, 0x00010000u);
                    emit_read(j, RAD);
                    dp_r(j, C_AL, OP_ORR, 0, RHL, RHL, R0, SH_LSL, 24);
                    break;
                }
                emit_read_const(j, in->n);
                mov_r(j, R2, R0, SH_LSL, 0);
                emit_read_const(j, in->n + 1);
                if (in->pre == PRE_FD) {
                    mov_r(j, R1, R2, SH_LSL, 16);
                    dp_r(j, C_AL, OP_ORR, 0, R1, R1, R0, SH_LSL, 24);
                    str_g(j, R1, GOFF(iy));
                } else {
                    rh = (in->pre == PRE_DD) ? RIX : RHL;
                    mov_r(j, rh, R2, SH_LSL, 16);
                    dp_r(j, C_AL, OP_ORR, 0, rh, rh, R0, SH_LSL, 24);
                }
                break;
            case 6:                                 /* LD (nn),A */
                if (in->dyn) {
                    emit_dyn_addr(j, in);
                    mov_r(j, R0, RA, SH_LSR, 24);
                    emit_write(b, in, RAD);
                    break;
                }
                mov_r(j, R0, RA, SH_LSR, 24);
                emit_write_const(b, in, in->n);
                break;
            default:                                /* LD A,(nn) */
                if (in->dyn) {
                    emit_dyn_addr(j, in);
                    emit_read(j, RAD);
                } else {
                    emit_read_const(j, in->n);
                }
                mov_r(j, RA, R0, SH_LSL, 24);
                break;
            }
            break;
        case 3:                                     /* INC / DEC rr */
            rr = rp_reg(j, in, p, RLR);
            dp_i(j, C_AL, q ? OP_SUB : OP_ADD, 0, rr, rr, 0x00010000u);
            if (p == 2)
                idx_store(j, in, rr);
            break;
        case 4:                                     /* INC r */
        case 5:                                     /* DEC r */
            if (y == 6) {
                ra = emit_mem_addr(j, in);
                emit_read(j, ra);
                mov_r(j, R0, R0, SH_LSL, 24);
                emit_incdec(j, in, R0, z == 5);
                mov_r(j, R0, R0, SH_LSR, 24);
                emit_write(b, in, ra);
            } else if (y == 7) {
                emit_incdec(j, in, RA, z == 5);
            } else {
                o = get8(j, y, idx, R0);
                if (o.reg != R0 || o.lsl != 0)
                    mov_r(j, R0, o.reg, SH_LSL, o.lsl);
                emit_incdec(j, in, R0, z == 5);
                o.reg = R0;
                o.lsl = 0;
                set8(j, y, idx, o);
            }
            break;
        case 6:                                     /* LD r,n */
            if (y == 6) {
                ra = emit_mem_addr(j, in);
                dp_i(j, C_AL, OP_MOV, 0, R0, 0, in->n & 0xFFu);
                emit_write(b, in, ra);
            } else {
                set8(j, y, idx, imm8(in->n));
            }
            break;
        default:
            switch (y) {
            case 0:                                 /* RLCA */
                if (live)
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n | PSR_C);
                dp_r(j, C_AL, OP_MOV, 1, RA, 0, RA, SH_LSL, 1);
                dp_i(j, C_CS, OP_ORR, 0, RA, RA, 0x01000000u);
                if (live)
                    dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                break;
            case 1:                                 /* RRCA */
                dp_r(j, C_AL, OP_MOV, 1, R0, 0, RA, SH_LSR, 25);
                dp_i(j, C_CS, OP_ORR, 0, R0, R0, 0x80);
                mov_r(j, RA, R0, SH_LSL, 24);
                if (live) {
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n | PSR_C);
                    dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                }
                break;
            case 2:                                 /* RLA */
                cmp_i(j, C_AL, OP_TST, RF, PSR_C);
                dp_i(j, C_NE, OP_ORR, 0, RA, RA, 0x00800000u);
                dp_r(j, C_AL, OP_MOV, 1, RA, 0, RA, SH_LSL, 1);
                if (live) {
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n | PSR_C);
                    dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                }
                break;
            case 3:                                 /* RRA */
                mov_r(j, R0, RA, SH_LSR, 24);
                cmp_i(j, C_AL, OP_TST, RF, PSR_C);
                dp_i(j, C_NE, OP_ORR, 0, R0, R0, 0x100);
                dp_r(j, C_AL, OP_MOV, 1, R0, 0, R0, SH_LSR, 1);
                mov_r(j, RA, R0, SH_LSL, 24);
                if (live) {
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n | PSR_C);
                    dp_i(j, C_CS, OP_ORR, 0, RF, RF, PSR_C);
                }
                break;
            case 4:                                 /* DAA */
                emit_b(j, C_AL, 1, j->glue.daa);
                break;
            case 5:                                 /* CPL */
                dp_i(j, C_AL, OP_EOR, 0, RA, RA, 0xFF000000u);
                if (live)
                    dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_H | PSR_n);
                break;
            case 6:                                 /* SCF */
                if (live) {
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n);
                    dp_i(j, C_AL, OP_ORR, 0, RF, RF, PSR_C);
                }
                break;
            default:                                /* CCF */
                if (live) {
                    dp_i(j, C_AL, OP_AND, 0, R0, RF, PSR_C);
                    dp_i(j, C_AL, OP_BIC, 0, RF, RF, PSR_H | PSR_n);
                    dp_i(j, C_AL, OP_EOR, 0, RF, RF, PSR_C);
                    dp_r(j, C_AL, OP_ORR, 0, RF, RF, R0, SH_LSL, 3);
                }
                break;
            }
            break;
        }
        break;

    case 1:
        if (op == 0x76) {                           /* HALT */
            emit_mov32(j, R0, (in->pc + in->len) & 0xFFFFu);
            emit_b(j, C_AL, 0, j->glue.halt);
        } else if (z == 6) {                        /* LD r,(HL) */
            ra = emit_mem_addr(j, in);
            emit_read(j, ra);
            set8(j, y, 0, reg_low(R0));
        } else if (y == 6) {                        /* LD (HL),r */
            ra = emit_mem_addr(j, in);
            get_low(j, z);
            emit_write(b, in, ra);
        } else if (y != z) {                        /* LD r,r' */
            o = get8(j, z, idx, R0);
            set8(j, y, idx, o);
        }
        break;

    case 2:                                         /* ALU A,r */
        if (z == 6) {
            ra = emit_mem_addr(j, in);
            if (y != 7 || in->fused || live)
                emit_read(j, ra);
            o = reg_low(R0);
        } else {
            o = get8(j, z, idx, R0);
        }
        emit_alu(j, in, y, o);
        break;

    default:
        switch (z) {
        case 0:                                     /* RET cc */
            {
                jit_stub *s;
                uint32 cond = emit_cond(j, prev, y);
                s = new_stub(b, STUB_TAKEN);
                s->index = index;
                emit(j, (cond << 28) | (5u << 25));
            }
            break;
        case 1:
            if (q == 0) {                           /* POP */
                emit_pop(j);
                if (p == 3) {                       /* AF */
                    dp_i(j, C_AL, OP_ADD, 0, R0, RG, Z80J_FDEC_OFF);
                    mem_r(j, C_AL, 1, 1, RF, R0, R2, SH_LSL, 0);
                    mov_r(j, RA, R1, SH_LSL, 24);
                } else if (p == 2 && in->pre == PRE_FD) {
                    mov_r(j, R0, R2, SH_LSL, 16);
                    dp_r(j, C_AL, OP_ORR, 0, R0, R0, R1, SH_LSL, 24);
                    str_g(j, R0, GOFF(iy));
                } else {
                    rr = (p == 0) ? RBC : (p == 1) ? RDE : (in->pre == PRE_DD) ? RIX : RHL;
                    mov_r(j, rr, R2, SH_LSL, 16);
                    dp_r(j, C_AL, OP_ORR, 0, rr, rr, R1, SH_LSL, 24);
                }
            } else if (p == 0) {                    /* RET */
                emit_pop(j);
                dp_r(j, C_AL, OP_ORR, 0, R0, R2, R1, SH_LSL, 8);
                emit_dynamic(b);
            } else if (p == 1) {                    /* EXX */
                /* BC', DE' and HL' are consecutive in the context. */
                dp_i(j, C_AL, OP_ADD, 0, RAD, RG, (uint32)GOFF(bc2));
                emit_ldm(j, RAD, (1u << R0) | (1u << R1) | (1u << R2), 0);
                emit_stm(j, RAD, (1u << RBC) | (1u << RDE) | (1u << RHL), 0);
                mov_r(j, RBC, R0, SH_LSL, 0);
                mov_r(j, RDE, R1, SH_LSL, 0);
                mov_r(j, RHL, R2, SH_LSL, 0);
            } else if (p == 2) {                    /* JP (HL) */
                rh = idx_reg(j, in, R0);
                mov_r(j, R0, rh, SH_LSR, 16);
                emit_dynamic(b);
            } else {                                /* LD SP,HL */
                rh = idx_reg(j, in, R0);
                mov_r(j, RSP, rh, SH_LSL, 0);
            }
            break;
        case 2:                                     /* JP cc,nn */
            emit_jump_cond(b, emit_cond(j, prev, y), in, index, 0);
            break;
        case 3:
            switch (y) {
            case 0:                                 /* JP nn */
                if (in->dyn) {
                    emit_dyn_exit(b, in);
                } else if (in->internal >= 0) {
                    if (in->busy)
                        emit_idle(j, C_AL);
                    emit_b(j, C_AL, 0, JIT_ADDR(b->ins[in->internal].host));
                } else {
                    emit_exit(b, in->target);
                }
                break;
            case 2:                                 /* OUT (n),A */
                dp_i(j, C_AL, OP_MOV, 0, R1, 0, in->n & 0xFFu);
                mov_r(j, R0, RA, SH_LSR, 24);
                time_left(j, in);
                emit_b(j, C_AL, 1, port_handler(j->ctx->port_out, in->n));
                emit_leave_check(b, in);
                break;
            case 3:                                 /* IN A,(n) */
                dp_i(j, C_AL, OP_MOV, 0, R1, 0, in->n & 0xFFu);
                time_left(j, in);
                emit_b(j, C_AL, 1, port_handler(j->ctx->port_in, in->n));
                mov_r(j, RA, R0, SH_LSL, 24);
                break;
            case 4:                                 /* EX (SP),HL */
                emit_read(j, RSP);
                mov_r(j, R2, R0, SH_LSL, 16);
                dp_i(j, C_AL, OP_ADD, 0, RAD, RSP, 0x00010000u);
                emit_read(j, RAD);
                dp_r(j, C_AL, OP_ORR, 0, R2, R2, R0, SH_LSL, 24);
                str_g(j, R2, GOFF(scratch));
                rh = idx_reg(j, in, R2);
                mov_r(j, R0, rh, SH_LSR, 16);
                emit_write(b, in, RSP);
                rh = idx_reg(j, in, R2);
                mov_r(j, R0, rh, SH_LSR, 24);
                dp_i(j, C_AL, OP_ADD, 0, RAD, RSP, 0x00010000u);
                emit_write(b, in, RAD);
                if (in->pre == PRE_FD) {
                    ldr_g(j, R0, GOFF(scratch));
                    str_g(j, R0, GOFF(iy));
                } else {
                    ldr_g(j, (in->pre == PRE_DD) ? RIX : RHL, GOFF(scratch));
                }
                break;
            case 5:                                 /* EX DE,HL */
                mov_r(j, R0, RDE, SH_LSL, 0);
                mov_r(j, RDE, RHL, SH_LSL, 0);
                mov_r(j, RHL, R0, SH_LSL, 0);
                break;
            case 6:                                 /* DI */
            case 7:                                 /* EI */
                dp_i(j, C_AL, OP_MOV, 0, R0, 0, (y == 7) ? 1u : 0u);
                str_g(j, R0, GOFF(iff1));
                str_g(j, R0, GOFF(iff2));
                break;
            default:
                break;
            }
            break;
        case 4:                                     /* CALL cc,nn */
            {
                jit_stub *s;
                uint32 cond = emit_cond(j, prev, y);
                s = new_stub(b, STUB_TAKEN);
                s->index = index;
                emit(j, (cond << 28) | (5u << 25));
            }
            break;
        case 5:
            if (q == 0) {                           /* PUSH */
                if (p == 3) {
                    dp_i(j, C_AL, OP_ADD, 0, R1, RG, Z80J_FENC_OFF);
                    mem_r(j, C_AL, 1, 1, R0, R1, RF, SH_LSL, 0);
                    dp_r(j, C_AL, OP_ORR, 0, R0, R0, RA, SH_LSR, 16);
                } else {
                    rr = rp_reg(j, in, p, R0);
                    mov_r(j, R0, rr, SH_LSR, 16);
                }
                emit_push(j);
            } else {                                /* CALL nn */
                emit_mov32(j, R0, (in->pc + in->len) & 0xFFFFu);
                emit_push(j);
                if (in->dyn)
                    emit_dyn_exit(b, in);
                else
                    emit_exit(b, in->target);
            }
            break;
        case 6:                                     /* ALU A,n */
            emit_alu(j, in, y, imm8(in->n));
            break;
        default:                                    /* RST */
            emit_mov32(j, R0, (in->pc + in->len) & 0xFFFFu);
            emit_push(j);
            emit_exit(b, in->target);
            break;
        }
        break;
    }
}

/*--------------------------------------------------------------------------
 * Blocks
 *------------------------------------------------------------------------*/

static void emit_insn(jit_block_ctx *b, jit_insn *in, int32 index)
{
    switch (in->pre) {
    case PRE_CB:
    case PRE_DDCB:
    case PRE_FDCB:
        emit_cb(b, in);
        break;
    case PRE_ED:
        emit_ed(b, in, index);
        break;
    default:
        emit_main(b, in, index);
        break;
    }
}

/* Taken path of CALL cc / RET cc. */
static void emit_taken(jit_block_ctx *b, jit_insn *in)
{
    z80j_state *j = b->j;

    if (in->kind == K_CALLCC) {
        dp_i(j, C_AL, OP_SUB, 0, RCYC, RCYC, 7u * CYCLE);
        emit_mov32(j, R0, (in->pc + in->len) & 0xFFFFu);
        emit_push(j);
        if (in->dyn)
            emit_dyn_exit(b, in);
        else
            emit_exit(b, in->target);
    } else {
        dp_i(j, C_AL, OP_SUB, 0, RCYC, RCYC, 6u * CYCLE);
        emit_pop(j);
        dp_r(j, C_AL, OP_ORR, 0, R0, R2, R1, SH_LSL, 8);
        emit_dynamic(b);
    }
}

void emit_block_code(jit_block_ctx *b)
{
    z80j_state *j = b->j;
    jit_insn *ins = b->ins;
    int32 n = b->n;
    int32 i;
    int32 k;
    uint32 *miss_site = 0;
    uint32 *d;
    uint32 host;

    b->nstubs = 0;

    /* Entry check. */
    if (b->entry_check == 1) {
        uint32 slot = (b->key >> 16) - 1;
        uint32 bank = b->key & 0xFFFFu;
        ldr_g(j, R0, GOFF(slot_bank) + (int32)(4 * slot));
        if (bank < 256) {
            cmp_i(j, C_AL, OP_CMP, R0, bank);
        } else {
            emit_mov32(j, R1, bank);
            cmp_r(j, C_AL, OP_CMP, R0, R1, SH_LSL, 0);
        }
        miss_site = j->cur;
        emit(j, (C_NE << 28) | (5u << 25));
    } else if (b->entry_check == 2) {
        /* The memory words holding the block, from the one holding its
         * first byte, then a mask: zero outside the block and, with
         * dynamic operands, on the bytes the code reads for itself. */
        uint32 len = (b->next_pc - ins[0].pc) & 0xFFFFu;
        uint32 lead = ins[0].pc & 3;
        uint32 nwords = (lead + len + 3) >> 2;
        uint32 w = 0;
        uint32 k2;
        uint32 pass;
        emit_b(j, C_AL, 1, j->glue.verify);
        emit(j, ins[0].pc);
        emit(j, nwords);
        for (pass = 0; pass < 2; pass++) {
            for (k2 = 0; k2 < nwords * 4; k2++) {
                uint32 v = 0;

                if (k2 >= lead && k2 < lead + len) {
                    uint32 a = (ins[0].pc + k2 - lead) & 0xFFFFu;

                    if (pass == 0) {
                        v = jit_byte(j->ctx, a);
                    } else {
                        v = 0xFFu;
                        for (i = 0; i < n; i++) {
                            if (ins[i].dyn && ((a - (ins[i].pc + ins[i].len - 2)) & 0xFFFFu) < 2)
                                v = 0;
                        }
                    }
                }
                w = (w << 8) | v;
                if ((k2 & 3) == 3) {
                    emit(j, w);
                    w = 0;
                }
            }
        }
    }

    /* The run mark of the chunk holding the block (see z80j_state): the
     * links of other blocks lead here, past the entry check. */
    b->body = j->cur;
    str_g(j, RG, (int32)b->mark_off);

    b->skip_site = 0;
    for (i = 0; i < n; i++) {
        jit_insn *in = &ins[i];

        if (b->skip_site != 0 && i == b->skip_to) {
            patch_b(b->skip_site, JIT_ADDR(j->cur));
            b->skip_site = 0;
        }
        in->host = j->cur;
        if (in->seg_start) {
            jit_stub *s;
            dp_i(j, C_AL, OP_SUB, 1, RCYC, RCYC, in->seg_t * CYCLE);
            s = new_stub(b, STUB_TIMEOUT);
            s->pc = in->pc;
            s->t = in->seg_t;
            s->resume = in->host;
            emit(j, (C_MI << 28) | (5u << 25));
        }
        emit_insn(b, in, i);
        if (in->irq_check) {
            /* The instruction after EI: a pending interrupt is taken
             * now (the block is left for it), otherwise the block goes
             * on. */
            ldr_g(j, R0, GOFF(irq_line));
            ldr_g(j, R1, GOFF(nmi));
            dp_r(j, C_AL, OP_ORR, 1, R0, R0, R1, SH_LSL, 0);
            emit_stub_branch(b, C_NE, 0, STUB_LEAVE, (in->pc + in->len) & 0xFFFFu,
                             in->t_after, 0);
        }
    }

    if (b->skip_site != 0) {
        patch_b(b->skip_site, JIT_ADDR(j->cur));
        b->skip_site = 0;
    }
    /* Fall-through exit when the block did not end with a transfer. */
    if (!IS_END(ins[n - 1].kind))
        emit_exit(b, b->next_pc);

    /* Stubs. */
    if (miss_site != 0) {
        patch_b(miss_site, JIT_ADDR(j->cur));
        emit_mov32(j, R0, ins[0].pc);
        emit_b(j, C_AL, 0, j->glue.miss);
    }
    for (k = 0; k < b->nstubs; k++) {
        jit_stub *s = &b->stubs[k];

        switch (s->kind) {
        case STUB_TIMEOUT:
            patch_b(s->site, JIT_ADDR(j->cur));
            emit_b(j, C_AL, 1, j->glue.seg_timeout);
            emit(j, s->t * CYCLE);
            emit(j, s->pc);
            emit(j, JIT_ADDR(s->resume));
            break;
        case STUB_FORWARD:
            patch_b(s->site, JIT_ADDR(ins[s->index].host));
            break;
        case STUB_LINK:
            patch_b(s->site, JIT_ADDR(j->cur));
            emit_b(j, C_AL, 1, j->glue.link);
            d = j->cur;
            emit(j, s->pc);
            emit(j, JIT_ADDR(s->site));
            emit(j, b->key);
            host = jit_link_host(j, s->pc, b->key, JIT_ADDR(d));
            if (host != 0)
                jit_link_stub(j, d, host);
            break;
        case STUB_TAKEN:
            patch_b(s->site, JIT_ADDR(j->cur));
            emit_taken(b, &ins[s->index]);
            break;
        case STUB_DYN:
            patch_b(s->site, JIT_ADDR(j->cur));
            emit_dyn_exit(b, &ins[s->index]);
            break;
        case STUB_WRITE:
            patch_b(s->site, JIT_ADDR(j->cur));
            if (s->reg != RAD)
                mov_r(j, RAD, s->reg, SH_LSL, 0);
            mov_r(j, R2, RLR, SH_LSL, 0);
            emit_b(j, C_AL, 1, j->glue.write);
            emit(j, s->pc);
            emit(j, s->t * CYCLE);
            emit(j, b->key);
            break;
        default:                                    /* STUB_LEAVE */
            patch_b(s->site, JIT_ADDR(j->cur));
            emit_mov32(j, R0, s->pc);
            dp_i(j, C_AL, OP_MOV, 0, R1, 0, s->t * CYCLE);
            emit_b(j, C_AL, 0, j->glue.leave);
            break;
        }
    }
    if (j->zone_end - j->cur < 16)
        j->error = 4;                   /* code buffer full */
}
