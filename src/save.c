/*
 * Native saves in the console's NVRAM. See save.h.
 */

#include "save.h"
#include "game.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#ifndef SAVE_NO_FILES
#include "io.h"
#include "filefunctions.h"
#include "blockfile.h"
#endif

#define SAVE_MAGIC  0x4D445331u     /* "MDS1" */
#define SAVE_HEAD   12              /* magic, bytes covered, their hash */
#define SAVE_KEEP   26              /* a name too long: its first bytes, '~', a hash */
#define RUN_MIN     4               /* shorter runs stay in the plain bytes */
#define RUN_MAX     (0x7FFF + 3)
#define BLOCK_MUL   0xE87DE001u     /* 31 to the power of the words in a block */

void save_init(save_file *sv, const char *rom)
{
    const char *base = rom;
    char *name;
    uint32 h = 0;

    for (name = (char *)rom; *name != 0; name++) {
        if (*name == '/')
            base = name + 1;
    }
    memset(sv, 0, sizeof(*sv));
    strcpy(sv->path, SAVE_DIR);
    name = sv->path + sizeof(SAVE_DIR) - 1;
    if (strlen(base) < FILESYSTEM_MAX_NAME_LEN) {
        strcpy(name, base);
    } else {
        memcpy(name, base, SAVE_KEEP);
        while (*base != 0)
            h = h * 31 + (uint8)*base++;
        sprintf(name + SAVE_KEEP, "~%04lx", (unsigned long)(h & 0xFFFF));
    }
    printf("Save: %s\n", sv->path);
}

/* n plain bytes from s, in pieces of 128 at most, at offset o of d (room
 * cap: a piece past it is counted, not written); returns the offset after
 * them. */
static uint32 put_plain(const uint8 *s, uint32 n, uint8 *d, uint32 o, uint32 cap)
{
    while (n != 0) {
        uint32 k = (n > 128) ? 128 : n;

        if (o + 1 + k <= cap) {
            d[o] = (uint8)(k - 1);
            memcpy(d + o + 1, s, (size_t)k);
        }
        o += 1 + k;
        s += k;
        n -= k;
    }
    return o;
}

/* Bytes equal to s[i] from i on, up to n (s word-aligned): compared four
 * words at a time where they can be. */
static uint32 run_of(const uint8 *s, uint32 i, uint32 n)
{
    uint32 b = s[i];
    uint32 j = i + 1;

    while (j < n && (j & 3) != 0 && s[j] == b)
        j++;
    if ((j & 3) == 0) {
        const uint32 *w = (const uint32 *)(const void *)(s + j);
        uint32 v = b * 0x01010101u;

        while (j + 16 <= n) {
            uint32 w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];

            if ((w0 ^ v) | (w1 ^ v) | (w2 ^ v) | (w3 ^ v))
                break;
            w += 4;
            j += 16;
        }
    }
    while (j < n && s[j] == b)
        j++;
    return j - i;
}

/* Packs the n bytes at s (word-aligned) into d, whose room is cap bytes
 * (0 to count only); returns the packed size, written in full only when
 * it is at most cap. */
static uint32 pack(const uint8 *s, uint32 n, uint8 *d, uint32 cap)
{
    uint32 i = 0;
    uint32 plain = 0;           /* first byte not packed yet */
    uint32 o = 0;

    while (i + RUN_MIN <= n) {
        uint32 c = s[i];
        uint32 r;

        /* No run of RUN_MIN can start before the first byte that differs. */
        if (s[i + 1] != c) {
            i += 1;
            continue;
        }
        if (s[i + 2] != c) {
            i += 2;
            continue;
        }
        if (s[i + 3] != c) {
            i += 3;
            continue;
        }
        r = run_of(s, i, n);
        if (r > RUN_MAX)
            r = RUN_MAX;        /* the rest: another run, or plain bytes */
        o = put_plain(s + plain, i - plain, d, o, cap);
        if (o + 3 <= cap) {
            d[o] = (uint8)(0x80 | ((r - 3) >> 8));
            d[o + 1] = (uint8)(r - 3);
            d[o + 2] = (uint8)c;
        }
        o += 3;
        i += r;
        plain = i;
    }
    return put_plain(s + plain, n - plain, d, o, cap);
}

