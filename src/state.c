/*
 * One savestate in the console's NVRAM. See state.h.
 */

#include "state.h"
#include "platform.h"
#include "render.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#ifndef STATE_NO_FILES
#include "io.h"
#include "filesystem.h"
#include "filefunctions.h"
#endif

#define STATE_PATH    "/NVRAM/masterdo.state"
#define STATE_MAGIC   0x4D445353u   /* "MDSS" */
#define STATE_VERSION 1u
#define STATE_ROOM    (24 * 1024)   /* bytes of the file at most */
#define STATE_SLACK   1024          /* past them: a write rounded up to whole blocks */
#define DICT_BANKS    4

/* Words of the file's head. */
enum {
    F_MAGIC, F_VERSION, F_IMAGE, F_ID, F_NBANKS, F_BANKS, F_DICT, F_DICT_SUM, F_RAW,
    F_RAW_SUM, F_PACKED, F_FRAME, F_CRAM, F_CART, F_WORDS
};

/* Words of the state's head (st_head), the paging registers from
 * W_PAGING. */
#define HEAD_WORDS  60
#define W_PAGING    38
#define CRAM_BYTES  32              /* the Master System's; the Game Gear's after them */

/* LD A,R reads ((R + 9 * line + cycles / 1024) & 0x7F) | (R & 0x80),
 * line being the emulator's count of lines since power-on (the code
 * generated and the interpreter do not count R per instruction): the
 * state keeps the value the game would see at the frame boundary, the
 * load gives R the value that shows the same on the line it is loaded
 * at. */
#define R_SEEN(r, line) ((((r) + 9 * (line)) & 0x7Fu) | ((r) & 0x80u))
#define R_KEPT(v, line) ((((v) - 9 * (line)) & 0x7Fu) | ((v) & 0x80u))

#ifdef STATE_NO_FILES
static uint32 file_mem[(STATE_ROOM + STATE_SLACK) / 4];
static uint32 file_len;
#endif

const char *state_why(int32 r)
{
    switch (r) {
    case STATE_NONE:  return "no state in the NVRAM";
    case STATE_OTHER: return "the NVRAM holds the state of another cartridge image";
    case STATE_BAD:   return "damaged state";
    case STATE_NOMEM: return "not enough memory";
    case STATE_FULL:  return "NVRAM full";
    case STATE_LARGE: return "state too large";
    default:          return "system error";
    }
}

/* A hash of n bytes at p (word aligned): whole runs of eight words by
 * the translator's routine, which reads eight at a time, then bytes. */
static uint32 st_sum(const uint8 *p, uint32 n)
{
    uint32 k = n & ~31u;
    uint32 h = (k != 0) ? z80j_hash_words(n, (const uint32 *)p, k / 4) : n;

    for (; k < n; k++)
        h = h * 31 + p[k];
    return h;
}

/*------------------------------------------------------------- the state */

/* The head of the state: every register and the timing of the frame
 * boundary, a word each (the packing makes the zeros cheap). */
static void st_head(const game *g, uint32 *w)
{
    const z80j_ctx *c = g->ctx;
    const sms_machine *s = g->sms;
    const vdp_state *v = &s->vdp;
    uint32 n = 0;
    uint32 k;

    w[n++] = s->gg;
    for (k = 0; k < 8; k++)
        w[n++] = c->regs[k];
    w[n++] = c->ix;
    w[n++] = c->iy;
    w[n++] = c->f2;
    w[n++] = c->a2;
    w[n++] = c->bc2;
    w[n++] = c->de2;
    w[n++] = c->hl2;
    w[n++] = c->i;
    w[n++] = R_SEEN(c->r, c->line);
    w[n++] = c->iff1;
    w[n++] = c->iff2;
    w[n++] = c->im;
    w[n++] = c->halted;
    w[n++] = c->irq_line;
    w[n++] = c->nmi;
    w[n++] = c->line - v->frame_base;
    w[n++] = (c->event_line == 0xFFFFFFFFu) ? 0xFFFFFFFFu : c->event_line - v->frame_base;
    w[n++] = v->acc;
    w[n++] = v->buffer;
    w[n++] = v->status;
    w[n++] = v->line_flag;
    w[n++] = v->gg_latch;
    w[n++] = v->vbl_done;
    w[n++] = v->lc_k;
    w[n++] = v->lc_val;
    for (k = 0; k < 16; k += 4)
        w[n++] = v->reg[k] | (uint32)v->reg[k + 1] << 8 | (uint32)v->reg[k + 2] << 16 |
                 (uint32)v->reg[k + 3] << 24;
    for (k = 0; k < 4; k++)
        w[n++] = s->mem.reg[k];
    w[n++] = s->psg_latch;
    for (k = 0; k < 8; k++)
        w[n++] = s->psg_reg[k];
    w[n++] = s->mem_ctrl;
    w[n++] = s->io_ctrl;
    for (k = 0; k < 7; k++)
        w[n++] = s->gg_port[k];
}

