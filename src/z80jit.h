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
#define Z80J_MARK_CHUNKS 256   /* run marks of the code buffer (z80j_state) */
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
    uint32 f2;              /* +0x428: F' (internal format); F', A' and */
    uint32 a2;              /* +0x42C: A' (bits 24-31), BC', DE', HL' are */
    uint32 bc2;             /* +0x430   consecutive for EX AF,AF' and EXX */
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
    uint32 write_a;         /* +0x4AC: special write handler (see below) */
    uint32 wdata;           /* +0x4B0: data of that handler */
    uint32 hot_tab;         /* +0x4B4: interpreter: entry counts per address, or 0 */
    uint32 hot_queue_at;    /* +0x4B8: counts at which the interpreter stops */
    uint32 hot_sync_at;     /* +0x4BC */
    uint32 ei_saved;        /* +0x4C0: interpreter: T-state counter saved by EI */
    uint32 int_leave;       /* +0x4C4: interpreter: a write asked to leave */
    uint32 int_runs;        /* +0x4C8: interpreter: stretches of interpretation */
    uint32 int_insns;       /* +0x4CC: interpreter: instructions interpreted */
    uint32 hot_sync_gate;   /* +0x4D0: interpreter: int_insns from which an address */
                            /* past hot_sync_at is handed to the C side again */
    uint32 hot_force_at;    /* +0x4D4: count at which it is handed over regardless */
    uint32 resume_tab;      /* +0x4D8: interrupted segments (z80j_resume[Z80J_RESUMES]) */
    uint32 event_a;         /* +0x4DC: machine event handler in assembly, or 0 */
    uint32 irq_count;       /* +0x4E0: maskable interrupts taken by the glue */
    uint32 resumed_count;   /* +0x4E4: returns resumed by the glue */
    uint32 pad0[6];
    uint8  page_kind[256];  /* +0x500 */
    uint8  pzst[256];       /* +0x600: sign / zero / parity flags of a byte */
    uint8  fenc[256];       /* +0x700: internal flags -> Z80 F */
    uint8  fdec[256];       /* +0x800: Z80 F -> internal flags */
    uint32 chunk_mark[Z80J_MARK_CHUNKS];  /* +0x900: set by the code of each chunk */
                            /* of the code buffer when it runs (see z80j_state) */
    uint8  pad1[Z80J_MRAM_OFF - 0x900 - 4 * Z80J_MARK_CHUNKS];
    uint8  mram[0x2000];    /* +0x1000: machine RAM near the global pointer */
    uint8  pad2[Z80J_TAB_OFF - Z80J_MRAM_OFF - 0x2000];
    uint32 lookup[0x10000]; /* +0x4000: code to run for each Z80 address */
} z80j_ctx;

/*
 * Emulated machine: I/O ports, special writes and scanline events. The
 * machine may also give the context an event handler in assembly
 * (ctx->event_a, called by the glue at a stretch end with r10 = global
 * pointer, r0-r2 and r12 free, returning r0 = 0 when it handled the
 * event and nonzero, with nothing changed, when the C callback must).
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

/*
 * Special write handler (ctx->write_a): entered from the generated code
 * for a write to a page whose write table entry is zero, with r0 = value
 * (bits 0-7 significant), r12 = Z80 address (bits 16-31), r2 = where to
 * return when the block goes on, lr -> the three data words of the write
 * stub (Z80 address of the next instruction or -1, T-states to give back
 * times 256, key of the block). r3-r11 must be preserved, r0-r1, r12 and
 * lr may be lost. The default handler, z80j_write_generic, calls the
 * machine's C write callback; a machine may put a faster handler of its
 * own there (with its data in ctx->wdata) and fall back to the generic
 * one, or end with the result code in r0 at z80j_write_result.
 */

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
    uint32 interp;          /* interpreter entry (registers in the context) */
} z80j_glue;

