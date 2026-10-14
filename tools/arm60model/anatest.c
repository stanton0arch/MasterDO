/*
 * Check of the analysis passes in assembly (src/z80jit_ana.s) against the
 * passes they replaced, written in C (below, as they stood in
 * translate_block, apart from the names), run in the ARM60 model with
 * "sim -anatest": random blocks of code built from instruction templates
 * (jumps and calls inside and outside the block, ALU and INC / DEC
 * producers before conditional jumps, OUTI / OUTD / OUT (C),r runs, OUTI
 * loops, busy-wait loops reading memory or a line port, EI, BIT, block
 * instructions) over pages of every kind, decoded by jit_decode_block and
 * analysed both ways from the same records; the fields the analysis
 * writes (internal, busy, run, seg_start, fused, live_after, live_before,
 * seg_t at segment starts, t_after) and the counts are compared.
 */

#include "z80jit_int.h"
#include "string.h"
#include "stdio.h"
#include "mem.h"

#define SEG_MAX_T   200
#define jit_pcmap (jit_dec_tab + 256)

static uint32 ref_run_elem(uint32 first, uint32 op, uint32 *outc)
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

static uint32 ref_fuse_mask(const jit_insn *in)
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
static uint32 ref_cc_fuse_need(uint32 cc)
{
    switch (cc) {
    case 0: case 1: return FU_Z_EQ | FU_Z_CS;
    case 2: case 3: return FU_C_CS | FU_C_CC;
    case 4: case 5: return FU_V_VS;
    default:        return FU_S_MI;
    }
}


/* The passes of translate_block as they were. */
static void ref_analyse(z80j_state *j, jit_block_ctx *bc, jit_insn *ins, int32 n,
                        uint32 pc, uint32 next_pc, uint32 serial,
                        uint32 *busy, uint32 *line, uint32 *fused)
{
    jit_insn *end = ins + n;
    jit_insn *seg;
    jit_insn *p;
    uint32 addr = next_pc;
    uint32 live;
    uint32 run;
    uint32 span;
    int32 k;

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
        m = jit_pcmap[p->target & 0xFFu];
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
            (p->busy = (uint32)jit_busy_loop(j, ins, n, p->internal, k)) != 0) {
            (*busy)++;
            if (p->busy == 2)
                (*line)++;
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
        e = ref_run_elem(p->op, p->op, &outc);
        for (q = p; e != 0; ) {
            if (e == 2)
                mask |= (uint32)1 << len;
            len++;
            if (q->irq_check || len == Z80J_RUN_MAX)
                break;
            q++;
            if (q >= end || q->seg_start || q->pre != PRE_ED)
                break;
            e = ref_run_elem(p->op, q->op, &outc);
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
        live = end[-1].dyn ? FL_ALL : jit_flags_needed(bc, end[-1].target);
        break;
    case K_RET: case K_RETN: case K_JPIND: case K_HALT:
        live = FL_ALL;
        break;
    default:
        live = jit_flags_needed(bc, next_pc);
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
                taken = jit_flags_needed(bc, p->target);
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
        fm = ref_fuse_mask(p) & ref_cc_fuse_need(c->cc);
        if (fm == 0)
            continue;
        if (c->kind == K_RETCC || c->dyn)
            after = FL_ALL;
        else if (c->internal >= 0 && &ins[c->internal] > c)
            after = ins[c->internal].live_before;
        else
            after = jit_flags_needed(bc, c->target);
        after |= c->live_after;
        if ((p->def & after) == 0) {
            p->fused = fm;
            (*fused)++;
        }
    }

}

static uint32 seed = 777;

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

/* Writes a block of code at pc: about len bytes of random instructions
 * whose jumps mostly land inside it (also used by the emitter test). */