/* The state into s (word aligned, room for it); returns its bytes,
 * with the sizes of its colour and cartridge RAMs. */
static uint32 st_capture(const game *g, uint8 *s, uint32 *cram, uint32 *cart)
{
    const sms_machine *m = g->sms;
    const vdp_state *v = &m->vdp;
    uint32 o = HEAD_WORDS * 4;

    st_head(g, (uint32 *)s);
    memcpy(s + o, g->ctx->mram, 0x2000);
    o += 0x2000;
    memcpy(s + o, v->vram, VDP_VRAM_SIZE);
    o += VDP_VRAM_SIZE;
    memcpy(s + o, v->cram, CRAM_BYTES);
    o += CRAM_BYTES;
    *cram = CRAM_BYTES;
    if (m->gg) {
        memcpy(s + o, v->gg_cram, VDP_GG_CRAM);
        o += VDP_GG_CRAM;
        *cram += VDP_GG_CRAM;
    }
    *cart = (m->mem.cart_ram != 0) ? m->mem.cart_ram_size : 0;
    memcpy(s + o, m->mem.cart_ram, *cart);
    return o + *cart;
}

/* The machine put back from the state s (cart: its cartridge RAM's
 * bytes). */
static void st_apply(game *g, const uint8 *s, uint32 cart)
{
    z80j_ctx *c = g->ctx;
    sms_machine *m = g->sms;
    vdp_state *v = &m->vdp;
    const uint32 *w = (const uint32 *)s;
    uint32 o = HEAD_WORDS * 4;
    uint32 n = 1;
    uint32 line_rel;
    uint32 event_rel;
    uint32 k;
    render_stats keep;

    for (k = 0; k < 8; k++)
        c->regs[k] = w[n++];
    c->ix = w[n++];
    c->iy = w[n++];
    c->f2 = w[n++];
    c->a2 = w[n++];
    c->bc2 = w[n++];
    c->de2 = w[n++];
    c->hl2 = w[n++];
    c->i = w[n++];
    c->r = R_KEPT(w[n], c->line);
    n++;
    c->iff1 = w[n++];
    c->iff2 = w[n++];
    c->im = w[n++];
    c->halted = w[n++];
    c->irq_line = w[n++];
    c->nmi = w[n++];
    line_rel = w[n++];
    event_rel = w[n++];
    v->acc = w[n++];
    v->buffer = w[n++];
    v->status = w[n++];
    v->line_flag = w[n++];
    v->gg_latch = (uint8)w[n++];
    v->vbl_done = w[n++];
    v->lc_k = w[n++];
    v->lc_val = w[n++];
    for (k = 0; k < 16; k += 4, n++) {
        v->reg[k] = (uint8)w[n];
        v->reg[k + 1] = (uint8)(w[n] >> 8);
        v->reg[k + 2] = (uint8)(w[n] >> 16);
        v->reg[k + 3] = (uint8)(w[n] >> 24);
    }
    n += 4;                             /* the paging registers (W_PAGING), below */
    m->psg_latch = w[n++];
    for (k = 0; k < 8; k++)
        m->psg_reg[k] = w[n++];
    m->mem_ctrl = w[n++];
    m->io_ctrl = w[n++];
    for (k = 0; k < 7; k++)
        m->gg_port[k] = w[n++];

    /* The paging, which gives the cartridge RAM when the state holds it
     * and the run has not been given it yet, then the memories. */
    sms_mem_restore(&m->mem, w + W_PAGING, cart != 0);
    memcpy(c->mram, s + o, 0x2000);
    o += 0x2000;
    memcpy(v->vram, s + o, VDP_VRAM_SIZE);
    o += VDP_VRAM_SIZE;
    memcpy(v->cram, s + o, CRAM_BYTES);
    o += CRAM_BYTES;
    if (m->gg) {
        memcpy(v->gg_cram, s + o, VDP_GG_CRAM);
        o += VDP_GG_CRAM;
    }
    if (cart != 0 && m->mem.cart_ram != 0) {
        uint32 had = m->mem.cart_ram_size;

        memcpy(m->mem.cart_ram, s + o, cart < had ? cart : had);
        if (had > cart)
            memset(m->mem.cart_ram + cart, 0, had - cart);
    }
    vdp_restored(v, line_rel, event_rel);

    /* The segments remembered at an interrupt resume a machine that is
     * gone. */
    c->resume_host = 0;
    z80j_forget_source(c, 0, 0xFFFFFFFFu);

    /* The picture is drawn whole from the new video RAM (the renderer's
     * counters run on, for the log's windows). */
    keep = g->rd->st;
    render_reset(g->rd);
    render_set_window(g->rd, v);
    g->rd->st = keep;
}