typedef struct {
    uint32 translations;    /* blocks translated */
    uint32 insns;           /* Z80 instructions translated */
    uint32 code_bytes;      /* ARM code generated (cumulative) */
    uint32 fused;           /* flag tests kept in the ARM flags */
    uint32 full_flags;      /* flag computations kept */
    uint32 flushes;         /* code buffer flushes (every zone emptied) */
    uint32 evictions;       /* zones emptied to make room */
    uint32 evicted;         /* blocks lost to evictions */
    uint32 evicted_live;    /* chunks of the evicted zones run in the last frames */
    uint32 unlinked;        /* direct branches undone by evictions */
    uint32 link_full;       /* links not made for lack of room in the log */
    uint32 retranslations;  /* RAM blocks whose code had changed */
    uint32 busy_loops;      /* busy-wait loops found in translated code */
    uint32 scan_hits;       /* flag scans answered by the cache */
    uint32 interrupts;      /* maskable interrupts taken */
    uint32 nmis;            /* non-maskable interrupts taken */
    uint32 prefetched;      /* blocks translated ahead, in spare time */
    uint32 promoted;        /* blocks translated again after an eviction */
    uint32 sync;            /* blocks translated at once, hot without spare time */
    uint32 sync_us;         /* time they took (microseconds, cumulative) */
    uint32 sync_refused;    /* translations at once refused for lack of budget */
    uint32 queue_full;      /* hot addresses dropped for lack of room */
    uint32 resumed;         /* interrupt returns resumed inside their block */
    uint32 dyn_blocks;      /* RAM blocks translated with dynamic targets */
    uint32 seams;           /* RAM blocks cut, or refused, at a seam of the host memory */
} z80j_stats;

/* An interrupted segment: where its block resumes, for the return. */
typedef struct {
    uint32 pc;              /* Z80 address of the segment */
    uint32 host;            /* its code (0: empty) */
    uint32 key;             /* key of the block */
    uint32 pad;             /* 16-byte entries for the glue */
} z80j_resume;

#define Z80J_RESUMES 64     /* direct-mapped by address */
#define Z80J_RESUME_SLOT(pc) (((pc) ^ ((pc) >> 6)) & (Z80J_RESUMES - 1))

typedef struct z80j_block z80j_block;

#define Z80J_QUEUE 256          /* hot addresses waiting to be translated */

#define JIT_MAX_ZONES   64
#define JIT_SCAN_CACHE  512

/* An area: the zone it fills and the zones it owns. */
typedef struct {
    uint32     *cur;        /* next free word */
    uint32     *end;        /* end of the zone being filled */
    uint32      zone;       /* zone being filled */
    uint32      count;      /* zones owned (0: the area does not exist) */
} z80j_area;

/*
 * Translator state. The code buffer is split into zones; when a zone to
 * be filled still holds code, its blocks are evicted: their entries in
 * the lookup table are reset, and the direct branches leading into the
 * zone from surviving blocks, recorded in the link log when they were
 * made, become calls to the linker again. With a single zone an eviction
 * is a flush of the whole buffer.
 *
 * Two areas fill zones of their own: a block translated for the first
 * time goes to the cold area, a block translated again after being
 * evicted (a bitmap remembers the addresses translated before) has
 * proven useful and goes to the hot area, so that code that runs once
 * and code that runs all the time do not share zones. The zones are not
 * split between the areas in advance: when an area needs a zone it takes
 * the one whose code has run the least recently, whichever area owned
 * it, so that each area is as large as its live code. What has run
 * recently is known from the generated code itself: the code buffer is
 * made of chunks (2 KiB in the usual layout) and every block starts by
 * storing a mark for its chunk in the context (one store per block
 * entry); each frame the marks of one zone are turned into ages. A zone
 * is empty, or dead, or has as many live chunks as the eviction would
 * lose, and the least live one goes first. The code reachable from the
 * entry points, translated before the run, goes to the cold area (or to
 * the hot one with seed_hot).
 *
 * With an interpreter (z80j_set_interp), code is interpreted the first
 * times it runs: a table counts the entries of each Z80 address, an
 * address reaching queue_at is queued for translation in spare time
 * (z80j_prefetch, the hottest first) and one reaching sync_at is
 * translated at once, within a time budget per frame (z80j_frame_start):
 * a floor the caller sets, raised by the estimated cost of the
 * instructions the frame has already interpreted, so that a frame that
 * spends its time interpreting a hot working set translates it rather
 * than paying the interpretation again in the next frame. Code that runs
 * once never enters the buffer, and translation mostly leaves the frames
 * of the game for their spare time.
 */
