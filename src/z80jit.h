#ifndef Z80JIT_H
#define Z80JIT_H

/*
 * Z80 to ARM translator (dynamic recompiler).
 *
 * Z80 code is translated into ARM code one block at a time, the first time
 * it runs, and the translation is kept for the following runs.
 * - A block runs through conditional jumps and ends at an unconditional
 *   transfer, at a page of different kind, or after a maximum length.
 * - Z80 registers stay in ARM registers (F r3, A r4, BC r5, DE r6, HL r7,
 *   IX r9, SP r11, 16-bit values in bits 16-31, A in bits 24-31), the
 *   T-state counter in r8 and the context pointer in r10.
 * - T-states are counted per segment (the instructions between two
 *   conditional jumps), with one check per segment. The counter runs down
 *   to the next machine event or to the end of the run, not to the end of
 *   each scanline: generated code only stops a few times per frame.
 * - Flags are only computed when a later instruction can read them; a
 *   flag-setting instruction whose only reader is the next conditional
 *   jump leaves its result in the ARM flags.
 * - Blocks are chained with direct branches; dynamic transfers (RET,
 *   JP (HL)) go through a table of the 65536 possible entry addresses.
 * - A loop that only reads memory and registers it does not modify (for
 *   instance LD A,(HL) / OR A / JR Z) cannot end before an interrupt,
 *   which only comes at the end of a stretch of execution: when its jump
 *   back is taken, the rest of the stretch goes by idle, as in HALT.
 *
 * The emulated machine is described by the context: 256-byte pages for
 * reads and writes, the kind of each page (fixed ROM, paged ROM slot, RAM)
 * and callbacks for I/O ports, special writes and scanline events. Blocks
 * translated from a paged slot check the slot's bank on entry; blocks
 * translated from RAM check their bytes on entry and are translated again
 * when the code has changed.
 */

#include "types.h"

/* Address of a host object as seen by generated code, and back. Every
 * address stored in the context or in generated code goes through these,
 * so that the translator can also run inside a simulated address space. */
#ifndef JIT_ADDR
#define JIT_ADDR(p) ((uint32)(p))
#define JIT_PTR(a)  ((void *)(a))
#endif

/* Page kinds (ctx->page_kind). */
#define Z80J_PAGE_FIXED    0x00     /* content never changes */
#define Z80J_PAGE_SLOT(n)  (0x01 + (n)) /* content given by slot_bank[n], n < 4 */
#define Z80J_PAGE_RAM      0x08     /* writable: code is checked on entry */
#define Z80J_PAGE_KIND     0x0F
#define Z80J_PAGE_RFIXED   0x40     /* read mapping never changes */
#define Z80J_PAGE_WFIXED   0x80     /* write mapping never changes */

/* Exit reasons. */
#define Z80J_EXIT_LINES    0        /* every requested scanline ran */
#define Z80J_EXIT_ABORT    2        /* translation failed, exit_arg = Z80 PC */

/* Offsets from the global pointer (r10), used by the generated code. */
#define Z80J_PZST_OFF  0x0600
#define Z80J_FENC_OFF  0x0700
#define Z80J_FDEC_OFF  0x0800
#define Z80J_MRAM_OFF  0x1000
#define Z80J_TAB_OFF   0x4000

typedef struct z80j_machine z80j_machine;

/*
 * Context shared with the generated code and z80jit_glue.s: CONTRACT with
 * the equates of z80jit_glue.s and the offsets above - keep in sync. The
 * global pointer is &wtab[0]; the read table sits below it.
 */
