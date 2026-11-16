#ifndef STATE_H
#define STATE_H

/*
 * One savestate for the whole console, kept in a file of the NVRAM so
 * that it outlives a reset or a power-off: the machine as it stands
 * between two frames (Z80 registers and interrupt state, Z80 RAM, VDP
 * registers, access and line counter state, video and colour RAMs,
 * paging registers, cartridge RAM once given, PSG registers, Game Gear
 * ports), packed by stpack.c with the cartridge image as a dictionary.
 *
 * File: a head of words (magic, version, the image's size and a hash of
 * its first bank, the dictionary and its hash, the state's size and hash,
 * the packed size, the frame it was saved at, the sizes of the colour and
 * cartridge RAMs), then the packed state. The dictionary is the image
 * when it is in memory as one block, else bank 0 and the banks the three
 * slots show (read from the image file at the load if the bank cache no
 * longer holds them). A state is loaded only into the image it was saved
 * from; a file that does not unpack to the state's hash is refused.
 *
 * Loading puts the machine back as it was, at the frame boundary it was
 * saved at: the paging registers applied (absent banks read), the
 * cartridge RAM given if the state holds it, the timing of the frame
 * taken up from the boundary, every tile and colour marked written, the
 * renderer reset (the next picture is drawn whole), the interrupted
 * segments the translator remembered forgotten (their machine state is
 * gone); its translations stay (an entry check guards code in RAM).
 *
 * Both calls work in memory the caller lends (stp_mem: memory nothing
 * else uses during the call, such as screens not on display): about 300
 * KiB to save a state of an image of 512 KiB, 200 KiB to load one. The
 * result is logged (State: line) and kept in the pad log (GAME_PAD_SAVE,
 * GAME_PAD_LOAD) so that the cycle model can replay the run.
 *
 * With STATE_NO_FILES defined (the cycle model includes this file), the
 * file is a block of memory.
 */

#include "types.h"
#include "game.h"
#include "stpack.h"

#define STATE_NONE   (-10)  /* no state in the NVRAM */
#define STATE_OTHER  (-11)  /* a state of another cartridge image */
#define STATE_BAD    (-12)  /* a damaged state: the machine left as it was */
#define STATE_NOMEM  (-13)  /* not enough memory lent */
#define STATE_FULL   (-14)  /* no room in the NVRAM: the state kept there is unchanged */
#define STATE_LARGE  (-15)  /* larger packed than the room of the file */

/* Writes the machine's state to the NVRAM; returns the file's bytes or
 * an error (STATE_*, or a system error). */
int32 state_save(game *g, stp_mem *m);

/* Puts the machine back as the state in the NVRAM has it; returns 0 or
 * an error (the machine then left as it was). */
int32 state_load(game *g, stp_mem *m);

/* An error in words, for the log. */
const char *state_why(int32 r);

#endif /* STATE_H */
