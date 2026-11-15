#ifndef ROM_H
#define ROM_H

/*
 * Cartridge image loading and header inspection.
 */

#include "types.h"
#include "blockfile.h"
#include "smsmem.h"

#define ROM_DIR       "roms"
#define ROM_GG_DIR    "gg"      /* a subdirectory of ROM_DIR: Game Gear images */
#define ROM_LIST_MAX  32
#define ROM_NAME_LEN  40        /* a file name, with ROM_GG_DIR "/" before it */

/* Files of the ROM directory sorted by name, then those of its Game Gear
 * subdirectory, named ROM_GG_DIR "/file". */
typedef struct {
    int32   count;
    char    name[ROM_LIST_MAX][ROM_NAME_LEN];
    uint32  size[ROM_LIST_MAX];
} rom_list;

typedef struct {
    uint8  *raw;           /* file as loaded from the CD */
    uint8  *data;          /* the image: the file, less a 512-byte copier header
                              (its first bank when read by banks) */
    int32   size;          /* size in bytes, or a negative error code */
    /* An image larger than SMS_BLOCK_MAX: its banks read from the file
     * into a bank cache when the mapper shows them (the file stays open
     * for the run), the first ones at the load (load_us). */
    sms_banks *banks;
    BlockFile bf;
    Item    port;
    Item    req;
    uint32  load_us;
    char    name[ROM_NAME_LEN];
    int32   header_offset; /* offset of "TMR SEGA", -1 when absent */
    uint32  checksum;      /* checksum stored in the header */
    uint32  product;       /* product code (BCD, 5 digits) */
    uint32  version;       /* revision nibble */
    uint32  region;        /* region code (high nibble of the last byte) */
    uint32  size_code;     /* ROM size code (low nibble of the last byte) */
    uint32  gg;            /* a Game Gear image: a .gg file, or a .sms file not, or
                              else a Game Gear region code (5 to 7) in the header */
} rom_image;

/* Lists the files of ROM_DIR and ROM_DIR/ROM_GG_DIR; returns their count
 * or a negative error. */
int32 rom_scan(rom_list *list);

/* Loads ROM_DIR/name, a file of size bytes (from the list): as one block
 * of VRAM, or by banks above SMS_BLOCK_MAX. */
Err  rom_load(rom_image *rom, const char *name, uint32 size);
void rom_unload(rom_image *rom);
void rom_log(const rom_image *rom);

#endif /* ROM_H */