/* Hash of n bytes (a multiple of 32). */
static uint32 sum_of(const uint8 *ram, uint32 n)
{
    return (n != 0) ? z80j_hash_words(0, (const uint32 *)(const void *)ram, n / 4) : 0;
}

#ifndef SAVE_NO_FILES
/* Unpacks the n bytes at s into d (max bytes at most); returns the bytes
 * unpacked, or 0xFFFFFFFF for a stream that does not fit. */
static uint32 unpack(const uint8 *s, uint32 n, uint8 *d, uint32 max)
{
    uint32 i = 0;
    uint32 o = 0;

    while (i < n) {
        uint32 c = s[i++];

        if (c < 0x80) {
            c++;
            if (i + c > n || o + c > max)
                return 0xFFFFFFFFu;
            memcpy(d + o, s + i, (size_t)c);
            i += c;
        } else {
            if (i + 2 > n)
                return 0xFFFFFFFFu;
            c = (((c & 0x7F) << 8) | s[i]) + 3;
            if (o + c > max)
                return 0xFFFFFFFFu;
            memset(d + o, s[i + 1], (size_t)c);
            i += 2;
        }
        o += c;
    }
    return o;
}

/* Fills ram (zero-filled) from the file, if any, and the blocks' hashes
 * in sv->seen; returns the file's size, 0 without a file, or
 * GAME_SAVE_BAD (the RAM left blank) or GAME_SAVE_FOREIGN. */
static int32 read_file(save_file *sv, uint8 *ram, uint32 bytes)
{
    int32 got = 0;
    uint32 *f = (uint32 *)LoadFile(sv->path, &got, MEMTYPE_ANY);
    int32 r = 0;

    if (f != NULL) {
        r = GAME_SAVE_FOREIGN;
        sv->foreign = 1;
        if (got >= SAVE_HEAD && f[0] == SAVE_MAGIC) {
            uint32 n = f[1];

            sv->foreign = 0;
            r = GAME_SAVE_BAD;
            if (n <= bytes && (n % SAVE_BLOCK) == 0 &&
                unpack((const uint8 *)(f + 3), (uint32)got - SAVE_HEAD, ram, n) == n) {
                uint32 sum = 0;
                uint32 k;

                for (k = 0; k < n / SAVE_BLOCK; k++) {
                    sv->seen[k] = sum_of(ram + k * SAVE_BLOCK, SAVE_BLOCK);
                    sum = sum * BLOCK_MUL + sv->seen[k];
                }
                if (sum == f[2])
                    r = got;
            }
            if (r < 0) {
                memset(ram, 0, (size_t)bytes);
                memset(sv->seen, 0, sizeof(sv->seen));
            }
        }
        UnloadFile(f);
    }
    return r;
}

/* One command on the file. */
static Err file_io(Item req, uint32 cmd, int32 offset, void *send, void *recv, int32 len)
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

/* The file's n bytes in buf: the head, then each block of the RAM's len
 * bytes packed on its own (hash h); a block counted as one run (3 bytes)
 * is not read again. Returns 0, or GAME_SAVE_BAD when the blocks do not
 * pack to the size counted (a block changed under an unchanged hash):
 * nothing is then written past n. */
