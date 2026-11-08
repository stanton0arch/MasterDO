#ifndef Z80JIT_INT_H
#define Z80JIT_INT_H

/*
 * Internals shared by the parts of the translator: z80jit.c (decoding,
 * analysis, block management) and z80jit_emit.c (ARM code generation).
 */

#include "z80jit.h"

/* ARM registers of the generated code. */
#define R0   0
#define R1   1
#define R2   2
#define RF   3      /* Z80 F, internal format */
#define RA   4      /* A in bits 24-31 */
#define RBC  5      /* register pairs in bits 16-31 */
#define RDE  6
#define RHL  7
#define RCYC 8      /* T-states left in the scanline, times CYCLE */
#define RIX  9
#define RG   10     /* global pointer */
#define RSP  11
#define RAD  12     /* memory address */
#define RLR  14
#define RPC  15

/* Z80 flags as kept in r3. S, Z, C and V are the ARM N, Z, C and V bits
 * (as left by MRS and a shift by 28). */
#define PSR_S 0x08u
#define PSR_Z 0x04u
#define PSR_C 0x02u
#define PSR_V 0x01u
#define PSR_P 0x01u
#define PSR_n 0x80u
#define PSR_X 0x40u
#define PSR_Y 0x20u
#define PSR_H 0x10u

/* Flag sets for the liveness analysis. */
#define FL_S   0x01u
#define FL_Z   0x02u
#define FL_H   0x04u
#define FL_PV  0x08u
#define FL_N   0x10u
#define FL_C   0x20u
#define FL_ALL 0x3Fu

/* Z80 flags valid in the ARM flags after a fused producer. */
#define FU_Z_EQ 0x01u   /* Z80 Z is ARM Z */
#define FU_Z_CS 0x02u   /* Z80 Z is ARM C (8-bit increment of a register) */
#define FU_C_CS 0x04u   /* Z80 C is ARM C */
#define FU_C_CC 0x08u   /* Z80 C is ARM C inverted (subtraction) */
#define FU_S_MI 0x10u   /* Z80 S is ARM N */
#define FU_V_VS 0x20u   /* Z80 P/V (overflow) is ARM V */

#define CYCLE       256
#define LINE_CYCLES 228
#define MAX_STRETCH 8192         /* scanlines; keeps the counter below 2^31 */

/* Prefixes. */
#define PRE_NONE 0
#define PRE_CB   1
#define PRE_ED   2
#define PRE_DD   3
#define PRE_FD   4
#define PRE_DDCB 5
#define PRE_FDCB 6

/* Instruction kinds. */
#define K_NORMAL 0
#define K_JRCC   1      /* conditional: taken path leaves the fall-through */
#define K_JPCC   2
#define K_CALLCC 3
#define K_RETCC  4
#define K_DJNZ   5
#define K_REP    6      /* repeated block instruction (LDIR...) */
#define K_JR     7      /* unconditional, static target */
#define K_JP     8
#define K_CALL   9
#define K_RST    10
#define K_RET    11     /* unconditional, dynamic target */
#define K_RETN   12
#define K_JPIND  13
#define K_HALT   14

#define IS_COND(k)   ((k) >= K_JRCC && (k) <= K_REP)
#define IS_STATIC(k) ((k) >= K_JR && (k) <= K_RST)
#define IS_END(k)    ((k) >= K_JR)

/* Decoded instruction, packed: the fields of a byte or less share words,
 * so that the decoder writes six words with one STM and each pass loads
 * one word where it loaded several. The offsets in the comments are the
 * byte offsets of the fields (big-endian words), which the assembly reads
 * with LDRB. CONTRACT with z80jit_dec.s, z80jit_ana.s and z80jit_emt.s
 * (offsets checked in z80jit.c). */