typedef struct {
    uint32 rtab[256];       /* -0x400: reads, page p at rtab[255 - p]; */
                            /* entry = host address of the page - Z80 address */
    uint32 wtab[256];       /* +0x000: writes, page p at wtab[p], 0 = special */
    uint32 regs[8];         /* +0x400: F A BC DE HL cycles PC SP */
    uint32 ix;              /* +0x420: IX in bits 16-31 */
    uint32 iy;              /* +0x424: IY in bits 16-31 */
    uint32 a2;              /* +0x428: A' (bits 24-31) */
    uint32 f2;              /* +0x42C: F' (internal format) */
    uint32 bc2;             /* +0x430 */
    uint32 de2;             /* +0x434 */
    uint32 hl2;             /* +0x438 */
    uint32 i;               /* +0x43C: I register (0-255) */
    uint32 r;               /* +0x440: R register (0-255) */
    uint32 iff1;            /* +0x444 */
    uint32 iff2;            /* +0x448 */
    uint32 im;              /* +0x44C: interrupt mode 0-2 */
    uint32 halted;          /* +0x450 */
    uint32 irq_line;        /* +0x454: maskable interrupt line (machine) */
    uint32 nmi;             /* +0x458: non-maskable interrupt pending */
    uint32 run_end;         /* +0x45C: scanline at which the run ends */
    uint32 line;            /* +0x460: scanline at which the current stretch */
                            /* of execution ends (see z80j_machine) */
    uint32 event_line;      /* +0x464: next line calling machine->event */
    uint32 host_sl;         /* +0x468: caller's r10, restored around C calls */
    uint32 exit_reason;     /* +0x46C */
    uint32 exit_arg;        /* +0x470 */
    uint32 state;           /* +0x474: translator state */
    uint32 machine;         /* +0x478: z80j_machine */
    uint32 slot_bank[4];    /* +0x47C: bank of each paged slot */
    uint32 resume_host;     /* +0x48C: segment to resume at on the next run */
    uint32 resume_pc;       /* +0x490: Z80 address of that segment */
    uint32 scratch;         /* +0x494: temporary of the generated code */
    uint32 idle;            /* +0x498: T-states (times 256) spent in HALT and */
                            /* in busy-wait loops, counted modulo 2^32 */
    uint32 port_in;         /* +0x49C: 256 input handlers (see below) */
    uint32 port_out;        /* +0x4A0: 256 output handlers */
    uint32 port_outn;       /* +0x4A4: 256 handlers of OUTI / OUTD runs */
    uint32 mdata;           /* +0x4A8: data of the machine's handlers */
    uint32 pad0[21];
    uint8  page_kind[256];  /* +0x500 */
    uint8  pzst[256];       /* +0x600: sign / zero / parity flags of a byte */
    uint8  fenc[256];       /* +0x700: internal flags -> Z80 F */
    uint8  fdec[256];       /* +0x800: Z80 F -> internal flags */
    uint8  pad1[Z80J_MRAM_OFF - 0x900];
    uint8  mram[0x2000];    /* +0x1000: machine RAM near the global pointer */
    uint8  pad2[Z80J_TAB_OFF - Z80J_MRAM_OFF - 0x2000];
    uint32 lookup[0x10000]; /* +0x4000: code to run for each Z80 address */
} z80j_ctx;

/*
 * Emulated machine: I/O ports, special writes and scanline events.
 *
 * Time is counted in scanlines of 228 T-states. The Z80 runs in stretches
 * that end at the start of scanline ctx->line: the next event line or the
 * end of the run, whichever comes first. For ports, left is the number of
 * T-states (times 256) between the end of the instruction and the end of
 * the stretch, so the access happens at T-state
 *     ctx->line * 228 - left / 256
 * counted from the start of line 0 (see Z80J_TIME).
 *
 * The machine raises interrupt lines in event(), or in out() and write()
 * returning nonzero, which leaves the block and takes the interrupt; it
 * may move event_line in the same places. ctx->line may be rebased (all
 * line numbers shifted together) in event().
 */
#define Z80J_TIME(ctx, left) ((ctx)->line * 228u - ((left) >> 8))

/*
 * Port handlers. Every port has an input and an output handler, called by
 * the generated code (directly for IN A,(n) and OUT (n),A, through the
 * tables otherwise) with the Z80 registers in place:
 *   in:  r1 = port (0-255), r2 = T-states left (times 256); returns r0
 *   out: r0 = value (bits 0-7), r1 = port, r2 = T-states left; returns r0,
 *        nonzero to leave the block (see z80j_machine.out)
 *   run: r0 = count (negative for OUTD), r1 = port, r2 = T-states left at
 *        the end of the run, r7 = HL (bits 16-31); sends count bytes read
 *        from HL upwards (downwards for OUTD) and leaves HL unchanged
 * r10 is the global pointer; r3-r11 must be preserved, r0-r2, r12 and lr
 * may be lost. The tables start with the generic handlers of the glue,
 * which call the machine's C callbacks below; a machine may put its own
 * handlers in them (and its data in ctx->mdata) before translating code.
 */
#define Z80J_PORTS 256

/* Write callback results. */
#define Z80J_WRITE_STAY   0     /* nothing a block relies on changed */
#define Z80J_WRITE_LEAVE  1     /* leave the block */
#define Z80J_WRITE_PAGING 2     /* paged slot banks changed: leave only a */
                                /* block whose own code is no longer mapped */

struct z80j_machine {
    uint32 (*in)(z80j_machine *m, uint32 port, uint32 left);
    /* A nonzero return leaves the translated block (interrupt lines,
     * paging or anything a block relies on may have changed). */
    uint32 (*out)(z80j_machine *m, uint32 port, uint32 value, uint32 left);
    /* Returns Z80J_WRITE_*. */
    uint32 (*write)(z80j_machine *m, uint32 addr, uint32 value);
    /* Called at the start of scanline ctx->event_line (ctx->line is then
     * that line); sets the interrupt lines and the next event line, which
     * must be later than ctx->line to be seen. */
    void   (*event)(z80j_machine *m);
    void   *user;
};

