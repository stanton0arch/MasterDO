#ifndef SAVE_H
#define SAVE_H

/*
 * Native saves: the cartridge RAM a game shows (the battery RAM of the
 * cartridge) kept in a file of the console's NVRAM, one per cartridge
 * image, named after the image's file. The NVRAM is small and shared with
 * every 3DO game's saves: the RAM is kept compressed (runs of a byte
 * packed: mostly empty, it takes a few hundred bytes), written only
 * when it changed, and no other file is ever written or removed (a file
 * of the same name not made by this program is left alone).
 *
 * When to write: many games also use that RAM as work memory, some
 * changing part of it every frame, and some hide it after every use.
 * game.c has the RAM looked at about once a second, a block of SAVE_BLOCK
 * bytes per frame (GAME_SAVE_STEP); a look hashes each block and keeps,
 * per block, which of the last four looks found it changed. A block
 * changed at this look and at none of the three before is a write in
 * progress (look again); a block changed at two looks or more of the four
 * is work memory (no reason to write); a block that differs from the file,
 * has not changed since the previous look and changed at most once in the
 * four is a save the game has finished: unless a write is in progress
 * elsewhere in the RAM, the look says so (GAME_SAVE_READY) and the RAM is
 * looked at whole again and written in a spare time that holds it
 * (GAME_SAVE_LOOK). When the run pauses or ends, any change is written.
 *
 * File: three words (magic, the bytes of RAM it covers, their hash) and
 * the packed bytes, each block packed on its own. Packing: a byte n below $80 is followed by n + 1
 * bytes as they are; a byte n from $80 on and the next one give a run of
 * ((n & $7F) << 8 | next) + 3 copies of the byte after them.
 *
 * With SAVE_NO_FILES defined (the cycle model includes this file), the
 * same decisions and packing without the NVRAM: nothing is read, and a
 * write packs the RAM into memory.
 */

#include "types.h"
#include "filesystem.h"

#define SAVE_DIR    "/NVRAM/"
#define SAVE_BLOCK  1024
#define SAVE_BLOCKS 32          /* the RAM: 32 KiB at most */

typedef struct {
    char   path[sizeof(SAVE_DIR) + FILESYSTEM_MAX_NAME_LEN];
    uint32 kept[SAVE_BLOCKS];   /* hash of each block as read or last written */
    uint32 seen[SAVE_BLOCKS];   /* at the last look */
    uint8  hist[SAVE_BLOCKS];   /* bit n: changed at the look n looks ago (four kept) */
    uint16 size[SAVE_BLOCKS];   /* packed size of each block, counted */
    uint8  uni[SAVE_BLOCKS];    /* its first byte (all of them when that size is 3) */
    uint32 sized[SAVE_BLOCKS];  /* at this hash of the block */
    uint32 size_ok;             /* a bit per block: counted since the load */
    uint32 next;                /* next block of the look under way */
    uint32 ready;               /* that look found a finished save, */
    uint32 busy;                /* a write in progress */
    uint32 foreign;             /* the file is not a save of this program: never written */
} save_file;

/* The file of the image named rom (a name of the ROM directory, a
 * subdirectory before it allowed). */
void  save_init(save_file *sv, const char *rom);

/* The save function of the run (game_set_saves, game.h): arg is the
 * save_file. */
int32 save_io(void *arg, uint8 *ram, uint32 bytes, uint32 why);

#endif /* SAVE_H */