typedef struct {
    z80j_ctx   *ctx;
    uint32     *code;       /* code buffer */
    uint32     *code_end;
    uint32     *cur;        /* next free word, in the area being filled */
    uint32     *zone_end;   /* end of that zone */
    uint32      zone_words;
    uint32      nzones;
    z80j_area   area[2];    /* 0: cold, 1: hot (count 0: no hot area) */
    uint32      seen[2048]; /* Z80 addresses translated before */
    uint32      flaky[2048];/* RAM addresses whose code changed once already */
    uint32      zone_blocks[JIT_MAX_ZONES]; /* blocks held by each zone */
    uint32      zone_fill[JIT_MAX_ZONES];   /* frame at which each zone was last taken */
    uint8       zone_area[JIT_MAX_ZONES];   /* area owning each zone */
    uint8       chunk_age[Z80J_MARK_CHUNKS];/* ageing rounds since each chunk of the */
                                            /* code buffer ran (255: never) */
    uint32      chunk_shift;                /* log2 of the words per chunk */
    uint32      frame_no;                   /* frames begun (z80j_frame_start) */
    /* Flags read by the code at an address (the forward scan of the
     * liveness analysis), cached across blocks for ROM code: the scan
     * result only depends on the bytes at the address and on the key of
     * the block the scan is made for. */
    uint32      scan_key[JIT_SCAN_CACHE];   /* pc | bank of the block key << 16 */
    uint8       scan_kind[JIT_SCAN_CACHE];  /* page kind of the block key, 0xFF: empty */
    uint8       scan_val[JIT_SCAN_CACHE];
    z80j_glue   glue;
    z80j_block *blocks;     /* block descriptors */
    z80j_block *free_blocks;/* descriptors not in use, chained by next */
    uint32      max_blocks;
    uint32      nblocks;    /* descriptors in use */
    z80j_block *hash[1024]; /* blocks by Z80 address */
    uint32     *links;      /* link log: data words of each resolved link */
                            /* stub; bit 0 set: a retranslation trampoline */
    uint32      nlinks;
    uint32      max_links;
    uint32      generation; /* incremented by each flush or eviction */
    int32       error;      /* an instruction could not be encoded */
    uint32      seed_hot;   /* translate straight into the hot area (code */
                            /* reachable from the entry points, before the run) */
    uint8      *hot;        /* entry counts per Z80 address (0: no interpreter) */
    uint32      queue_at;
    uint32      sync_at;
    uint32      force_at;   /* entries from which translation ignores the budget */
    uint32    (*clock)(void);   /* microsecond clock, for the budget */
    uint32      sync_floor_us;  /* budget of translations at once per frame */
    uint32      int_cost;       /* estimated cost of an interpreted instruction, */
                                /* in 1/16 microsecond */
    uint32      sync_spent_us;  /* spent in this frame */
    uint32      int_insns0;     /* interpreted instructions when the frame began */
    uint32      frame_start_us; /* clock at the start of the frame */
    uint32      frame_us;       /* length of a frame */
    uint32      reserve_us;     /* time the frame still needs after the emulation */
    uint32      est_us;         /* running average of the cost of a translation */
    uint32      queue[Z80J_QUEUE];  /* unordered; the hottest goes first */
    uint32      nqueue;
    uint32      queued[2048];   /* bitmap of the addresses in the queue */
    /* Segments interrupted at a stretch end: the interrupt's return
     * lands inside a translated block, at a segment check, which is
     * entered directly rather than translated as a block of its own
     * (games taking a line interrupt every few lines would otherwise
     * make a block of every return address). An entry goes with the
     * eviction of the zone holding its code. */
    z80j_resume resume[Z80J_RESUMES];
    z80j_stats  stats;
} z80j_state;

/* Memory for max_blocks block descriptors, and for a link log of
 * max_links entries. */
uint32 z80j_block_bytes(uint32 max_blocks);
uint32 z80j_link_bytes(uint32 max_links);

/* Prepares the translator and the context: lookup table, flag tables and
 * register reset. The code buffer of code_words words is split into
 * nzones zones (at least 1); with nhot nonzero (and nzones at least 3)
 * the blocks translated again after an eviction get an area of their
 * own. The pages, page kinds, slot banks and machine are set up by the
 * caller before running. */
