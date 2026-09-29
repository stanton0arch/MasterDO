/*
 * Z80 interpreter on the translator's context. See z80int.h.
 *
 * One function holds the registers in locals for the whole run; the
 * prefixes DD and FD redirect HL (and H, L) to IX or IY through pointers
 * and add their T-states, then the unprefixed decoder runs. Flags are
 * kept in the translator's internal layout (z80jit_int.h) and the S, Z
 * and P bits of a byte come from the context's table.
 */

#include "z80int.h"
#include "z80jit_int.h"

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

/* How an unprefixed opcode uses HL, for the DD / FD prefixes: 0 not at
 * all (the prefix is a 4 T-state no-op), 1 the pair or its halves, 2 a
 * memory operand (HL). Built once from the opcode fields. */
static uint8 hl_use[256];

static void init_hl_use(void)
{
    uint32 op;

    for (op = 0; op < 256; op++) {
        uint32 x = op >> 6;
        uint32 y = (op >> 3) & 7;
        uint32 z = op & 7;
        uint32 p = y >> 1;
        uint32 q = y & 1;
        uint32 u = 0;

        switch (x) {
        case 0:
            switch (z) {
            case 1: u = (p == 2 || q == 1) ? 1 : 0; break;
            case 2: case 3: u = (p == 2) ? 1 : 0; break;
            case 4: case 5: case 6: u = (y == 6) ? 2 : (y == 4 || y == 5) ? 1 : 0; break;
            default: break;
            }
            break;
        case 1:
            if (op == 0x76) u = 0;
            else if (y == 6 || z == 6) u = 2;
            else if (y == 4 || y == 5 || z == 4 || z == 5) u = 1;
            break;
        case 2:
            u = (z == 6) ? 2 : (z == 4 || z == 5) ? 1 : 0;
            break;
        default:
            u = (op == 0xE1 || op == 0xE3 || op == 0xE5 || op == 0xE9 || op == 0xF9) ? 1 : 0;
            break;
        }
        hl_use[op] = (uint8)u;
    }
}

#define FS  PSR_S
#define FZ  PSR_Z
#define FC  PSR_C
#define FV  PSR_V
#define FH  PSR_H
#define FN  PSR_n
#define FSZ (PSR_S | PSR_Z)

#define RD8(ad) (*(const uint8 *)JIT_PTR(rt[255 - (((ad) >> 8) & 0xFFu)] + ((ad) & 0xFFFFu)))
#define RD16(ad) ((uint32)RD8(ad) | ((uint32)RD8((ad) + 1) << 8))

/* Writes a byte; a special page goes through the machine, which may ask
 * to leave (interrupt check after the instruction). */
#define WR8(ad, v) do { \
        uint32 wa_ = (ad) & 0xFFFFu; \
        uint32 we_ = c->wtab[wa_ >> 8]; \
        if (we_ != 0) *(uint8 *)JIT_PTR(we_ + wa_) = (uint8)(v); \
        else if (m->write(m, wa_, (v) & 0xFFu) == Z80J_WRITE_LEAVE) leave = 1; \
    } while (0)

#define PUSH(v) do { sp = (sp - 2) & 0xFFFFu; WR8(sp, (v) & 0xFFu); WR8(sp + 1, (v) >> 8); } while (0)
#define POP(v)  do { v = RD16(sp); sp = (sp + 2) & 0xFFFFu; } while (0)

/* Effective address of the (HL) / (IX+d) / (IY+d) operand. */
#define MADDR() (idx ? ((*hlp + (uint32)d) & 0xFFFFu) : hl)

/* 8-bit register operands 0-7 (B C D E H L (HL) A); H and L follow
 * r8p (HL, or IX / IY when the prefix applies to them). */
#define GET8(i, v) do { switch (i) { \
        case 0: v = bc >> 8; break; case 1: v = bc & 0xFFu; break; \
        case 2: v = de >> 8; break; case 3: v = de & 0xFFu; break; \
        case 4: v = *r8p >> 8; break; case 5: v = *r8p & 0xFFu; break; \
        case 6: v = RD8(MADDR()); break; default: v = a; break; } } while (0)
