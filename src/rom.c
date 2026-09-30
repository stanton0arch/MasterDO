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
#include "filesystem.h"
#include "directory.h"
#include "directoryfunctions.h"
#include "filefunctions.h"

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

int32 rom_scan(rom_list *list)
{
    Directory *dir;
    DirectoryEntry de;

    list->count = 0;
    dir = OpenDirectoryPath(ROM_DIR);
    if (dir == NULL) {
        /* Relative to the current directory, spelled out. */
        char path[256];

        if (GetDirectory(path, sizeof(path) - 8) >= 0) {
            strcat(path, "/" ROM_DIR);
            dir = OpenDirectoryPath(path);
        }
        if (dir == NULL) {
            printf("ERROR: cannot open the " ROM_DIR " directory (%s)\n", path);
            return -1;
        }
    }
    while (ReadDirectory(dir, &de) >= 0) {
        int32 k;
        int32 n = list->count;

        if (de.de_Flags & FILE_IS_DIRECTORY)
            continue;
        if (n >= ROM_LIST_MAX) {
            printf("ROM: more than %d files in " ROM_DIR ", the rest is ignored\n",
                   ROM_LIST_MAX);
            break;
        }
        /* Insertion in name order. */
        for (k = n; k > 0 && strcmp(list->name[k - 1], de.de_FileName) > 0; k--) {
            strcpy(list->name[k], list->name[k - 1]);
            list->size[k] = list->size[k - 1];
        }
        strncpy(list->name[k], de.de_FileName, ROM_NAME_LEN - 1);
        list->name[k][ROM_NAME_LEN - 1] = 0;
        list->size[k] = de.de_ByteCount;
        list->count++;
    }
    CloseDirectory(dir);
    return list->count;
}

Err rom_load(rom_image *rom, const char *name)
{
    char path[ROM_NAME_LEN + 8];
    int32 size = 0;

    memset(rom, 0, sizeof(*rom));
    rom->header_offset = -1;
    strncpy(rom->name, name, ROM_NAME_LEN - 1);
    strcpy(path, ROM_DIR "/");
    strcat(path, rom->name);

    /* The image goes to VRAM, which the CPU reads like DRAM, to leave
     * the DRAM to the translator (code buffer) and the renderer. */
    rom->raw = (uint8 *)LoadFile(path, &size, MEMTYPE_VRAM);
    if (rom->raw == NULL) {
        rom->size = size < 0 ? size : -1;
        printf("ERROR: cannot load %s (0x%lx)\n", path, (unsigned long)size);
        return rom->size;
    }
    rom->data = rom->raw;
    rom->size = size;
    /* Some dumps carry a 512-byte copier header before the image. */
    if (size > 512 && (size & 0x3FFF) == 512) {
        rom->data += 512;
        rom->size -= 512;
        printf("ROM: %s has a 512-byte header, skipped\n", rom->name);
    }
    rom_parse_header(rom);
    return 0;
}

void rom_unload(rom_image *rom)
{
    if (rom->raw != NULL)
        UnloadFile(rom->raw);
    rom->raw = NULL;
    rom->data = NULL;
}

void rom_log(const rom_image *rom)
{
    if (rom->data == NULL) {
        printf("ROM: not loaded\n");
        return;
    }
    printf("ROM: %s, %ld bytes, first bytes %02x %02x %02x %02x\n", rom->name,
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