static int32 fill(const save_file *sv, const uint8 *ram, uint32 len, uint32 h, uint32 *buf,
                  uint32 n)
{
    uint8 *d = (uint8 *)buf;
    uint32 o = SAVE_HEAD;
    uint32 k;

    buf[0] = SAVE_MAGIC;
    buf[1] = len;
    buf[2] = h;
    for (k = 0; k < len / SAVE_BLOCK && o <= n; k++) {
        if (sv->size[k] == 3 && o + 3 <= n) {
            d[o] = (uint8)(0x80 | ((SAVE_BLOCK - 3) >> 8));
            d[o + 1] = (uint8)(SAVE_BLOCK - 3);
            d[o + 2] = sv->uni[k];
            o += 3;
        } else {
            o += pack(ram + k * SAVE_BLOCK, SAVE_BLOCK, d + o, n - o);
        }
    }
    return (o == n) ? 0 : GAME_SAVE_BAD;
}

/* Writes the RAM's len bytes (hash h, n bytes once packed with the head)
 * to the file, creating it when absent: the blocks it lacks are asked
 * for before anything is written, so that a full NVRAM leaves the save
 * it holds as it was (and a file just created is removed). Returns n or
 * an error. */
static int32 write_file(save_file *sv, const uint8 *ram, uint32 len, uint32 h, uint32 n)
{
#ifdef SAVE_NO_FILES
    uint32 need = (n + 3) & ~3u;
    uint32 *buf = (uint32 *)AllocMem((int32)need, MEMTYPE_DRAM);
    Err r = GAME_SAVE_MEM;

    if (buf != NULL) {
        r = fill(sv, ram, len, h, buf, n);
        FreeMem(buf, (int32)need);
    }
#else
    Item f = OpenDiskFile(sv->path);
    Item req;
    FileStatus st;
    uint32 made = 0;
    uint32 need = 0;
    uint32 *buf;
    Err r;

    if (f < 0) {
        made = 1;
        r = CreateFile(sv->path);
        if (r < 0)
            return r;
        f = OpenDiskFile(sv->path);
        if (f < 0) {
            DeleteFile(sv->path);
            return f;
        }
    }
    r = req = CreateIOReq(NULL, 0, f, 0);
    if (req >= 0) {
        r = file_io(req, CMD_STATUS, 0, NULL, &st, sizeof(st));
        if (r >= 0) {
            need = (n + st.fs.ds_DeviceBlockSize - 1) / st.fs.ds_DeviceBlockSize;
            if (need > st.fs.ds_DeviceBlockCount &&
                file_io(req, FILECMD_ALLOCBLOCKS, (int32)(need - st.fs.ds_DeviceBlockCount),
                        NULL, NULL, 0) < 0)
                r = GAME_SAVE_FULL;
            need *= st.fs.ds_DeviceBlockSize;
        }
        if (r >= 0)
            r = file_io(req, FILECMD_SETEOF, (int32)n, NULL, NULL, 0);
        if (r >= 0) {
            r = GAME_SAVE_MEM;
            buf = (uint32 *)AllocMem((int32)need, MEMTYPE_DMA | MEMTYPE_FILL);
            if (buf != NULL) {
                r = fill(sv, ram, len, h, buf, n);
                if (r >= 0)
                    r = file_io(req, CMD_WRITE, 0, buf, NULL, (int32)need);
                FreeMem(buf, (int32)need);
            }
        }
        DeleteIOReq(req);
    }
    CloseDiskFile(f);
    if (r < 0 && made)
        DeleteFile(sv->path);
#endif
    return (r < 0) ? r : (int32)n;
}

/* Block k looked at (save.h): its hash, its history, and what it tells
 * of the look under way. */
static void look_block(save_file *sv, const uint8 *ram, uint32 k)
{
    uint32 h = sum_of(ram + k * SAVE_BLOCK, SAVE_BLOCK);
    uint32 m = ((uint32)sv->hist[k] << 1) & 0xF;
    uint32 lone;

    if (h != sv->seen[k]) {
        sv->seen[k] = h;
        m |= 1;
    }
    sv->hist[k] = (uint8)m;
    lone = (m & (m - 1)) == 0;              /* changed at one look of the four at most */
    sv->busy |= m & lone;
    sv->ready |= (h != sv->kept[k]) & (~m & lone);
}