#define SET8(i, v) do { switch (i) { \
        case 0: bc = (bc & 0xFFu) | ((v) << 8); break; case 1: bc = (bc & 0xFF00u) | (v); break; \
        case 2: de = (de & 0xFFu) | ((v) << 8); break; case 3: de = (de & 0xFF00u) | (v); break; \
        case 4: *r8p = (*r8p & 0xFFu) | ((v) << 8); break; case 5: *r8p = (*r8p & 0xFF00u) | (v); break; \
        case 6: WR8(MADDR(), v); break; default: a = (v); break; } } while (0)

/* Register pairs 0-3: BC DE HL SP (HL follows hlp). */
#define GETRP(i, v) do { switch (i) { case 0: v = bc; break; case 1: v = de; break; \
        case 2: v = *hlp; break; default: v = sp; break; } } while (0)
#define SETRP(i, v) do { switch (i) { case 0: bc = (v); break; case 1: de = (v); break; \
        case 2: *hlp = (v); break; default: sp = (v); break; } } while (0)

#define COND(y) ((y) == 0 ? !(f & FZ) : (y) == 1 ? (f & FZ) : (y) == 2 ? !(f & FC) : \
                 (y) == 3 ? (f & FC) : (y) == 4 ? !(f & FV) : (y) == 5 ? (f & FV) : \
                 (y) == 6 ? !(f & FS) : (f & FS))

#define F_ADD8(r, x, y) (uint32)((pzst[(r) & 0xFFu] & FSZ) | (((r) & 0x100u) ? FC : 0) | \
                        (((x) ^ (y) ^ (r)) & FH) | ((((x) ^ (r)) & ((y) ^ (r)) & 0x80u) ? FV : 0))
#define F_SUB8(r, x, y) (uint32)((pzst[(r) & 0xFFu] & FSZ) | FN | (((r) & 0x100u) ? FC : 0) | \
                        (((x) ^ (y) ^ (r)) & FH) | ((((x) ^ (y)) & ((x) ^ (r)) & 0x80u) ? FV : 0))

/* Transfer of control: to translated code, or counted. */
#define TRANSFER(tgt) do { pc = (tgt) & 0xFFFFu; goto transfer; } while (0)