/* Addresses of the routines of z80jit_glue.s used by generated code. */
typedef struct {
    uint32 link;            /* static exit: translate / link the target */
    uint32 miss;            /* untranslated or mismatching entry */
    uint32 seg_timeout;     /* segment does not fit in the scanline */
    uint32 leave;           /* leave the block, then dispatch */
    uint32 halt;            /* HALT */
    uint32 abort;           /* translation failed */
    uint32 io_in;           /* IN (C): dispatch through ctx->port_in */
    uint32 io_out;          /* OUT (C): dispatch through ctx->port_out */
    uint32 io_outn;         /* OUTI / OUTD run: dispatch through ctx->port_outn */
    uint32 write;           /* special write */
    uint32 verify;          /* RAM block check */
    uint32 daa;             /* DAA on r3 / r4 */
    uint32 push_slow;       /* push to a special page: r0 = value */
} z80j_glue;

typedef struct {
    uint32 translations;    /* blocks translated */
    uint32 insns;           /* Z80 instructions translated */
    uint32 code_bytes;      /* ARM code generated */
    uint32 fused;           /* flag tests kept in the ARM flags */
    uint32 full_flags;      /* flag computations kept */
    uint32 flushes;         /* code buffer flushes */
    uint32 retranslations;  /* RAM blocks whose code had changed */
    uint32 busy_loops;      /* busy-wait loops found in translated code */
    uint32 interrupts;      /* maskable interrupts taken */
    uint32 nmis;            /* non-maskable interrupts taken */
} z80j_stats;

typedef struct z80j_block z80j_block;

typedef struct {
    z80j_ctx   *ctx;
    uint32     *code;       /* code buffer */
    uint32     *code_end;
    uint32     *cur;        /* next free word */
    z80j_glue   glue;
    z80j_block *blocks;     /* block descriptors */
    uint32      max_blocks;
    uint32      nblocks;
    z80j_block *hash[1024]; /* blocks by Z80 address */
    uint32      generation; /* incremented by each flush */
    int32       error;      /* an instruction could not be encoded */
    z80j_stats  stats;
} z80j_state;

/* Memory for max_blocks block descriptors. */
uint32 z80j_block_bytes(uint32 max_blocks);

/* Prepares the translator and the context: lookup table, flag tables and
 * register reset. The pages, page kinds, slot banks and machine are set up
 * by the caller before running. */
void   z80j_init(z80j_state *j, z80j_ctx *ctx, uint32 *code, uint32 code_words,
                 void *block_mem, uint32 max_blocks, const z80j_glue *glue);

/* Z80 reset: PC 0, interrupts disabled, interrupt mode 0. */
void   z80j_reset(z80j_ctx *ctx);

/* Forgets every translation. */
void   z80j_flush(z80j_state *j);

/* Translates the block at pc if needed, and reports its static exits
 * (jump and call targets, fall-through) in targets. Returns the number of
 * Z80 instructions of the block, 0 when translation failed. */
uint32 z80j_prepare(z80j_state *j, uint32 pc, uint32 *targets, uint32 max_targets,
                    uint32 *ntargets);

/* Length and T-states (not taken, taken) of the instruction at pc. */
void   z80j_insn_info(const z80j_ctx *ctx, uint32 pc, uint32 *len, uint32 *t,
                      uint32 *t_taken);

/* Called by z80jit_glue.s. The first ones return the generated code to
 * jump to (0: the run is over); the registers are in the context for
 * entry, stretch end and leave. */
uint32 z80j_translate(z80j_state *j, uint32 pc);
uint32 z80j_retranslate(z80j_state *j, uint32 pc);
uint32 z80j_link(z80j_state *j, uint32 data);
uint32 z80j_entry(z80j_state *j, int32 lines);
uint32 z80j_stretch_end(z80j_state *j);
uint32 z80j_leave(z80j_state *j);
uint32 z80j_io_write(z80j_state *j, uint32 addr, uint32 value);
void   z80j_push_slow(z80j_state *j, uint32 sp, uint32 value);

/* Implemented in z80jit_glue.s. */
void   z80j_run(z80j_ctx *ctx, int32 lines);
void   z80j_glue_link(void);
void   z80j_glue_miss(void);
void   z80j_glue_seg_timeout(void);
void   z80j_glue_leave(void);
void   z80j_glue_halt(void);
void   z80j_glue_abort(void);
void   z80j_glue_io_in(void);
void   z80j_glue_io_out(void);
void   z80j_glue_io_outn(void);

/* Generic port handlers (machine C callbacks), for the port tables. */
void   z80j_port_in_c(void);
void   z80j_port_out_c(void);
void   z80j_port_outn_loop(void);
void   z80j_glue_write(void);
void   z80j_glue_verify(void);
void   z80j_glue_daa(void);
void   z80j_glue_push_slow(void);

/* Fills a glue table with the routines of z80jit_glue.s. */
void   z80j_default_glue(z80j_glue *glue);

/* Stores value in count words, count a multiple of 4 (z80jit_glue.s). */
void   z80j_fill_words(uint32 *dst, uint32 count, uint32 value);

#endif /* Z80JIT_H */
