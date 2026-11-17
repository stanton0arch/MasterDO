#ifndef ROM_H
#define ROM_H

/*
 * Cartridge image loading and header inspection.
 */

#include "types.h"
#include "blockfile.h"
#include "smsmem.h"
#include "filesystem.h"

#define ROM_DIR        "roms"
#define ROM_DIR_LEN    128      /* a folder's path under ROM_DIR, with its NUL */
#define ROM_NAME_LEN   FILESYSTEM_MAX_NAME_LEN  /* a file name, with its NUL */
#define ROM_LIST_BYTES (48 * 1024)  /* the block of a folder's list (rom_dir) */
#define ROM_FOLDER     0xFFFFFFFFu  /* the size of a folder in a list */

/* A place of the ROM menu: a folder under ROM_DIR ("" for ROM_DIR itself)
 * and one of its entries (the image chosen, or the entry the cursor goes
 * to when the folder is shown again). */
typedef struct {
    char    dir[ROM_DIR_LEN];
    char    name[ROM_NAME_LEN];
    uint32  size;           /* the image's size in bytes */
} rom_place;

/* The entries of a folder: its folders, then its .sms and .gg files (the
 * other files left out), each group sorted by name whatever the case, an
 * underscore taken as a space (the menu shows it as one). At
 * the root, the folders named in rom.c's root order come first, in that
 * order. The list lives in a block of ROM_LIST_BYTES given by the caller
 * (mem), the names packed, the index of the entries at its end. */
typedef struct {
    uint8   *mem;
    uint32  *idx;           /* entry k: its record at mem + idx[k] */
    int32    count;
    int32    folders;
    int32    left_out;      /* entries the block could not hold */
    uint32   read_us;       /* time of the directory read */
    uint32   sort_us;
} rom_dir;

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

/* Reads folder dir of ROM_DIR into the list; returns the entry count or
 * a negative error (folder absent). */
int32       rom_dir_read(rom_dir *d, const char *dir);
const char *rom_dir_name(const rom_dir *d, int32 k);
/* The size of entry k in bytes, or ROM_FOLDER. */
uint32      rom_dir_size(const rom_dir *d, int32 k);
/* The entry named name, or 0 when there is none. */
int32       rom_dir_find(const rom_dir *d, const char *name);

/* Loads ROM_DIR/dir/name, a file of size bytes (from the list): as one
 * block of VRAM, or by banks above SMS_BLOCK_MAX. The image is named by
 * the file name alone (logs, pad script, save file). */
Err  rom_load(rom_image *rom, const char *dir, const char *name, uint32 size);
void rom_unload(rom_image *rom);
void rom_log(const rom_image *rom);

#endif /* ROM_H */