/*-------------------------------------------------------------- the file */

#ifndef STATE_NO_FILES
/* One command on the file. */
static Err st_io(Item req, uint32 cmd, int32 offset, void *send, void *recv, int32 len)
{
    IOInfo io;

    memset(&io, 0, sizeof(io));
    io.ioi_Command = (uint8)cmd;
    io.ioi_Offset = offset;
    io.ioi_Send.iob_Buffer = send;
    io.ioi_Send.iob_Len = (send != NULL) ? len : 0;
    io.ioi_Recv.iob_Buffer = recv;
    io.ioi_Recv.iob_Len = (recv != NULL) ? len : 0;
    return DoIO(req, &io);
}
#endif

/* Writes the n bytes of buf (cap bytes long) to the file, created when
 * absent; the blocks it lacks are asked for first, so that a full NVRAM
 * leaves the state it holds as it was. Returns n or an error. */
static int32 st_write(uint8 *buf, uint32 n, uint32 cap)
{
#ifdef STATE_NO_FILES
    (void)cap;
    memcpy(file_mem, buf, n);
    file_len = n;
    return (int32)n;
#else
    Item f = OpenDiskFile(STATE_PATH);
    Item req;
    FileStatus st;
    uint32 made = 0;
    uint32 need = 0;
    Err r;

    if (f < 0) {
        made = 1;
        r = CreateFile(STATE_PATH);
        if (r < 0)
            return r;
        f = OpenDiskFile(STATE_PATH);
        if (f < 0) {
            DeleteFile(STATE_PATH);
            return f;
        }
    }
    r = req = CreateIOReq(NULL, 0, f, 0);
    if (req >= 0) {
        r = st_io(req, CMD_STATUS, 0, NULL, &st, sizeof(st));
        if (r >= 0) {
            uint32 bs = st.fs.ds_DeviceBlockSize;

            need = (n + bs - 1) / bs;
            if (need > st.fs.ds_DeviceBlockCount &&
                st_io(req, FILECMD_ALLOCBLOCKS, (int32)(need - st.fs.ds_DeviceBlockCount),
                        NULL, NULL, 0) < 0)
                r = STATE_FULL;
            need *= bs;
            if (r >= 0 && need > cap)
                r = STATE_NOMEM;
        }
        if (r >= 0)
            r = st_io(req, FILECMD_SETEOF, (int32)n, NULL, NULL, 0);
        if (r >= 0) {
            memset(buf + n, 0, need - n);
            r = st_io(req, CMD_WRITE, 0, buf, NULL, (int32)need);
        }
        DeleteIOReq(req);
    }
    CloseDiskFile(f);
    if (r < 0 && made)
        DeleteFile(STATE_PATH);
    return (r < 0) ? r : (int32)n;
#endif
}

/* Reads the file into buf (cap bytes); returns its bytes, STATE_NONE
 * without a file, or an error. */
static int32 st_read(uint8 *buf, uint32 cap)
{
#ifdef STATE_NO_FILES
    if (file_len == 0)
        return STATE_NONE;
    if (file_len > cap)
        return STATE_BAD;
    memcpy(buf, file_mem, file_len);
    return (int32)file_len;
#else
    Item f = OpenDiskFile(STATE_PATH);
    Item req;
    FileStatus st;
    uint32 bytes = 0;
    Err r;

    if (f < 0)
        return STATE_NONE;
    r = req = CreateIOReq(NULL, 0, f, 0);
    if (req >= 0) {
        r = st_io(req, CMD_STATUS, 0, NULL, &st, sizeof(st));
        if (r >= 0) {
            uint32 bs = st.fs.ds_DeviceBlockSize;
            uint32 need;

            bytes = st.fs_ByteCount;
            need = (bytes + bs - 1) / bs * bs;
            if (bytes < F_WORDS * 4 || need > cap)
                r = STATE_BAD;
            else
                r = st_io(req, CMD_READ, 0, NULL, buf, (int32)need);
        }
        DeleteIOReq(req);
    }
    CloseDiskFile(f);
    return (r < 0) ? r : (int32)bytes;
#endif
}