void ana_gen_code(const z80j_ctx *c, uint32 pc, uint32 len)
{
    uint32 at = 0;
    uint32 r;

    while (at < len) {
        uint32 kind = rnd() % 40;
        uint8 b[6];
        uint32 nb = 1;
        int32 disp;
        uint32 target;

        r = rnd();
        switch (kind) {
        case 0: case 1: case 2:                 /* JR cc d, JR d, DJNZ d */
            b[0] = (kind == 2) ? 0x10 : (kind == 1) ? 0x18 : (uint8)(0x20 | ((r & 3) << 3));
            disp = (int32)(rnd() % 40) - 24;
            b[1] = (uint8)disp;
            nb = 2;
            break;
        case 3: case 4:                         /* JP cc nn, JP nn, CALL cc nn, CALL nn */
            b[0] = (r & 1) ? (uint8)(0xC2 | ((r >> 1) & 7) << 3) : (uint8)((r & 2) ? 0xC3 : 0xCD);
            if ((r & 4) == 0)
                b[0] = (uint8)(0xC4 | (((r >> 3) & 7) << 3));
            target = (r & 8) ? (pc + at + (rnd() % 60) - 20) & 0xFFFFu : rnd() & 0xFFFFu;
            b[1] = (uint8)target;
            b[2] = (uint8)(target >> 8);
            nb = 3;
            break;
        case 5: case 6: case 7:                 /* ALU A,r / ALU A,n */
            if (r & 1) {
                b[0] = (uint8)(0x80 | ((r >> 1) & 0x3F));
            } else {
                b[0] = (uint8)(0xC6 | (((r >> 1) & 7) << 3));
                b[1] = (uint8)(r >> 8);
                nb = 2;
            }
            break;
        case 8: case 9:                         /* INC / DEC r, INC / DEC rr, ADD HL,rr */
            b[0] = (r & 1) ? (uint8)(0x04 | (((r >> 1) & 7) << 3) | ((r >> 4) & 1))
                           : (uint8)(0x03 | (((r >> 1) & 3) << 4) | ((r >> 3) & 1) << 3);
            if ((r & 0x30) == 0)
                b[0] = (uint8)(0x09 | (((r >> 6) & 3) << 4));
            break;
        case 10: case 11:                       /* OUTI, OUTD, OUT (C),A/D/E, OTIR, OTDR, INI, LDIR, LDI */
            b[0] = 0xED;
            switch ((r >> 2) & 15) {
            case 0: case 1: case 2: case 3: b[1] = 0xA3; break;
            case 4: case 5: b[1] = 0xAB; break;
            case 6: b[1] = 0x79; break;
            case 7: b[1] = 0x51; break;
            case 8: b[1] = 0x59; break;
            case 9: b[1] = 0xB3; break;
            case 10: b[1] = 0xBB; break;
            case 11: b[1] = 0xA2; break;
            case 12: b[1] = 0xB0; break;
            case 13: b[1] = 0xA0; break;
            case 14: b[1] = 0x44; break;        /* NEG */
            default: b[1] = (uint8)(0x42 | (((r >> 6) & 7) << 3)); break; /* SBC / ADC HL,rr, LD (nn),rr */
            }
            nb = 2;
            if ((b[1] & 0xC7) == 0x43) {
                b[2] = (uint8)(r >> 8);
                b[3] = (uint8)(r >> 16);
                nb = 4;
            }
            break;
        case 12:                                /* CB: BIT / rotations */
            b[0] = 0xCB;
            b[1] = (uint8)(r >> 2);
            nb = 2;
            break;
        case 13:                                /* DD / FD prefixed */
            b[0] = (r & 1) ? 0xDD : 0xFD;
            b[1] = (uint8)((r >> 1) & 0xFF);
            b[2] = (uint8)(r >> 9);
            b[3] = (uint8)(r >> 17);
            nb = 4;
            break;
        case 14: case 15:                       /* LD A,(HL), OR A, LD A,(nn), IN A,(n), CP n, AND n */
            switch ((r >> 2) & 7) {
            case 0: b[0] = 0x7E; break;
            case 1: b[0] = 0xB7; break;
            case 2: b[0] = 0x3A; b[1] = (uint8)(r >> 8); b[2] = (uint8)(r >> 16); nb = 3; break;
            case 3: b[0] = 0xDB; b[1] = (r & 0x20) ? 0x7E : (uint8)(r >> 8); nb = 2; break;
            case 4: b[0] = 0xFE; b[1] = (uint8)(r >> 8); nb = 2; break;
            case 5: b[0] = 0xE6; b[1] = (uint8)(r >> 8); nb = 2; break;
            case 6: b[0] = 0x32; b[1] = (uint8)(r >> 8); b[2] = (uint8)(r >> 16); nb = 3; break;
            default: b[0] = 0xA7; break;
            }
            break;
        case 16:                                /* EI, DI, RET, RET cc, RETI, HALT, JP (HL), RST */
            switch ((r >> 2) & 7) {
            case 0: b[0] = 0xFB; break;
            case 1: b[0] = 0xF3; break;
            case 2: b[0] = 0xC9; break;
            case 3: b[0] = (uint8)(0xC0 | (((r >> 5) & 7) << 3)); break;
            case 4: b[0] = 0xED; b[1] = 0x4D; nb = 2; break;
            case 5: b[0] = 0x76; break;
            case 6: b[0] = 0xE9; break;
            default: b[0] = (uint8)(0xC7 | (((r >> 5) & 7) << 3)); break;
            }
            break;
        case 18: case 19:                       /* loops: OUTI / JR NZ, OUTI / JP NZ, a busy wait, a line wait */
            switch ((r >> 2) & 3) {
            case 0: b[0] = 0xED; b[1] = 0xA3; b[2] = 0x20; b[3] = 0xFC; nb = 4; break;
            case 1:
                b[0] = 0xED; b[1] = (r & 0x10) ? 0xAB : 0xA3; b[2] = 0xC2;
                target = (pc + at) & 0xFFFFu;
                b[3] = (uint8)target; b[4] = (uint8)(target >> 8);
                nb = 5;
                break;
            case 2: b[0] = 0x7E; b[1] = 0xB7; b[2] = 0x28; b[3] = 0xFC; nb = 4; break;
            default: b[0] = 0xDB; b[1] = 0x7E; b[2] = 0xFE; b[3] = (uint8)(r >> 8); b[4] = 0x38; b[5] = 0xFA; nb = 6; break;
            }
            break;
        case 17:                                /* LD r,n, LD rr,nn */
            if (r & 1) {
                b[0] = (uint8)(0x06 | (((r >> 1) & 7) << 3));
                b[1] = (uint8)(r >> 8);
                nb = 2;
            } else {
                b[0] = (uint8)(0x01 | (((r >> 1) & 3) << 4));
                b[1] = (uint8)(r >> 8);
                b[2] = (uint8)(r >> 16);
                nb = 3;
            }
            break;
        default:                                /* any byte */
            b[0] = (uint8)(r >> 3);
            break;
        }
        for (r = 0; r < nb; r++)
            *host_of(c, pc + at + r) = b[r];
        at += nb;
    }
}