typedef struct {
    /* decoded (jit_decode): the first six words */
    uint32  pc;             /*  0 */
    uint32  target;         /*  4 static target; a repeated block instruction: itself */
    uint8   ei;             /*  8 EI: the block ends after the next instruction */
    uint8   mem;            /*  9 (HL) or (IX+d) operand: H and L stay H and L */
    int8    d;              /* 10 (IX+d) displacement */
    uint8   dyn;            /* 11 JP / CALL in RAM: the target is read from */
                            /*    memory when the instruction runs (block decoder) */
    uint32  n;              /* 12 immediate, 8 or 16 bits */
    uint8   len;            /* 16 */
    uint8   pre;            /* 17 PRE_* */
    uint8   op;             /* 18 opcode byte after the prefixes */
    uint8   t;              /* 19 T-states, branch not taken / not repeated */
    uint8   kind;           /* 20 */
    uint8   cc;             /* 21 condition 0-7 (NZ Z NC C PO PE P M) */
    uint8   use;            /* 22 flags read */
    uint8   def;            /* 23 flags written */
    /* set by the block decoder (jit_decode_block) */
    int8    internal;       /* 24 index of the target inside the block, or -1 */
    uint8   busy;           /* 25 jump back of a busy-wait loop (analysis) */
    uint8   irq_check;      /* 26 instruction after EI: check for a pending */
                            /*    interrupt after it */
    uint8   seg_start;      /* 27 a cycle check precedes the instruction: 1 at */
                            /*    the block start, after a conditional */
                            /*    instruction and at a jump target, 2 where */
                            /*    the T-states of the segment would exceed */
                            /*    SEG_MAX_T (the decoder; the analysis cuts */
                            /*    again when a target moves the cuts) */
    uint32  run;            /* 28 first of a run of port outputs: its length, */
                            /*    the mask of its OUT (C),r elements << 8 and */
                            /*    the opcode of OUT (C),r << 24; RUN_PART for */
                            /*    the others; RUN_LOOP | NOP count; or RUN_GRP */
                            /*    (below) */
                            /*    (analysis) */
    uint8   fused;          /* 32 FU_* valid in the ARM flags for the next jump */
    uint8   pad[3];
    /* analysis */
    uint8   live_after;     /* 36 flags read later on the fall-through path */
    uint8   live_before;    /* 37 flags read from this instruction on */
    uint8   seg_t;          /* 38 T-states of the segment starting here */
    uint8   t_after;        /* 39 T-states of its segment after this instruction */
    uint32 *host;           /* 40 generated code of the instruction */
    uint32  pad2;           /* 44 (a 48-byte record: index * 48 in two adds) */
} jit_insn;

#define JIT_DEC_DYN   0x4u  /* some operand is read when it runs */
#define JIT_DEC_SEAM  0x8u  /* the block was cut at a seam of RAM */
#define JIT_DEC_EDIO  0x10u /* INI, IND, OUTI, OUTD or OUT (C),A / D / E met: */
                            /* the analysis looks for runs and port loops */
#define JIT_DEC_RECUT 0x20u /* a jump target lies before a segment the */
                            /* decoder cut for its length: the analysis */
                            /* cuts the segments again */
#define JIT_DEC_NOUT_SHIFT 16 /* bits 16-31: OUTI, OUTD and OUT (n),A met */

/* Branches resolved at the end of the block. CONTRACT with z80jit_emt.s
 * (offsets checked in z80jit.c). */
#define STUB_TIMEOUT 0      /* segment check -> time-out stub */
#define STUB_FORWARD 1      /* jump forward inside the block */
#define STUB_LINK    2      /* jump to untranslated code -> linker stub */
#define STUB_TAKEN   3      /* CALL cc / RET cc taken path */
#define STUB_WRITE   4      /* special write */
#define STUB_LEAVE   5      /* leave the block after a special write / OUT */
#define STUB_DYN     6      /* conditional JP in RAM taken: dynamic target */

typedef struct {
    uint32  kind;
    uint32 *site;           /* branch to patch */
    uint32  pc;             /* Z80 address */
    uint32  t;              /* T-states to give back / segment T-states */
    uint32 *resume;         /* STUB_TIMEOUT: segment check to resume at */
    int32   index;          /* instruction */
    uint32  reg;            /* STUB_WRITE: register holding the address */
} jit_stub;

/* The assembly emitter (z80jit_emt.s) keeps its own stub records in the
 * same array: STUB_WORDS words each (the branch word with the kind and the
 * T-states, the site, a Z80 address, a resume address, register or record),
 * written with one STM and read back with one LDM. The structure above is
 * the one of the C reference emitter of the model (emittest.c). */
#define STUB_WORDS 4

#define RUN_PART   0xFFFFFFFFu
/* INI, IND, OUTI or OUTD closed by JR NZ or JP NZ back to it, with only
 * NOP between them: RUN_LOOP | the number of NOP (RUN_IS_LOOP tells one). */
