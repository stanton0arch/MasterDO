#ifndef ROM_H
#define ROM_H

/*
 * Cartridge image loading and header inspection.
 */

#include "types.h"

typedef struct {
    uint8  *data;          /* whole image, loaded from the CD */
    int32   size;          /* size in bytes, or a negative error code */
    int32   header_offset; /* offset of "TMR SEGA", -1 when absent */
    uint32  checksum;      /* checksum stored in the header */
    uint32  product;       /* product code (BCD, 5 digits) */
    uint32  version;       /* revision nibble */
    uint32  region;        /* region code (high nibble of the last byte) */
    uint32  size_code;     /* ROM size code (low nibble of the last byte) */
} rom_image;

Err  rom_load(rom_image *rom, const char *path);
void rom_unload(rom_image *rom);
void rom_log(const rom_image *rom);

#endif /* ROM_H */
