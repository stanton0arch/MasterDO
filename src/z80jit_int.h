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

typedef struct {
    uint32  pc;
    uint32  len;
    uint32  pre;            /* PRE_* */
    uint32  op;             /* opcode byte after the prefixes */
    int32   d;              /* (IX+d) displacement */
    uint32  n;              /* immediate, 8 or 16 bits */
    uint32  t;              /* T-states, branch not taken / not repeated */
    uint32  kind;
    uint32  cc;             /* condition 0-7 (NZ Z NC C PO PE P M) */
    uint32  target;
    uint32  use;            /* flags read */
    uint32  def;            /* flags written */
    uint32  ei;             /* EI: the block ends after the next instruction */
    uint32  mem;            /* (HL) or (IX+d) operand: H and L stay H and L */
    /* analysis */
    uint32  live_after;     /* flags read later on the fall-through path */
    uint32  live_before;    /* flags read from this instruction on */
    uint32  seg_start;      /* a cycle check precedes the instruction */
    uint32  seg_t;          /* T-states of the segment starting here */
    uint32  t_after;        /* T-states of its segment after this instruction */
    uint32  fused;          /* FU_* valid in the ARM flags for the next jump */
    int32   internal;       /* index of the target inside the block, or -1 */
    uint32  busy;           /* jump back of a busy-wait loop */
    uint32  irq_check;      /* instruction after EI: check for a pending */
                            /* interrupt after it */
    uint32  run;            /* first of a run of port outputs: its length, */
                            /* the mask of its OUT (C),r elements << 8 and */
                            /* the opcode of OUT (C),r << 24; RUN_PART for */
                            /* the others */
    uint32  dyn;            /* JP / CALL in RAM: the target is read from */
                            /* memory when the instruction runs */
    uint32 *host;           /* generated code of the instruction */
} jit_insn;

/* Branches resolved at the end of the block. */
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
    int32   index;          /* instruction */
    uint32  reg;            /* STUB_WRITE: register holding the address */
    uint32 *resume;         /* STUB_TIMEOUT: segment check to resume at */
} jit_stub;

#define RUN_PART   0xFFFFFFFFu
#define RUN_LOOP   0xFFFFFFFEu  /* OUTI / OUTD closed by JR NZ or JP NZ back to it */

#define MAX_INSNS  64
#define MAX_STUBS  (4 * MAX_INSNS)

/* Block descriptor. */
struct z80j_block {
    uint32      pc;         /* Z80 address */
    uint32      key;        /* page kind and bank of the code */
    uint32     *entry;      /* generated code, entry check included; 0: free */
    uint32     *body;       /* generated code after the entry check */
    z80j_block *next;       /* hash chain, or free list */
};

#define SCAN_MEMO 16

/* Translation of one block, shared by the two parts. */
typedef struct {
    z80j_state *j;
    jit_insn   *ins;
    int32       n;
    uint32      next_pc;    /* address after the last instruction */
    uint32      key;        /* page kind and bank of the code */
    uint32      end_leave;  /* the block ends after EI + 1: leave */
    jit_stub   *stubs;
    int32       nstubs;
    uint32      entry_check;/* 0 none, 1 bank check, 2 RAM check */
    uint32      has_dyn;    /* RAM block with dynamic targets: masked check */
    uint32      mark_off;   /* context offset of the run mark of the block's chunk */
    uint32     *body;       /* code after the entry check, where links lead */
    uint32     *skip_site;  /* branch past a port output loop, patched at */
    int32       skip_to;    /* the code of instruction skip_to */
    uint32      memo_pc[SCAN_MEMO];
    uint32      memo_val[SCAN_MEMO];
    int32       nmemo;
} jit_block_ctx;

/* z80jit_emit.c */
uint32 arm_branch(uint32 cond, uint32 link, uint32 from, uint32 to);
void   patch_b(uint32 *site, uint32 to);
void   emit_b(z80j_state *j, uint32 cond, uint32 link, uint32 to);
void   emit_block_code(jit_block_ctx *b);

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

#endif /* Z80JIT_INT_H */
