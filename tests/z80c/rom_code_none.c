/*
 * The empty table of translated code: the file src/rom_code.c is a copy
 * of until the host tool writes a real one there.
 *
 * Same symbols, same header as a generated file, so that it compiles on
 * the console chain and on the PC alike; no block, so that the core never
 * arms and runs the interpreter alone. `make` copies it into src/ when
 * that file is missing (Makefile); the benches link it by name.
 */
#include "z80c.h"

const uint32 z80c_rom_size    = 0UL;
const uint32 z80c_rom_fnv     = 0UL;
const uint32 z80c_code_bytes  = 0UL;
const uint32 z80c_block_count = 0UL;

const z80c_entry_t z80c_table[1] = { { 0UL, 0 } };

#if Z80C_HITS
uint32 z80c_hits[1];
uint32 z80c_tstates[1];
#endif
