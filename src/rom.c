/*
 * Cartridge image loading and header inspection.
 *
 * The SMS / Game Gear header is 16 bytes long and starts with "TMR SEGA".
 * It sits at $7FF0 on most cartridges, or at $3FF0 / $1FF0 on small ones:
 *   +$0A-$0B  checksum (little endian)
 *   +$0C-$0D  product code, low four BCD digits (little endian)
 *   +$0E      high nibble: fifth product digit, low nibble: version
 *   +$0F      high nibble: region code, low nibble: ROM size code
 * The region code tells the Master System (3, 4) from the Game Gear (5
 * Japan, 6 export, 7 international) where the file name does not.
 */

#include "rom.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#include "blockfile.h"
#include "filesystem.h"
#include "directory.h"
#include "directoryfunctions.h"
#include "filefunctions.h"
#include "msgport.h"
#include "io.h"
#include "platform.h"

static const char rom_signature[8] = { 'T', 'M', 'R', ' ', 'S', 'E', 'G', 'A' };
static const int32 rom_header_offsets[3] = { 0x7FF0, 0x3FF0, 0x1FF0 };

/* The image's byte at off (in its first two banks when read by banks). */
static const uint8 *rom_at(const rom_image *rom, int32 off)
{
    if (rom->banks != NULL)
        return rom->banks->tab[off >> 14] + (off & 0x3FFF);
    return rom->data + off;
}

static void rom_parse_header(rom_image *rom)
{
    const uint8 *h;
    int32 i;

    rom->header_offset = -1;
    for (i = 0; i < 3; i++) {
        int32 off = rom_header_offsets[i];
        if (off + 16 <= rom->size &&
            memcmp(rom_at(rom, off), rom_signature, sizeof(rom_signature)) == 0) {
            rom->header_offset = off;
            break;
        }
    }
    if (rom->header_offset < 0)
        return;

    h = rom_at(rom, rom->header_offset);
    rom->checksum = (uint32)h[0x0A] | ((uint32)h[0x0B] << 8);
    rom->product = (uint32)h[0x0C] | ((uint32)h[0x0D] << 8) | (((uint32)h[0x0E] >> 4) << 16);
    rom->version = (uint32)h[0x0E] & 0x0F;
    rom->region = (uint32)h[0x0F] >> 4;
    rom->size_code = (uint32)h[0x0F] & 0x0F;
}

/* Does the name end with the extension ext (lower case, with its dot),
 * whatever the case of the name? */
static uint32 has_ext(const char *name, const char *ext)
{
    uint32 n = strlen(name);
    uint32 e = strlen(ext);
    uint32 k;

    if (n < e)
        return 0;
    for (k = 0; k < e; k++) {
        char c = name[n - e + k];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[k])
            return 0;
    }
    return 1;
}

/*--------------------------------------------------------------------------
 * Folder lists of the ROM menu
 *
 * A record per entry, packed from the start of the block: the size (word,
 * ROM_FOLDER for a folder), the rank (byte: the order of the groups), the
 * name and its NUL, padded to a word. The index (a word per entry, the
 * offset of its record) grows down from the end of the block, with room
 * kept below it for the merge sort's second index.
 *------------------------------------------------------------------------*/

#define REC_RANK 4              /* offset of the rank byte in a record */
#define REC_NAME 5              /* offset of the name */
#define RANK_FOLDER 8           /* folders not in the root order */
#define RANK_FILE   9

/* The folders of the root shown first, in this order (one per console,
 * underscores read as spaces); any other folder of the root follows them,
 * sorted by name. */
static const char *const root_order[] = { "Master System", "Game Gear" };

/* A character as the names are compared: lower case, an underscore as
 * the space it is shown as (the menu shows underscores as spaces: names
 * under the CD's root directory cannot hold spaces, the build's
 * dependency list splits them). */