void ana_test(void)
{
    z80j_ctx *c = (z80j_ctx *)AllocMem(sizeof(z80j_ctx), MEMTYPE_DRAM | MEMTYPE_FILL);
    z80j_state *j = (z80j_state *)AllocMem(sizeof(z80j_state), MEMTYPE_DRAM | MEMTYPE_FILL);
    uint32 *code = (uint32 *)AllocMem(4096, MEMTYPE_DRAM);
    void *descs = AllocMem((int32)z80j_block_bytes(4), MEMTYPE_DRAM);
    uint8 *mem = (uint8 *)AllocMem(0x10000, MEMTYPE_DRAM | MEMTYPE_FILL);
    z80j_glue glue;
    uint32 bad = 0;
    uint32 blocks = 0;
    uint32 insns = 0;
    uint32 nbusy = 0;
    uint32 nline = 0;
    uint32 nfused = 0;
    uint32 nruns = 0;
    uint32 nloops = 0;
    uint32 ninternal = 0;
    uint32 nscans = 0;
    uint32 v;
    uint32 p;
    uint32 round;

    if (c == 0 || j == 0 || code == 0 || descs == 0 || mem == 0) {
        printf("Analysis test: no memory\n");
        return;
    }
    z80j_default_glue(&glue);
    z80j_init(j, c, code, 1024, 1, 0, descs, 4, 0, 0, &glue);
    z80j_set_line_port(j, 0x7E);

    /* Pages: 0-3F fixed, 40-7F slot 1, 80-BF slot 2, C0-FF RAM with E0-FF
     * mirroring C0-DF; layouts changing per round. */
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
            if ((r & 31) == 0)
                kind = (r >> 5) & 15;
            c->page_kind[p] = (uint8)kind;
            if (p >= 0xE0 && (round & 1))
                map_page(c, mem, p, p - 0x20);
            else
                map_page(c, mem, p, (round & 2) ? p : ((p * 37 + round) & 0xFF));
        }
        for (v = 0; v < 4; v++)
            c->slot_bank[v] = rnd() & ((round & 4) ? 0xFFFFFu : 0x1Fu);
        for (v = 0; v < 0x10000; v++)
            mem[v] = (uint8)(rnd() >> 3);
        for (v = 0; v < 1500; v++) {
            uint32 pc = rnd() & 0xFFFFu;
            uint32 key;
            uint32 na;
            uint32 serial = ((round * 1500 + v) + 1) << 8;
            uint32 ba = 0, la = 0, fa = 0;
            jit_dec_block d;
            jit_block_ctx bca;
            jit_block_ctx bcb;
            jit_ana_block a;
            uint32 k;

            if ((v & 7) == 0)
                pc = (pc & 0xFF00u) | (0xF0u + (pc & 15u));     /* near a page end */
            ana_gen_code(c, (pc - 8) & 0xFFFFu, 160);
            key = jit_block_key(c, pc);
            d.ins = ra;
            d.pc = pc;
            d.ctx = c;
            d.key = key;
            d.serial = serial;
            d.next_pc = 0;
            d.flags = 0;
            na = (uint32)jit_decode_block(&d);
            if (na == 0)
                continue;
            memcpy(rb, ra, na * sizeof(jit_insn));
            for (k = 0; k < na; k++) {
                ra[k].live_after = ra[k].live_before = ra[k].seg_t = ra[k].t_after = 0xDEADBEEFu;
                rb[k].live_after = rb[k].live_before = rb[k].seg_t = rb[k].t_after = 0x5A5A5A5Au;
            }
            bca.j = j;
            bca.key = key;
            bca.nmemo = 0;
            bcb = bca;
            j->stats.scan_hits = 0;
            /* The scan cache takes ROM code as immutable; the test rewrites
             * it, so the cache starts empty for each block (the assembly
             * then finds what the C run cached, for the same code). */
            memset(j->scan_kind, 0xFF, sizeof(j->scan_kind));
            ref_analyse(j, &bca, ra, (int32)na, pc, d.next_pc, serial, &ba, &la, &fa);
            a.ins = rb;
            a.n = (int32)na;
            a.pc = pc;
            a.next_pc = d.next_pc;
            a.serial = serial;
            a.bc = &bcb;
            a.busy = a.line = a.fused = 0x12345678u;
            jit_analyse_block(&a);
            blocks++;
            insns += na;
            nbusy += ba;
            nline += la;
            nfused += fa;
            if (a.busy != ba || a.line != la || a.fused != fa) {
                if (bad++ < 10)
                    printf("Analysis test: block at $%04lx (key %08lx): C %lu busy loops (%lu line), "
                           "%lu fused; assembly %lu (%lu), %lu\n", (unsigned long)pc,
                           (unsigned long)key, (unsigned long)ba, (unsigned long)la,
                           (unsigned long)fa, (unsigned long)a.busy, (unsigned long)a.line,
                           (unsigned long)a.fused);
                continue;
            }
            for (k = 0; k < na; k++) {
                const uint32 *x = (const uint32 *)&ra[k];
                const uint32 *y = (const uint32 *)&rb[k];
                uint32 w;

                if (ra[k].internal >= 0)
                    ninternal++;
                if (ra[k].run != 0 && ra[k].run != RUN_PART && ra[k].run != RUN_LOOP)
                    nruns++;
                if (ra[k].run == RUN_LOOP)
                    nloops++;
                for (w = 14; w < 25; w++) {
                    if (w == 23 && !ra[k].seg_start)
                        continue;               /* seg_t is only read at segment starts */
                    if (x[w] != y[w]) {
                        if (bad++ < 10)
                            printf("Analysis test: block at $%04lx (key %08lx), instruction %lu at "
                                   "$%04lx, word %lu: C %08lx, assembly %08lx\n",
                                   (unsigned long)pc, (unsigned long)key, (unsigned long)k,
                                   (unsigned long)ra[k].pc, (unsigned long)w,
                                   (unsigned long)x[w], (unsigned long)y[w]);
                        break;
                    }
                }
                if (w < 25)
                    break;
            }
            nscans += j->stats.scan_hits;
        }
    }
    printf("Analysis test: %lu blocks, %lu instructions, %lu internal targets, %lu busy-wait "
           "loops (%lu waiting for a line), %lu fused tests, %lu output runs, %lu output loops, "
           "%lu flag scans from the cache; %lu differences\n",
           (unsigned long)blocks, (unsigned long)insns, (unsigned long)ninternal,
           (unsigned long)nbusy, (unsigned long)nline, (unsigned long)nfused,
           (unsigned long)nruns, (unsigned long)nloops, (unsigned long)nscans,
           (unsigned long)bad);
}