#define RUN_LOOP   0xFE000000u
#define RUN_IS_LOOP(run) (((run) & 0xFF000000u) == RUN_LOOP)
#define RUN_LOOP_NOPS(run) ((run) & 0xFFu)
/* Element of a group of port outputs sent in one call (jit_out_groups):
 * RUN_GRP | d << 16 | k, with RUN_GRP_N when it goes to the constant port
 * of a group that also has outputs through C; the last one RUN_GRP |
 * RUN_GRP_LAST | n << 16 | count << 8, with RUN_GRP_HASC and RUN_GRP_HASN
 * for the kinds of port its group uses. k is the word of ctx->obuf, d the
 * T-states from the end of the element to the end of the last one, n the
 * constant port. */
#define RUN_GRP      0x80000000u
#define RUN_GRP_LAST 0x40000000u
#define RUN_GRP_N    0x04000000u
#define RUN_GRP_HASN 0x02000000u
#define RUN_GRP_HASC 0x01000000u

#define MAX_INSNS  64
#define MAX_STUBS  (4 * MAX_INSNS)

/* Block descriptor. */
struct z80j_block {
    uint32      pc;         /* Z80 address (bits 0-15); bits 16-21 hold the
                             * flags the block's code reads (its forward
                             * flag scan value) when bit 31 is set */
    uint32      key;        /* page kind and bank of the code */
    uint32     *entry;      /* generated code, entry check included; 0: free */
    uint32     *body;       /* generated code after the entry check */
    z80j_block *next;       /* hash chain, or free list */
    z80j_block *znext;      /* the blocks of the same zone (z80j_state.zone_head) */
};

#define BLK_PC_OF(b)    ((b)->pc & 0xFFFFu)
#define BLK_SCAN_SHIFT  16
#define BLK_SCAN_VALID  0x80000000u

/* Translation of one block, shared by the parts: the block management
 * (z80jit_blk.s) fills the inputs of the decoder, the analysis and the
 * emitter, each of which writes its outputs back here. One static
 * instance (jit_bctx) serves every translation, with its instruction
 * records and stubs. CONTRACT with z80jit_dec.s, z80jit_ana.s,
 * z80jit_blk.s and z80jit_emt.s (offsets checked in z80jit.c): the
 * translator state comes first. */
typedef struct jit_block_ctx_s {
    z80j_state *j;
    jit_insn   *ins;
    int32       n;
    uint32      next_pc;    /* address after the last instruction */
    uint32      key;        /* page kind and bank of the code */
    uint32      end_leave;  /* the block ends after EI + 1: leave */
    jit_stub   *stubs;
    int32       nstubs;     /* (the next free record while the assembly
                             * emitter runs, the count once it is done) */
    uint32      entry_check;/* 0 none, 1 bank check, 2 RAM check */
    uint32      has_dyn;    /* RAM block with dynamic targets: masked check */
    uint32      mark_off;   /* context offset of the run mark of the block's chunk */
    uint32     *body;       /* code after the entry check, where links lead */
    uint32     *skip_site;  /* branch past a port output loop, patched at */
    int32       skip_to;    /* the code of instruction skip_to */
    uint32      pc;         /* address of the block (16 bits) */
    const z80j_ctx *ctx;
    uint32      dec_flags;  /* JIT_DEC_* of the decoding */
    uint32      busy;       /* out of the analysis: busy-wait loops found */
    uint32      line;       /* of which wait for a line */
    uint32      fused;      /* flag tests fused with their jump */
    uint32     *targets;    /* where the analysis reports the static exits (0: not asked) */
    uint32      max_targets;/* room there */
    uint32      ntargets;   /* exits reported */
    uint32      back[2];    /* out of the decoder: a bit per instruction, the
                             * jumps back (JR, JP, JR cc, JP cc to an
                             * instruction of the block at or before them)
                             * the busy-wait loop analysis looks at */
} jit_block_ctx;

extern jit_block_ctx jit_bctx;

/* z80jit_dec.s */
/* Opcode records: two words per unprefixed opcode (see dec_entry in
 * z80jit.c). */