void   z80j_init(z80j_state *j, z80j_ctx *ctx, uint32 *code, uint32 code_words,
                 uint32 nzones, uint32 nhot, void *block_mem, uint32 max_blocks,
                 uint32 *link_mem, uint32 max_links, const z80j_glue *glue);

/* Z80 reset: PC 0, interrupts disabled, interrupt mode 0. */
void   z80j_reset(z80j_ctx *ctx);

/* Forgets every translation. */
void   z80j_flush(z80j_state *j);

/* Enables the interpreter: hot is a table of 65536 entry counts, cleared
 * here; an address is queued for translation when its count reaches
 * queue_at and translated at once when it reaches sync_at. */
void   z80j_set_interp(z80j_state *j, uint8 *hot, uint32 queue_at, uint32 sync_at);

/* Entries from which an address is translated at once whatever the
 * budget (0: never): code interpreted that often is either a loop or
 * code that keeps missing the budget. */
void   z80j_set_force(z80j_state *j, uint32 force_at);

/* Budget of the translations at once, measured with clock: translations
 * paid for by the instructions the frame has interpreted (int_cost per
 * instruction, in 1/16 us) are always allowed; beyond them, up to
 * floor_us per frame while the frame has room for one (the frame began
 * at start_us, lasts frame_us and still needs reserve_us after the
 * emulation). z80j_frame_start opens a new frame's budget. */
void   z80j_set_budget(z80j_state *j, uint32 (*clock)(void), uint32 floor_us,
                       uint32 int_cost, uint32 frame_us);
void   z80j_frame_start(z80j_state *j, uint32 start_us, uint32 reserve_us);

/* Whether a translation in spare time fits in the frame, elapsed_us
 * after its start, leaving margin_us: uses the running average of the
 * cost of a translation, with half as much again for safety. */
uint32 z80j_spare_fits(const z80j_state *j, uint32 elapsed_us, uint32 margin_us);

/* Translates one queued hot address; returns 0 when the queue is empty.
 * Meant for the spare time at the end of a frame. */
uint32 z80j_prefetch(z80j_state *j);

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
/* These two return 0 when the code at the address must be interpreted
 * instead (the address is then in regs[6]). */
uint32 z80j_retranslate(z80j_state *j, uint32 pc);
uint32 z80j_link(z80j_state *j, uint32 data);
uint32 z80j_entry(z80j_state *j, int32 lines);
uint32 z80j_stretch_end(z80j_state *j);
uint32 z80j_leave(z80j_state *j);
uint32 z80j_io_write(z80j_state *j, uint32 addr, uint32 value);
void   z80j_push_slow(z80j_state *j, uint32 sp, uint32 value);
/* The interpreter reached an address whose entry count hit a threshold
 * (regs[6]): translated now, or queued and interpreted on. */
uint32 z80j_hot(z80j_state *j);

/* Implemented in z80jit_glue.s and z80int_a.s (the interpreter). */
void   z80j_run(z80j_ctx *ctx, int32 lines);
void   z80i_loop(void);
void   z80j_glue_link(void);
void   z80j_glue_miss(void);
void   z80j_glue_seg_timeout(void);
void   z80j_glue_leave(void);
void   z80j_glue_halt(void);
void   z80j_glue_abort(void);
void   z80j_glue_io_in(void);
void   z80j_glue_io_out(void);
void   z80j_glue_io_outn(void);
void   z80j_glue_interp(void);

/* Generic port handlers (machine C callbacks), for the port tables. */
void   z80j_port_in_c(void);
void   z80j_port_out_c(void);
void   z80j_port_outn_loop(void);
void   z80j_glue_write(void);
void   z80j_write_generic(void);
void   z80j_write_result(void);
void   z80j_glue_verify(void);
void   z80j_glue_daa(void);
void   z80j_glue_push_slow(void);

/* Fills a glue table with the routines of z80jit_glue.s. */
void   z80j_default_glue(z80j_glue *glue);

/* Stores value in count words, count a multiple of 4 (z80jit_glue.s). */
void   z80j_fill_words(uint32 *dst, uint32 count, uint32 value);

#endif /* Z80JIT_H */