/* The end of a look: 0 (the RAM as kept), GAME_SAVE_READY (a finished
 * save, no write in progress) or GAME_SAVE_WAIT. */
static int32 verdict(save_file *sv, uint32 bytes)
{
    uint32 diff = 0;
    uint32 k;
    int32 r;

    for (k = 0; k < bytes / SAVE_BLOCK; k++)
        diff |= sv->seen[k] ^ sv->kept[k];
    r = (diff == 0) ? 0 : (sv->ready && !sv->busy) ? GAME_SAVE_READY : GAME_SAVE_WAIT;
    sv->ready = sv->busy = sv->next = 0;
    return r;
}

/* Writes the RAM as the look just hashed it; the packed size of a block
 * is counted again only when its hash changed. A content tried once is
 * not tried again, whatever the result. */
static int32 write_ram(save_file *sv, const uint8 *ram, uint32 bytes)
{
    uint32 sum = 0;             /* the whole RAM's hash, from its blocks' */
    uint32 need = SAVE_HEAD;
    uint32 k;

    memcpy(sv->kept, sv->seen, sizeof(sv->kept));
    if (sv->foreign)
        return GAME_SAVE_FOREIGN;
    for (k = 0; k < bytes / SAVE_BLOCK; k++) {
        if ((sv->size_ok & ((uint32)1 << k)) == 0 || sv->sized[k] != sv->seen[k]) {
            sv->size[k] = (uint16)pack(ram + k * SAVE_BLOCK, SAVE_BLOCK, 0, 0);
            sv->uni[k] = ram[k * SAVE_BLOCK];       /* the byte of a block of one run */
            sv->sized[k] = sv->seen[k];
            sv->size_ok |= (uint32)1 << k;
        }
        need += sv->size[k];
        sum = sum * BLOCK_MUL + sv->seen[k];
    }
    return write_file(sv, ram, bytes, sum, need);
}

/* GAME_SAVE_STEP: the next block of the look under way (GAME_SAVE_MORE
 * until its last). GAME_SAVE_LOOK, GAME_SAVE_END: a whole look at once
 * (a look under way dropped), and the write of a finished save (at the
 * end: of any change). */
static int32 store(save_file *sv, const uint8 *ram, uint32 bytes, uint32 why)
{
    uint32 k;
    int32 r;

    if (why == GAME_SAVE_STEP) {
        look_block(sv, ram, sv->next++);
        return (sv->next < bytes / SAVE_BLOCK) ? GAME_SAVE_MORE : verdict(sv, bytes);
    }
    sv->ready = sv->busy = 0;
    for (k = 0; k < bytes / SAVE_BLOCK; k++)
        look_block(sv, ram, k);
    r = verdict(sv, bytes);
    if (r == GAME_SAVE_READY || (r != 0 && why == GAME_SAVE_END))
        r = write_ram(sv, ram, bytes);
    return r;
}

int32 save_io(void *arg, uint8 *ram, uint32 bytes, uint32 why)
{
    save_file *sv = (save_file *)arg;
    int32 r = 0;
    uint32 k;

    if (why != GAME_SAVE_LOAD)
        return store(sv, ram, bytes, why);
    /* The RAM is given zero-filled: a block of zeros hashes to 0. */
    memset(sv->seen, 0, sizeof(sv->seen));
#ifndef SAVE_NO_FILES
    r = read_file(sv, ram, bytes);
#endif
    /* A blank RAM: every block one run of zeros. */
    sv->size_ok = (r > 0) ? 0 : 0xFFFFFFFFu;
    for (k = 0; k < bytes / SAVE_BLOCK; k++) {
        sv->kept[k] = sv->seen[k];
        sv->sized[k] = 0;
        sv->size[k] = 3;
        sv->uni[k] = 0;
    }
    return r;
}
