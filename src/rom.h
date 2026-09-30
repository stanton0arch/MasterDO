#ifndef ROM_H
#define ROM_H

/*
 * Cartridge image loading and header inspection.
 */

#include "types.h"

#define ROM_DIR       "roms"
#define ROM_LIST_MAX  32
#define ROM_NAME_LEN  32

/* Files of the ROM directory, sorted by name. */
typedef struct {
    int32   count;
    char    name[ROM_LIST_MAX][ROM_NAME_LEN];
    uint32  size[ROM_LIST_MAX];
} rom_list;

typedef struct {
    uint8  *raw;           /* file as loaded from the CD */
    uint8  *data;          /* the image: the file, less a 512-byte copier header */
    int32   size;          /* size in bytes, or a negative error code */
    char    name[ROM_NAME_LEN];
    int32   header_offset; /* offset of "TMR SEGA", -1 when absent */
    uint32  checksum;      /* checksum stored in the header */
    uint32  product;       /* product code (BCD, 5 digits) */
    uint32  version;       /* revision nibble */
    uint32  region;        /* region code (high nibble of the last byte) */
    uint32  size_code;     /* ROM size code (low nibble of the last byte) */
} rom_image;

/* Lists the files of ROM_DIR; returns their count or a negative error. */
int32 rom_scan(rom_list *list);

/* Loads ROM_DIR/name. */
Err  rom_load(rom_image *rom, const char *name);
void rom_unload(rom_image *rom);
void rom_log(const rom_image *rom);

#endif /* ROM_H */