extern uint32 jit_dec_tab[2 * 256];
/* Decodes the instruction at pc (16 bits): the first six words of *in. */
void   jit_decode(jit_insn *in, uint32 pc, const z80j_ctx *c);
/* Decodes the instructions of the block b (ins, pc, ctx and key set),
 * with the marks of each (internal, dyn, irq_check, and seg_start for the
 * first instruction, those after a conditional one, the jump targets and
 * those that would take their segment past SEG_MAX_T T-states; busy, run
 * and fused cleared): writes n, next_pc, dec_flags, has_dyn and back,
 * returns n, 0 when the first instruction crosses a seam of RAM. */
int32  jit_decode_block(jit_block_ctx *b);

/* z80jit_dec.s, for jit_flags_needed */
#define JIT_SCAN_INSNS 8    /* instructions walked by the forward flag scan */
#define JIT_SCAN_DEPTH 1    /* conditional jumps whose both arms it follows */
/* Flags that code starting at pc may read before writing them: follows
 * the unconditional static transfers and, JIT_SCAN_DEPTH deep, both arms
 * of the conditional ones, over code that cannot change while the block
 * b is valid (a fixed page, or a slot showing the bank of b's key);
 * anything unknown counts as reading every flag. */
uint32 jit_scan_needed(const jit_block_ctx *b, uint32 pc);

/* z80jit_ana.s */
/* Runs the analysis passes over the decoded block b (the busy-wait
 * loops, the segments, the runs and groups of port outputs, the flag
 * liveness and fusion): writes busy, line, fused and, when targets is
 * set, the static exits and ntargets. */
void   jit_analyse_block(jit_block_ctx *b);

/* z80jit_blk.s, for z80jit_ana.s */
/* Flags the code at pc may read before writing them, for the block b
 * (jit_scan_needed, through the flag scan cache and the descriptor of the
 * block at pc). */
uint32 jit_flags_needed(jit_block_ctx *b, uint32 pc);

/* z80jit.c, for z80jit_ana.s */
/* Whether the loop from ins[h] to the jump back ins[e] waits for an
 * interrupt (1) or for a line (2); 0 when it may do something else. */
int32  jit_busy_loop(const z80j_state *j, const jit_insn *ins, int32 n, int32 h, int32 e);
/* Marks the groups of port outputs of the block sent in one call. */
void   jit_out_groups(const z80j_state *j, jit_insn *ins, int32 n);

/* z80jit_emit.c */
uint32 arm_branch(uint32 cond, uint32 link, uint32 from, uint32 to);
void   patch_b(uint32 *site, uint32 to);

/* z80jit_emt.s */
/* Emits the code of the block at b->j->cur (see translate_block). */
void   jit_emit_block(jit_block_ctx *b);

/* Block keys. */
#define KEY_FIXED 0u
#define KEY_RAM   ((uint32)Z80J_PAGE_RAM << 16)
#define KEY_IS_SLOT(k) (((k) >> 16) >= 1 && ((k) >> 16) <= 4)

/* z80jit.c */
uint32 jit_block_key(const z80j_ctx *ctx, uint32 pc);
uint32 jit_byte(const z80j_ctx *ctx, uint32 addr);
/* Generated code of an exit to target from a block of key from_key,
 * whose link stub data words are at from_site, or 0 when the target is
 * not translated yet. */
uint32 jit_link_host(z80j_state *j, uint32 target, uint32 from_key, uint32 from_site);
/* Makes the link stub whose data words are at d (target, conditional
 * jump site or 0, key), preceded by its call to the linker, a direct
 * branch to host, and logs it; does nothing when the log is full. */
void   jit_link_stub(z80j_state *j, uint32 *d, uint32 host);
/* Moves an area on to another zone, emptied first if it holds code. */
void   jit_next_zone(z80j_state *j, z80j_area *a);
/* Undoes the direct branches recorded in the link log that lead into the
 * code between lo and hi, drops the entries whose own words lie there,
 * and returns the number of branches undone (z80jit_blk.s). */
uint32 jit_unlink_zone(z80j_state *j, uint32 lo, uint32 hi);

/* z80jit_blk.s */
/* Translates the block at pc: the block management around the decoding,
 * the analysis and the emission. Returns 0 with j->error set when the
 * block could not be translated (5: nothing decodable, 6: no block
 * descriptor could be freed). With a non-null
 * targets, the static exits of the block are written there (at most
 * max_targets), their number in jit_bctx.ntargets. */
z80j_block *jit_translate_block(z80j_state *j, uint32 pc, uint32 *targets,
                                uint32 max_targets);

#endif /* Z80JIT_INT_H */