/*-------------------------------------------------------- save and load */

/* The dictionary of the packing: the image when it is one block, else
 * the banks of nb (copied into memory taken from m). Returns its address
 * (NULL: no memory or a bank not read) and its bytes in *n. */
static const uint8 *st_dict(const game *g, stp_mem *m, uint32 nb, uint32 banks, uint32 *n)
{
    const sms_mem *s = &g->sms->mem;
    uint8 *d;
    uint32 k;

    if (nb == 0) {
        *n = g->rom_size;
        return s->rom;
    }
    *n = nb * 0x4000u;
    d = (uint8 *)stp_take(m, *n);
    if (d == NULL)
        return NULL;
    for (k = 0; k < nb; k++) {
        if (sms_mem_bank_copy(s, (banks >> (8 * k)) & 0xFF, d + k * 0x4000u) != 0)
            return NULL;
    }
    return d;
}

/* The image's identity: its first bank (or what there is of it). */
static uint32 st_image(const game *g)
{
    return st_sum(g->sms->mem.rom, g->rom_size < 0x4000u ? g->rom_size : 0x4000u);
}

static void st_bounds(stp_in *in, uint32 cram, uint32 cart)
{
    in->rb[0] = 0;
    in->rb[1] = HEAD_WORDS * 4;
    in->rb[2] = in->rb[1] + 0x2000;
    in->rb[3] = in->rb[2] + VDP_VRAM_SIZE;
    in->rb[4] = in->rb[3] + cram;
    in->rb[5] = in->rb[4] + cart;
    in->n = in->rb[5];
}

/* The log record (code_us: packing or unpacking, file_us: the NVRAM)
 * and, when it worked, the pad log entry. */
static void st_note(game *g, uint32 *w, uint32 load, int32 r, uint32 code_us, uint32 file_us)
{
    w[0] = g->frame;
    w[1] = load;
    w[2] = (uint32)(r < 0 ? r : 0);
    w[5] = code_us;
    w[6] = file_us;
    game_note_state(g, w);
    if (r >= 0)
        game_pad_event(g, load == 0 ? GAME_PAD_SAVE :
                          g->state_saved ? GAME_PAD_LOAD : GAME_PAD_LOAD | GAME_PAD_SAVE);
}

int32 state_save(game *g, stp_mem *m)
{
    uint32 w[GAME_STATE_WORDS];
    uint32 t0 = plat_usec_now();
    uint32 t1;
    uint32 cram;
    uint32 cart;
    uint32 stride = 0;
    uint32 nb = 0;
    uint32 banks = 0;
    const sms_machine *ms = g->sms;
    uint8 *s = (uint8 *)stp_take(m, HEAD_WORDS * 4 + 0x2000 + VDP_VRAM_SIZE + CRAM_BYTES +
                                    (ms->gg ? VDP_GG_CRAM : 0) +
                                    (ms->mem.cart_ram != 0 ? ms->mem.cart_ram_size : 0));
    uint32 *f = (uint32 *)stp_take(m, STATE_ROOM + STATE_SLACK);
    stp_in in;
    int32 r = STATE_NOMEM;

    memset(w, 0, sizeof(w));
    if (g->sms->mem.bank_tab != 0) {
        /* Bank 0 and the banks the slots show, each once. */
        uint32 b[DICT_BANKS];
        uint32 k;
        uint32 j;

        b[0] = 0;
        b[1] = g->ctx->slot_bank[0];
        b[2] = g->ctx->slot_bank[1];
        b[3] = g->ctx->slot_bank[2];
        for (k = 0; k < DICT_BANKS; k++) {
            for (j = 0; j < nb && ((banks >> (8 * j)) & 0xFF) != b[k]; j++)
                ;
            if (j == nb && b[k] < g->sms->mem.banks)
                banks |= b[k] << (8 * nb++);
        }
    }
    if (s != NULL && f != NULL) {
        in.s = s;
        in.n = st_capture(g, s, &cram, &cart);
        st_bounds(&in, cram, cart);
        in.dict = st_dict(g, m, nb, banks, &in.dict_n);
        if (in.dict != NULL) {
            r = stp_pack(&in, (uint8 *)(f + F_WORDS), STATE_ROOM - F_WORDS * 4, m, &stride);
            if (r == STP_LARGE)
                r = STATE_LARGE;
            else if (r < 0)
                r = STATE_NOMEM;
        }
    }
    if (r >= 0) {
        f[F_MAGIC] = STATE_MAGIC;
        f[F_VERSION] = STATE_VERSION;
        f[F_IMAGE] = g->rom_size;
        f[F_ID] = st_image(g);
        f[F_NBANKS] = nb;
        f[F_BANKS] = banks;
        f[F_DICT] = in.dict_n;
        f[F_DICT_SUM] = st_sum(in.dict, in.dict_n);
        f[F_RAW] = in.n;
        f[F_RAW_SUM] = st_sum(s, in.n);
        f[F_PACKED] = (uint32)r;
        f[F_FRAME] = g->frame;
        f[F_CRAM] = cram;
        f[F_CART] = cart;
        w[3] = in.n;
        w[4] = F_WORDS * 4 + (uint32)r;
        w[8] = stride;
    }
    t1 = plat_usec_now();
    if (r >= 0)
        r = st_write((uint8 *)f, F_WORDS * 4 + (uint32)r, STATE_ROOM + STATE_SLACK);
    if (r >= 0)
        g->state_saved = 1;
    st_note(g, w, 0, r, t1 - t0, plat_usec_now() - t1);
    return r;
}