uint32 z80i_run(z80j_ctx *c, z80i_params *p)
{
    const uint32 *rt = c->rtab;
    const uint8 *pzst = c->pzst;
    z80j_machine *m = (z80j_machine *)JIT_PTR(c->machine);
    uint32 a = c->regs[1] >> 24;
    uint32 f = c->regs[0] & 0xFFu;
    uint32 bc = c->regs[2] >> 16;
    uint32 de = c->regs[3] >> 16;
    uint32 hl = c->regs[4] >> 16;
    uint32 sp = c->regs[7] >> 16;
    uint32 ix = c->ix >> 16;
    uint32 iy = c->iy >> 16;
    uint32 pc = c->regs[6] & 0xFFFFu;
    int32 cyc = (int32)c->regs[5];
    uint32 *hlp;            /* HL, or IX / IY under a prefix */
    uint32 *r8p;            /* pair holding H and L for 8-bit operands */
    uint32 idx = 0;         /* prefix: 0 none, 1 IX, 2 IY */
    uint32 d = 0;           /* displacement of (IX+d) */
    uint32 leave = 0;       /* a callback asked to leave: check interrupts */
    uint32 ei = 0;          /* EI: interrupts checked after the next instruction */
    uint32 result;
    uint32 op;
    uint32 x;
    uint32 y;
    uint32 z;
    uint32 v;
    uint32 r;
    uint32 nn;
    uint32 insns = 0;

    if (hl_use[0x09] == 0)
        init_hl_use();

    for (;;) {
        if (leave) {
            result = Z80I_IRQ_CHECK;
            break;
        }
        if (ei) {
            if (ei == 2) {
                ei = 0;
                result = Z80I_IRQ_CHECK;
                break;
            }
            ei = 2;
        }
        if (cyc < 23 * CYCLE) {
            result = Z80I_DONE;
            break;
        }
        hlp = &hl;
        r8p = &hl;
        idx = 0;
        insns++;
        op = RD8(pc);
        pc++;
        cyc -= (int32)t_main[op] * CYCLE;

    main_op:
        x = op >> 6;
        y = (op >> 3) & 7;
        z = op & 7;
        switch (x) {
        case 0:
            switch (z) {
            case 0:
                switch (y) {
                case 0:                                 /* NOP */
                    break;
                case 1:                                 /* EX AF,AF' */
                    v = c->a2 >> 24;
                    c->a2 = a << 24;
                    a = v;
                    v = c->f2;
                    c->f2 = f;
                    f = v & 0xFFu;
                    break;
                case 2:                                 /* DJNZ */
                    d = (uint32)(int32)(int8)RD8(pc);
                    pc++;
                    bc = (bc - 0x100u) & 0xFFFFu;
                    if (bc & 0xFF00u) {
                        cyc -= 5 * CYCLE;
                        TRANSFER(pc + d);
                    }
                    break;
                case 3:                                 /* JR */
                    d = (uint32)(int32)(int8)RD8(pc);
                    pc++;
                    TRANSFER(pc + d);
                default:                                /* JR cc */
                    d = (uint32)(int32)(int8)RD8(pc);
                    pc++;
                    if (COND(y - 4)) {
                        cyc -= 5 * CYCLE;
                        TRANSFER(pc + d);
                    }
                    break;
                }
                break;
            case 1:
                if ((y & 1) == 0) {                     /* LD rr,nn */
                    nn = RD16(pc);
                    pc += 2;
                    SETRP(y >> 1, nn);
                } else {                                /* ADD HL,rr */
                    GETRP(y >> 1, v);
                    r = *hlp + v;
                    f = (f & (FS | FZ | FV)) | ((r & 0x10000u) ? FC : 0) |
                        (((*hlp ^ v ^ r) >> 8) & FH);
                    *hlp = r & 0xFFFFu;
                }
                break;
            case 2:
                switch (y) {
                case 0: WR8(bc, a); break;              /* LD (BC),A */
                case 1: a = RD8(bc); break;             /* LD A,(BC) */
                case 2: WR8(de, a); break;              /* LD (DE),A */
                case 3: a = RD8(de); break;             /* LD A,(DE) */
                case 4:                                 /* LD (nn),HL */
                    nn = RD16(pc);
                    pc += 2;
                    WR8(nn, *hlp & 0xFFu);
                    WR8(nn + 1, *hlp >> 8);
                    break;
                case 5:                                 /* LD HL,(nn) */
                    nn = RD16(pc);
                    pc += 2;
                    *hlp = RD16(nn);
                    break;
                case 6:                                 /* LD (nn),A */
                    nn = RD16(pc);
                    pc += 2;
                    WR8(nn, a);
                    break;
                default:                                /* LD A,(nn) */
                    nn = RD16(pc);
                    pc += 2;
                    a = RD8(nn);
                    break;
                }
                break;
            case 3:                                     /* INC / DEC rr */
                GETRP(y >> 1, v);
                v = (v + ((y & 1) ? 0xFFFFu : 1u)) & 0xFFFFu;
                SETRP(y >> 1, v);
                break;
            case 4:                                     /* INC r */
                GET8(y, v);
                r = (v + 1) & 0xFFu;
                f = (f & FC) | (pzst[r] & FSZ) | ((r & 0x0Fu) == 0 ? FH : 0) | (r == 0x80u ? FV : 0);
                SET8(y, r);
                break;
            case 5:                                     /* DEC r */
                GET8(y, v);
                r = (v - 1) & 0xFFu;
                f = (f & FC) | FN | (pzst[r] & FSZ) | ((r & 0x0Fu) == 0x0Fu ? FH : 0) |
                    (r == 0x7Fu ? FV : 0);
                SET8(y, r);
                break;
            case 6:                                     /* LD r,n */
                v = RD8(pc);
                pc++;
                SET8(y, v);
                break;
            default:
                switch (y) {
                case 0:                                 /* RLCA */
                    v = a >> 7;
                    a = ((a << 1) | v) & 0xFFu;
                    f = (f & (FS | FZ | FV)) | (v ? FC : 0);
                    break;
                case 1:                                 /* RRCA */
                    v = a & 1;
                    a = (a >> 1) | (v << 7);
                    f = (f & (FS | FZ | FV)) | (v ? FC : 0);
                    break;
                case 2:                                 /* RLA */
                    v = a >> 7;
                    a = ((a << 1) | ((f >> 1) & 1)) & 0xFFu;
                    f = (f & (FS | FZ | FV)) | (v ? FC : 0);
                    break;
                case 3:                                 /* RRA */
                    v = a & 1;
                    a = (a >> 1) | ((f & FC) ? 0x80u : 0);
                    f = (f & (FS | FZ | FV)) | (v ? FC : 0);
                    break;
                case 4:                                 /* DAA */
                    {
                        uint32 lo = a & 0x0Fu;
                        uint32 corr = ((f & FH) || lo > 9) ? 6 : 0;
                        uint32 cout = ((f & FC) || a > 0x99u) ? 1 : 0;
                        uint32 h;

                        if (cout)
                            corr |= 0x60u;
                        if (f & FN) {
                            h = ((f & FH) && lo < 6) ? FH : 0;
                            a = (a - corr) & 0xFFu;
                        } else {
                            h = (lo > 9) ? FH : 0;
                            a = (a + corr) & 0xFFu;
                        }
                        f = pzst[a] | (f & FN) | h | (cout ? FC : 0);
                    }
                    break;
                case 5:                                 /* CPL */
                    a ^= 0xFFu;
                    f |= FH | FN;
                    break;
                case 6:                                 /* SCF */
                    f = (f & ~(FH | FN)) | FC;
                    break;
                default:                                /* CCF */
                    v = f & FC;
                    f = ((f & ~(FH | FN)) ^ FC) | (v << 3);
                    break;
                }
                break;
            }
            break;

        case 1:
            if (op == 0x76) {                           /* HALT */
                c->halted = 1;
                c->idle += (uint32)cyc;
                cyc = 0;
                result = Z80I_HALT;
                goto done;
            }
            GET8(z, v);                                 /* LD r,r' */
            SET8(y, v);
            break;

        case 2:                                         /* ALU A,r */
            GET8(z, v);
        alu:
            switch (y) {
            case 0:                                     /* ADD */
                r = a + v;
                f = F_ADD8(r, a, v);
                a = r & 0xFFu;
                break;
            case 1:                                     /* ADC */
                r = a + v + ((f >> 1) & 1);
                f = F_ADD8(r, a, v);
                a = r & 0xFFu;
                break;
            case 2:                                     /* SUB */
                r = a - v;
                f = F_SUB8(r, a, v);
                a = r & 0xFFu;
                break;
            case 3:                                     /* SBC */
                r = a - v - ((f >> 1) & 1);
                f = F_SUB8(r, a, v);
                a = r & 0xFFu;
                break;
            case 4:                                     /* AND */
                a &= v;
                f = pzst[a] | FH;
                break;
            case 5:                                     /* XOR */
                a ^= v;
                f = pzst[a];
                break;
            case 6:                                     /* OR */
                a |= v;
                f = pzst[a];
                break;
            default:                                    /* CP */
                r = a - v;
                f = F_SUB8(r, a, v);
                break;
            }
            break;

        default:
            switch (z) {
            case 0:                                     /* RET cc */
                if (COND(y)) {
                    cyc -= 6 * CYCLE;
                    POP(nn);
                    TRANSFER(nn);
                }
                break;
            case 1:
                if ((y & 1) == 0) {                     /* POP */
                    POP(nn);
                    if (y == 6) {
                        a = nn >> 8;
                        f = c->fdec[nn & 0xFFu];
                    } else {
                        SETRP(y >> 1, nn);
                    }
                } else if (y == 1) {                    /* RET */
                    POP(nn);
                    TRANSFER(nn);
                } else if (y == 3) {                    /* EXX */
                    v = c->bc2 >> 16; c->bc2 = bc << 16; bc = v;
                    v = c->de2 >> 16; c->de2 = de << 16; de = v;
                    v = c->hl2 >> 16; c->hl2 = hl << 16; hl = v;
                } else if (y == 5) {                    /* JP (HL) */
                    TRANSFER(*hlp);
                } else {                                /* LD SP,HL */
                    sp = *hlp;
                }
                break;
            case 2:                                     /* JP cc,nn */
                nn = RD16(pc);
                pc += 2;
                if (COND(y))
                    TRANSFER(nn);
                break;
            case 3:
                switch (y) {
                case 0:                                 /* JP nn */
                    nn = RD16(pc);
                    TRANSFER(nn);
                case 1:                                 /* CB prefix */
                    goto prefix_cb;
                case 2:                                 /* OUT (n),A */
                    v = RD8(pc);
                    pc++;
                    if (m->out(m, v, a, (uint32)cyc))
                        leave = 1;
                    break;
                case 3:                                 /* IN A,(n) */
                    v = RD8(pc);
                    pc++;
                    a = m->in(m, v, (uint32)cyc) & 0xFFu;
                    break;
                case 4:                                 /* EX (SP),HL */
                    nn = RD16(sp);
                    WR8(sp, *hlp & 0xFFu);
                    WR8(sp + 1, *hlp >> 8);
                    *hlp = nn;
                    break;
                case 5:                                 /* EX DE,HL */
                    v = de;
                    de = hl;
                    hl = v;
                    break;
                case 6:                                 /* DI */
                    c->iff1 = 0;
                    c->iff2 = 0;
                    break;
                default:                                /* EI */
                    c->iff1 = 1;
                    c->iff2 = 1;
                    ei = 1;
                    break;
                }
                break;
            case 4:                                     /* CALL cc,nn */
                nn = RD16(pc);
                pc += 2;
                if (COND(y)) {
                    cyc -= 7 * CYCLE;
                    PUSH(pc);
                    TRANSFER(nn);
                }
                break;
            case 5:
                if ((y & 1) == 0) {                     /* PUSH */
                    if (y == 6)
                        nn = (a << 8) | c->fenc[f];
                    else
                        GETRP(y >> 1, nn);
                    PUSH(nn);
                } else if (y == 1) {                    /* CALL nn */
                    nn = RD16(pc);
                    pc += 2;
                    PUSH(pc);
                    TRANSFER(nn);
                } else if (y == 3) {                    /* DD prefix */
                    goto prefix_dd;
                } else if (y == 5) {                    /* ED prefix */
                    goto prefix_ed;
                } else {                                /* FD prefix */
                    goto prefix_dd;
                }
                break;
            case 6:                                     /* ALU A,n */
                v = RD8(pc);
                pc++;
                goto alu;
            default:                                    /* RST */
                PUSH(pc);
                TRANSFER(op & 0x38u);
            }
            break;
        }
        continue;

    prefix_dd:
        /* DD or FD: the next opcode uses IX / IY instead of HL, or the
         * prefix is a 4 T-state no-op and that opcode is decoded afresh
         * (t_main[0xDD] is 0: nothing was charged yet). */
        idx = (op == 0xDD) ? 1 : 2;
        hlp = idx == 1 ? &ix : &iy;
        op = RD8(pc);
        if (op == 0xCB) {
            pc++;
            d = (uint32)(int32)(int8)RD8(pc);
            pc++;
            goto prefix_cb;
        }
        v = hl_use[op];
        if (v == 0) {
            cyc -= 4 * CYCLE;
            continue;
        }
        pc++;
        if (v == 2) {
            d = (uint32)(int32)(int8)RD8(pc);
            pc++;
            r8p = &hl;
            cyc -= (int32)(t_main[op] + (op == 0x36 ? 9 : 12)) * CYCLE;
        } else {
            r8p = hlp;
            cyc -= (int32)(t_main[op] + 4) * CYCLE;
        }
        goto main_op;

    prefix_cb:
        /* CB, or DD CB d / FD CB d (idx set, d fetched): rotations and
         * shifts, BIT, RES, SET. */
        op = RD8(pc);
        pc++;
        x = op >> 6;
        y = (op >> 3) & 7;
        z = op & 7;
        if (idx) {
            cyc -= (int32)((x == 1) ? 20 : 23) * CYCLE;
            nn = (*hlp + d) & 0xFFFFu;
            v = RD8(nn);
        } else if (z == 6) {
            cyc -= (int32)((x == 1) ? 12 : 15) * CYCLE;
            nn = hl;
            v = RD8(nn);
        } else {
            cyc -= 8 * CYCLE;
            GET8(z, v);
        }
        if (x == 0) {
            switch (y) {
            case 0: r = ((v << 1) | (v >> 7)) & 0xFFu; f = pzst[r] | ((v & 0x80u) ? FC : 0); break;
            case 1: r = (v >> 1) | ((v & 1) << 7); f = pzst[r] | ((v & 1) ? FC : 0); break;
            case 2: r = ((v << 1) | ((f >> 1) & 1)) & 0xFFu; f = pzst[r] | ((v & 0x80u) ? FC : 0); break;
            case 3: r = (v >> 1) | ((f & FC) ? 0x80u : 0); f = pzst[r] | ((v & 1) ? FC : 0); break;
            case 4: r = (v << 1) & 0xFFu; f = pzst[r] | ((v & 0x80u) ? FC : 0); break;
            case 5: r = (v >> 1) | (v & 0x80u); f = pzst[r] | ((v & 1) ? FC : 0); break;
            case 6: r = ((v << 1) | 1) & 0xFFu; f = pzst[r] | ((v & 0x80u) ? FC : 0); break;
            default: r = v >> 1; f = pzst[r] | ((v & 1) ? FC : 0); break;
            }
        } else if (x == 1) {                            /* BIT */
            r = v & (1u << y);
            f = (f & FC) | FH | (r ? 0 : (FZ | FV)) | ((y == 7 && r) ? FS : 0);
            continue;
        } else if (x == 2) {                            /* RES */
            r = v & ~(1u << y);
        } else {                                        /* SET */
            r = v | (1u << y);
        }
        if (idx) {
            WR8(nn, r);
            if (z != 6) {                               /* undocumented copy */
                r8p = &hl;
                SET8(z, r);
            }
        } else if (z == 6) {
            WR8(nn, r);
        } else {
            SET8(z, r);
        }
        continue;

    prefix_ed:
        op = RD8(pc);
        pc++;
        x = op >> 6;
        y = (op >> 3) & 7;
        z = op & 7;
        if (x == 1) {
            switch (z) {
            case 0:                                     /* IN r,(C) */
                cyc -= 12 * CYCLE;
                v = m->in(m, bc & 0xFFu, (uint32)cyc) & 0xFFu;
                if (y != 6)
                    SET8(y, v);
                f = (f & FC) | pzst[v];
                break;
            case 1:                                     /* OUT (C),r */
                cyc -= 12 * CYCLE;
                if (y == 6)
                    v = 0;
                else
                    GET8(y, v);
                if (m->out(m, bc & 0xFFu, v, (uint32)cyc))
                    leave = 1;
                break;
            case 2:                                     /* SBC / ADC HL,rr */
                cyc -= 15 * CYCLE;
                GETRP(y >> 1, v);
                if (y & 1) {
                    r = hl + v + ((f >> 1) & 1);
                    f = ((r & 0x8000u) ? FS : 0) | ((r & 0xFFFFu) == 0 ? FZ : 0) |
                        ((r & 0x10000u) ? FC : 0) | (((hl ^ v ^ r) >> 8) & FH) |
                        (((hl ^ r) & (v ^ r) & 0x8000u) ? FV : 0);
                } else {
                    r = hl - v - ((f >> 1) & 1);
                    f = FN | ((r & 0x8000u) ? FS : 0) | ((r & 0xFFFFu) == 0 ? FZ : 0) |
                        ((r & 0x10000u) ? FC : 0) | (((hl ^ v ^ r) >> 8) & FH) |
                        (((hl ^ v) & (hl ^ r) & 0x8000u) ? FV : 0);
                }
                hl = r & 0xFFFFu;
                break;
            case 3:                                     /* LD (nn),rr / LD rr,(nn) */
                cyc -= 20 * CYCLE;
                nn = RD16(pc);
                pc += 2;
                if (y & 1) {
                    v = RD16(nn);
                    SETRP(y >> 1, v);
                } else {
                    GETRP(y >> 1, v);
                    WR8(nn, v & 0xFFu);
                    WR8(nn + 1, v >> 8);
                }
                break;
            case 4:                                     /* NEG */
                cyc -= 8 * CYCLE;
                r = 0u - a;
                f = F_SUB8(r, 0, a);
                a = r & 0xFFu;
                break;
            case 5:                                     /* RETN / RETI */
                cyc -= 14 * CYCLE;
                c->iff1 = c->iff2;
                POP(nn);
                pc = nn;
                leave = 1;
                break;
            case 6:                                     /* IM */
                cyc -= 8 * CYCLE;
                c->im = ((y & 3) == 2) ? 1 : ((y & 3) == 3) ? 2 : 0;
                break;
            default:
                switch (y) {
                case 0:                                 /* LD I,A */
                    cyc -= 9 * CYCLE;
                    c->i = a;
                    break;
                case 1:                                 /* LD R,A */
                    cyc -= 9 * CYCLE;
                    c->r = a;
                    break;
                case 2:                                 /* LD A,I */
                case 3:                                 /* LD A,R */
                    cyc -= 9 * CYCLE;
                    if (y == 2)
                        a = c->i & 0xFFu;
                    else
                        a = ((c->line * 9 + ((uint32)cyc >> 10) + c->r) & 0x7Fu) | (c->r & 0x80u);
                    f = (f & FC) | (pzst[a] & FSZ) | (c->iff2 ? FV : 0);
                    break;
                case 4:                                 /* RRD */
                case 5:                                 /* RLD */
                    cyc -= 18 * CYCLE;
                    v = RD8(hl);
                    if (y == 5) {
                        r = ((v << 4) | (a & 0x0Fu)) & 0xFFu;
                        a = (a & 0xF0u) | (v >> 4);
                    } else {
                        r = ((a & 0x0Fu) << 4) | (v >> 4);
                        a = (a & 0xF0u) | (v & 0x0Fu);
                    }
                    f = (f & FC) | pzst[a];
                    WR8(hl, r);
                    break;
                default:                                /* NOP */
                    cyc -= 8 * CYCLE;
                    break;
                }
                break;
            }
        } else if (x == 2 && z <= 3 && y >= 4) {        /* block instructions */
            uint32 dec = y & 1;
            uint32 rep = y >= 6;
            uint32 step = dec ? 0xFFFFu : 1u;

            cyc -= 16 * CYCLE;
            switch (z) {
            case 0:                                     /* LDI LDD LDIR LDDR */
                v = RD8(hl);
                WR8(de, v);
                hl = (hl + step) & 0xFFFFu;
                de = (de + step) & 0xFFFFu;
                bc = (bc - 1) & 0xFFFFu;
                f = (f & ~(FH | FN | FV)) | (bc ? FV : 0);
                if (rep && bc) {
                    cyc -= 5 * CYCLE;
                    pc -= 2;
                }
                break;
            case 1:                                     /* CPI CPD CPIR CPDR */
                v = RD8(hl);
                r = a - v;
                f = (f & FC) | FN | (pzst[r & 0xFFu] & FSZ) | ((a ^ v ^ r) & FH);
                hl = (hl + step) & 0xFFFFu;
                bc = (bc - 1) & 0xFFFFu;
                if (bc)
                    f |= FV;
                if (rep && bc && !(f & FZ)) {
                    cyc -= 5 * CYCLE;
                    pc -= 2;
                }
                break;
            case 2:                                     /* INI IND INIR INDR */
                v = m->in(m, bc & 0xFFu, (uint32)cyc) & 0xFFu;
                WR8(hl, v);
                hl = (hl + step) & 0xFFFFu;
                bc = (bc - 0x100u) & 0xFFFFu;
                f = (f & ~FZ) | FN | ((bc & 0xFF00u) ? 0 : FZ);
                if (rep && (bc & 0xFF00u)) {
                    cyc -= 5 * CYCLE;
                    pc -= 2;
                }
                break;
            default:                                    /* OUTI OUTD OTIR OTDR */
                v = RD8(hl);
                bc = (bc - 0x100u) & 0xFFFFu;
                if (m->out(m, bc & 0xFFu, v, (uint32)cyc))
                    leave = 1;
                hl = (hl + step) & 0xFFFFu;
                f = (f & ~FZ) | FN | ((bc & 0xFF00u) ? 0 : FZ);
                if (rep && (bc & 0xFF00u)) {
                    cyc -= 5 * CYCLE;
                    pc -= 2;
                }
                break;
            }
        } else {                                        /* NOP */
            cyc -= 8 * CYCLE;
        }
        continue;

    transfer:
        if (c->lookup[pc] != p->miss) {
            result = Z80I_TRANSLATED;
            break;
        }
        if (p->hot != 0) {
            v = p->hot[pc];
            if (v < 255u) {
                v++;
                p->hot[pc] = (uint8)v;
                if (v == p->queue_at || v == p->sync_at) {
                    result = Z80I_HOT;
                    break;
                }
            }
        }
    }

done:
    p->insns += insns;
    c->regs[0] = f;
    c->regs[1] = a << 24;
    c->regs[2] = bc << 16;
    c->regs[3] = de << 16;
    c->regs[4] = hl << 16;
    c->regs[5] = (uint32)cyc;
    c->regs[6] = pc & 0xFFFFu;
    c->regs[7] = sp << 16;
    c->ix = ix << 16;
    c->iy = iy << 16;
    return result;
}