static uint32 lower(uint32 c)
{
    if (c == '_')
        return ' ';
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Names compared whatever the case and the underscores. */
static int32 name_ci(const char *a, const char *b)
{
    const uint8 *x = (const uint8 *)a;
    const uint8 *y = (const uint8 *)b;

    for (;; x++, y++) {
        uint32 c = lower(*x);
        uint32 d = lower(*y);

        if (c != d || c == 0)
            return (int32)c - (int32)d;
    }
}

/* Names compared as shown, then as they are. */
static int32 name_cmp(const char *a, const char *b)
{
    int32 r = name_ci(a, b);

    return (r != 0) ? r : strcmp(a, b);
}

static int32 rec_cmp(const uint8 *a, const uint8 *b)
{
    if (a[REC_RANK] != b[REC_RANK])
        return (int32)a[REC_RANK] - (int32)b[REC_RANK];
    return name_cmp((const char *)a + REC_NAME, (const char *)b + REC_NAME);
}

static uint32 entry_rank(uint32 root, uint32 folder, const char *name)
{
    uint32 k;

    if (!folder)
        return RANK_FILE;
    if (root) {
        for (k = 0; k < sizeof(root_order) / sizeof(root_order[0]); k++) {
            if (name_ci(name, root_order[k]) == 0)
                return k;
        }
    }
    return RANK_FOLDER;
}

/* Bottom-up merge sort of the n offsets of idx (tmp: room for n more),
 * stable. */
static void sort_entries(const uint8 *mem, uint32 *idx, uint32 *tmp, uint32 n)
{
    uint32 *a = idx;
    uint32 *b = tmp;
    uint32 w;

    for (w = 1; w < n; w *= 2) {
        uint32 i;
        uint32 *t;

        for (i = 0; i < n; i += 2 * w) {
            uint32 mid = (i + w < n) ? i + w : n;
            uint32 hi = (i + 2 * w < n) ? i + 2 * w : n;
            uint32 p = i;
            uint32 q = mid;
            uint32 k = i;

            while (p < mid && q < hi)
                b[k++] = (rec_cmp(mem + a[q], mem + a[p]) < 0) ? a[q++] : a[p++];
            while (p < mid)
                b[k++] = a[p++];
            while (q < hi)
                b[k++] = a[q++];
        }
        t = a;
        a = b;
        b = t;
    }
    if (a != idx)
        memcpy(idx, a, n * sizeof(uint32));
}

/* Opens folder dir of ROM_DIR. */
static Directory *open_folder(const char *dir)
{
    char path[FILESYSTEM_MAX_PATH_LEN];
    Directory *d;

    strcpy(path, ROM_DIR);
    if (dir[0]) {
        strcat(path, "/");
        strcat(path, dir);
    }
    d = OpenDirectoryPath(path);
    if (d != NULL)
        return d;
    /* Relative to the current directory, spelled out. */
    if (GetDirectory(path, sizeof(path) - sizeof(ROM_DIR) - ROM_DIR_LEN) < 0)
        return NULL;
    strcat(path, "/" ROM_DIR);
    if (dir[0]) {
        strcat(path, "/");
        strcat(path, dir);
    }
    return OpenDirectoryPath(path);
}

int32 rom_dir_read(rom_dir *d, const char *dir)
{
    uint32 *top = (uint32 *)(d->mem + ROM_LIST_BYTES);
    uint32 used = 0;
    uint32 t0 = plat_usec_now();
    uint32 t1;
    Directory *f;
    DirectoryEntry de;

    d->count = 0;
    d->folders = 0;
    d->left_out = 0;
    d->idx = top;
    f = open_folder(dir);
    if (f == NULL)
        return -1;
    while (ReadDirectory(f, &de) >= 0) {
        uint32 folder = (de.de_Flags & FILE_IS_DIRECTORY) != 0;
        uint32 len = 0;
        uint32 need;
        uint8 *rec;

        while (len < FILESYSTEM_MAX_NAME_LEN - 1 && de.de_FileName[len] != 0)
            len++;
        de.de_FileName[len] = 0;
        if (len == 0 || (!folder && !has_ext(de.de_FileName, ".sms") &&
                         !has_ext(de.de_FileName, ".gg")))
            continue;
        /* The record, its index word and the sort's one below the index. */
        need = (REC_NAME + len + 1 + 3) & ~3u;
        if (used + need + 2 * sizeof(uint32) * (uint32)(d->count + 1) > ROM_LIST_BYTES) {
            d->left_out++;
            continue;
        }
        rec = d->mem + used;
        *(uint32 *)rec = folder ? ROM_FOLDER : de.de_ByteCount;
        rec[REC_RANK] = (uint8)entry_rank(dir[0] == 0, folder, de.de_FileName);
        memcpy(rec + REC_NAME, de.de_FileName, len + 1);
        d->count++;
        d->folders += (int32)folder;
        top[-d->count] = used;
        used += need;
    }
    CloseDirectory(f);
    t1 = plat_usec_now();
    d->idx = top - d->count;
    sort_entries(d->mem, d->idx, d->idx - d->count, (uint32)d->count);
    d->sort_us = plat_usec_now() - t1;
    d->read_us = t1 - t0;
    return d->count;
}

const char *rom_dir_name(const rom_dir *d, int32 k)
{
    return (const char *)d->mem + d->idx[k] + REC_NAME;
}

uint32 rom_dir_size(const rom_dir *d, int32 k)
{
    return *(const uint32 *)(d->mem + d->idx[k]);
}

int32 rom_dir_find(const rom_dir *d, const char *name)
{
    int32 k;

    for (k = 0; k < d->count; k++) {
        if (strcmp(rom_dir_name(d, k), name) == 0)
            return k;
    }
    return 0;
}

/* Bank b of an image read by banks (the reader of its bank cache), from
 * the file; waits for the read: the emulation stops meanwhile. */
static uint32 read_bank(void *arg, uint32 b, uint8 *to)
{
    rom_image *rom = (rom_image *)arg;
    Err e;

    e = AsynchReadBlockFile(&rom->bf, rom->req, to, 0x4000, (int32)(b * 0x4000u));
    if (e >= 0)
        e = WaitReadDoneBlockFile(rom->req);
    return e < 0;
}

Err rom_load(rom_image *rom, const char *dir, const char *name, uint32 size)
{
    char path[sizeof(ROM_DIR) + ROM_DIR_LEN + ROM_NAME_LEN];

    memset(rom, 0, sizeof(*rom));
    rom->header_offset = -1;
    strncpy(rom->name, name, ROM_NAME_LEN - 1);
    strcpy(path, ROM_DIR "/");
    if (dir[0]) {
        strcat(path, dir);
        strcat(path, "/");
    }
    strcat(path, rom->name);

    if (size > SMS_BLOCK_MAX + 512) {
        /* Larger than the VRAM holds: read by banks, the file kept open
         * with a reply port for its requests (WaitIO never returns on the
         * console without one). */
        uint32 t0 = plat_usec_now();

        rom->size = -1;
        if ((size & 0x3FFF) != 0 || size > SMS_BANK_TAB * 0x4000u) {
            printf("ERROR: %s: an image read by banks must be whole banks of 16 KiB, "
                   "4 MiB at most (no copier header)\n", path);
            return -1;
        }
        if (OpenBlockFile(path, &rom->bf) < 0) {
            printf("ERROR: cannot open %s\n", path);
            return -1;
        }
        rom->size = (int32)size;
        rom->port = CreateMsgPort(NULL, 0, 0);
        rom->req = CreateBlockFileIOReq(rom->bf.fDevice, rom->port);
        if (rom->req >= 0)
            rom->banks = sms_banks_new(size / 0x4000u, 0, SMS_BANK_DRAM, read_bank, rom);
        rom->load_us = plat_usec_now() - t0;
        if (rom->banks == NULL) {
            printf("ERROR: cannot read %s by banks (no memory or a read error)\n", path);
            rom_unload(rom);
            rom->size = -1;
            return -1;
        }
        rom->data = (uint8 *)rom->banks->tab[0];
    } else {
        /* The image goes to VRAM, which the CPU reads like DRAM, to leave
         * the DRAM to the translator (code buffer) and the renderer. */
        int32 got = 0;

        rom->raw = (uint8 *)LoadFile(path, &got, MEMTYPE_VRAM);
        if (rom->raw == NULL) {
            rom->size = got < 0 ? got : -1;
            printf("ERROR: cannot load %s (0x%lx)\n", path, (unsigned long)got);
            return rom->size;
        }
        rom->data = rom->raw;
        rom->size = got;
        /* Some dumps carry a 512-byte copier header before the image. */
        if (got > 512 && (got & 0x3FFF) == 512) {
            rom->data += 512;
            rom->size -= 512;
            printf("ROM: %s has a 512-byte header, skipped\n", rom->name);
        }
    }
    rom_parse_header(rom);
    if (has_ext(rom->name, ".gg"))
        rom->gg = 1;
    else if (has_ext(rom->name, ".sms"))
        rom->gg = 0;
    else
        rom->gg = rom->header_offset >= 0 && rom->region >= 5 && rom->region <= 7;
    return 0;
}

void rom_unload(rom_image *rom)
{
    if (rom->raw != NULL)
        UnloadFile(rom->raw);
    if (rom->banks != NULL)
        sms_banks_free(rom->banks);
    if (rom->bf.fDevice > 0) {
        if (rom->req > 0)
            DeleteIOReq(rom->req);
        if (rom->port > 0)
            DeleteMsgPort(rom->port);
        CloseBlockFile(&rom->bf);
    }
    rom->raw = NULL;
    rom->data = NULL;
    rom->banks = NULL;
    rom->bf.fDevice = 0;
}

void rom_log(const rom_image *rom)
{
    if (rom->data == NULL) {
        printf("ROM: not loaded\n");
        return;
    }
    printf("ROM: %s, %ld bytes, first bytes %02x %02x %02x %02x\n", rom->name,
           (long)rom->size, rom->data[0], rom->data[1], rom->data[2], rom->data[3]);
    if (rom->banks != NULL)
        printf("ROM: %ld banks read when mapped, a cache of %lu (%lu in DRAM), the first "
               "ones read in %lu us\n", (long)(rom->size >> 14), (unsigned long)rom->banks->nbuf,
               (unsigned long)rom->banks->ndram, (unsigned long)rom->load_us);
    if (rom->header_offset < 0) {
        printf("ROM: no TMR SEGA header\n");
        return;
    }
    printf("ROM: header at $%04lx, checksum $%04lx, product %05lx, version %lu, "
           "region %lx, size code %lx; %s image\n",
           (unsigned long)rom->header_offset, (unsigned long)rom->checksum,
           (unsigned long)rom->product, (unsigned long)rom->version,
           (unsigned long)rom->region, (unsigned long)rom->size_code,
           rom->gg ? "Game Gear" : "Master System");
}
