#ifndef STPACK_H
#define STPACK_H

/*
 * Packing of a machine state for the NVRAM (state.c): an LZ77 over the
 * state whose matches may also copy from the cartridge image, which is in
 * memory when the state is unpacked again (an external dictionary: the
 * video RAM holds tiles the game copied from the image, often as they
 * are). The tokens (literal or match, the kind of match, lengths and
 * offsets) are coded with adaptive binary probabilities and a carry-less
 * binary arithmetic coder.
 *
 * Kinds of match: the last state offset again, a state offset (up to
 * 32 KiB back), an image address, and the image continued where the last
 * image match ended. A literal is coded under its region (head, Z80 RAM,
 * video RAM, colour RAM, cartridge RAM) and the high bits of the byte
 * before it, or in the video RAM of the byte four before (the same plane
 * of the tile row above).
 *
 * Matches into the image are found through an index of its positions
 * every stride bytes (8, or more when memory is short): a state position
 * probes the index with its next 6 bytes, a hit is extended both ways.
 *
 * The coder works in memory the caller lends (stp_mem): pieces of memory
 * nothing else uses during the call, taken best fit. Packing a state of
 * 41 KiB with an image of 512 KiB takes about 290 KiB of it, unpacking
 * 43 KiB (the probabilities) besides the state and the packed bytes.
 */

#include "types.h"

#define STP_REGIONS 5   /* head, Z80 RAM, video RAM, colour RAM, cartridge RAM */
#define STP_PIECES  4

#define STP_NOMEM   (-1)    /* not enough memory lent */
#define STP_LARGE   (-2)    /* packed larger than the room given */
#define STP_BAD     (-3)    /* packed bytes that do not decode to a state */

typedef struct {
    uint8  *p;              /* the free part of the piece */
    uint32  left;           /* its bytes */
} stp_piece;

typedef struct {
    stp_piece piece[STP_PIECES];
    uint32    n;
} stp_mem;

typedef struct {
    const uint8 *s;         /* the state (packing only) */
    uint32       n;         /* its bytes */
    uint32       rb[STP_REGIONS + 1];   /* region starts, rb[STP_REGIONS] = n */
    const uint8 *dict;      /* the image bytes matches may copy from, or NULL */
    uint32       dict_n;
} stp_in;

/* Lends n bytes at p (word aligned) to the coder. */
void   stp_lend(stp_mem *m, void *p, uint32 n);
/* n bytes of the pieces lent, best fit, word aligned; NULL without room. */
void  *stp_take(stp_mem *m, uint32 n);

/* Packs in->s into out (cap bytes at most); returns the packed size or
 * STP_NOMEM / STP_LARGE. *stride: the image index's stride (0: no image
 * matches, for want of memory or of an image). */
int32  stp_pack(const stp_in *in, uint8 *out, uint32 cap, stp_mem *m, uint32 *stride);

/* Unpacks len packed bytes into s (in->n bytes, regions and image as
 * when packed); returns 0, STP_BAD (s then holds garbage) or STP_NOMEM. */
int32  stp_unpack(const stp_in *in, uint8 *s, const uint8 *src, uint32 len, stp_mem *m);

#endif /* STPACK_H */