int32 state_load(game *g, stp_mem *m)
{
    uint32 w[GAME_STATE_WORDS];
    uint32 t0 = plat_usec_now();
    uint32 t1;
    uint32 *f = (uint32 *)stp_take(m, STATE_ROOM + STATE_SLACK);
    uint8 *s = NULL;
    stp_in in;
    int32 r = STATE_NOMEM;
    int32 got = 0;

    memset(w, 0, sizeof(w));
    if (f != NULL)
        r = got = st_read((uint8 *)f, STATE_ROOM + STATE_SLACK);
    t1 = plat_usec_now();
    if (r >= 0) {
        uint32 cram = g->sms->gg ? CRAM_BYTES + VDP_GG_CRAM : CRAM_BYTES;

        r = STATE_BAD;
        if (f[F_MAGIC] == STATE_MAGIC && f[F_VERSION] == STATE_VERSION &&
            f[F_PACKED] <= (uint32)got - F_WORDS * 4) {
            r = STATE_OTHER;
            if (f[F_IMAGE] == g->rom_size && f[F_ID] == st_image(g) && f[F_CRAM] == cram &&
                f[F_NBANKS] <= DICT_BANKS && (f[F_NBANKS] == 0) == (g->sms->mem.bank_tab == 0))
                r = 0;
        }
        if (r == 0 && (f[F_CART] > 0x8000u || (f[F_CART] & 0x3FFF) != 0))
            r = STATE_BAD;
        if (r == 0) {
            st_bounds(&in, cram, f[F_CART]);
            if (f[F_RAW] != in.n)
                r = STATE_BAD;
        }
        if (r == 0) {
            in.s = s = (uint8 *)stp_take(m, in.n);
            in.dict = st_dict(g, m, f[F_NBANKS], f[F_BANKS], &in.dict_n);
            if (s == NULL || in.dict == NULL)
                r = STATE_NOMEM;
            else if (in.dict_n != f[F_DICT] || st_sum(in.dict, in.dict_n) != f[F_DICT_SUM])
                r = STATE_OTHER;
        }
        if (r == 0) {
            r = stp_unpack(&in, s, (const uint8 *)(f + F_WORDS), f[F_PACKED], m);
            if (r == STP_NOMEM)
                r = STATE_NOMEM;
            else if (r < 0 || st_sum(s, in.n) != f[F_RAW_SUM] || ((uint32 *)s)[0] != g->sms->gg)
                r = STATE_BAD;
        }
        if (r == 0) {
            st_apply(g, s, f[F_CART]);
            w[3] = in.n;
            w[4] = (uint32)got;
            w[7] = f[F_FRAME];
            w[8] = g->state_saved ? 0 : 0x100;
        }
    }
    st_note(g, w, 1, r, plat_usec_now() - t1, t1 - t0);
    return r;
}
