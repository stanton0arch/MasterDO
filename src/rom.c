/*
 * Cartridge image loading and header inspection.
 *
 * The SMS / Game Gear header is 16 bytes long and starts with "TMR SEGA".
 * It sits at $7FF0 on most cartridges, or at $3FF0 / $1FF0 on small ones:
 *   +$0A-$0B  checksum (little endian)
 *   +$0C-$0D  product code, low four BCD digits (little endian)
 *   +$0E      high nibble: fifth product digit, low nibble: version
 *   +$0F      high nibble: region code, low nibble: ROM size code
 */

#include "rom.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#include "blockfile.h"

static const char rom_signature[8] = { 'T', 'M', 'R', ' ', 'S', 'E', 'G', 'A' };
static const int32 rom_header_offsets[3] = { 0x7FF0, 0x3FF0, 0x1FF0 };

static void rom_parse_header(rom_image *rom)
{
    const uint8 *h;
    int32 i;

    rom->header_offset = -1;
    for (i = 0; i < 3; i++) {
        int32 off = rom_header_offsets[i];
        if (off + 16 <= rom->size &&
            memcmp(rom->data + off, rom_signature, sizeof(rom_signature)) == 0) {
            rom->header_offset = off;
            break;
        }
    }
    if (rom->header_offset < 0)
        return;

    h = rom->data + rom->header_offset;
    rom->checksum = (uint32)h[0x0A] | ((uint32)h[0x0B] << 8);
    rom->product = (uint32)h[0x0C] | ((uint32)h[0x0D] << 8) | (((uint32)h[0x0E] >> 4) << 16);
    rom->version = (uint32)h[0x0E] & 0x0F;
    rom->region = (uint32)h[0x0F] >> 4;
    rom->size_code = (uint32)h[0x0F] & 0x0F;
}

Err rom_load(rom_image *rom, const char *path)
{
    int32 size = 0;

    memset(rom, 0, sizeof(*rom));
    rom->header_offset = -1;

    /* The image goes to VRAM, which the CPU reads like DRAM, to leave
     * the DRAM to the translator (code buffer) and the renderer. */
    rom->data = (uint8 *)LoadFile(path, &size, MEMTYPE_VRAM);
    if (rom->data == NULL) {
        rom->size = size < 0 ? size : -1;
        printf("ERROR: cannot load %s (0x%lx)\n", path, (unsigned long)size);
        return rom->size;
    }
    rom->size = size;
    rom_parse_header(rom);
    return 0;
}

void rom_unload(rom_image *rom)
{
    if (rom->data != NULL)
        UnloadFile(rom->data);
    rom->data = NULL;
}

void rom_log(const rom_image *rom)
{
    if (rom->data == NULL) {
        printf("ROM: not loaded\n");
        return;
    }
    printf("ROM: %ld bytes, first bytes %02x %02x %02x %02x\n",
           (long)rom->size, rom->data[0], rom->data[1], rom->data[2], rom->data[3]);
    if (rom->header_offset < 0) {
        printf("ROM: no TMR SEGA header\n");
        return;
    }
    printf("ROM: header at $%04lx, checksum $%04lx, product %05lx, version %lu, "
           "region %lx, size code %lx\n",
           (unsigned long)rom->header_offset, (unsigned long)rom->checksum,
           (unsigned long)rom->product, (unsigned long)rom->version,
           (unsigned long)rom->region, (unsigned long)rom->size_code);
}
