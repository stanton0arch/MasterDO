/*
 * The cartridge this repository writes itself, for the one case no real
 * ROM of takeme/roms/ produces: code that moves the very window it is
 * running in, in the middle of a chain of translated blocks.
 *
 * Why it exists. A block renders its linear successor as a table entry
 * whenever the next instruction is in the same 16k bank
 * (tests/z80c/translate.c, succ_expr): the linear case carries no window
 * test at all, because a position in the block's own bank sits at the
 * same offset whatever slot that bank is turned into. The only thing
 * standing between that rendered successor and the bytes the mapper has
 * just put in its place is the epoch: src/cart.c steps z80c_map_epoch on
 * every bank change and src/z80c.c, z80c_run, drops the successor and
 * asks the live page table again when the epoch has moved. Real cartridge
 * code never pulls that rug from under itself -- a program that changes
 * the bank it is executing in would have to have identical bytes at the
 * address in both banks -- so on the reference ROM the increment can be
 * deleted and the side-by-side check stays green: the guard is never
 * seen to bite. This image makes it bite.
 *
 * The shape. Four banks of 16k, the smallest power of two that gives the
 * second slot two different banks to hold.
 *
 *   bank 0, at 0x0000 -- the reset vector, inside the fixed first
 *     kilobyte of slot 0, so it renders no successor past it:
 *       di                 interrupts off: the run is the loop below and
 *                          nothing else, frame after frame
 *       ld a,$02
 *       ld ($FFFF),a       slot 2 <- bank 2
 *       or a               a is 2, so the test below is not taken
 *       jp z,$8005         never taken at run time; walked, so that
 *                          $8005 of bank 2 is a block start and the
 *                          block below closes on the mapper write
 *       jp $8000
 *
 *   bank 2, at $8000 -- reached with slot 2 holding bank 2:
 *       ld a,$03
 *       ld ($FFFF),a       slot 2 <- bank 3: its OWN window, mid-block
 *       --- block ends here, its linear successor rendered as the table
 *           entry of $8005 IN BANK 2, which is no longer what $8005 holds
 *       ld c,$11           bank 2's byte, the wrong one from here on
 *       jp $8000
 *
 *   bank 3, at $8000 -- the same, the other way round, so the two banks
 *     trade places for ever and the guard is asked on every pass:
 *       ld a,$02
 *       ld ($FFFF),a       slot 2 <- bank 2
 *       ld c,$22           bank 3's byte
 *       jp $8000
 *
 * What the check sees. With the epoch stepped, the chain drops the
 * rendered successor, the live page table is asked, nothing is written
 * at bank 3's $8005 -- the walk never reaches bank 3, since an address in
 * slot 2 is planned into bank 2 -- and the interpreter takes over and
 * loads c with $22. With the increment deleted, the chain walks into
 * bank 2's own $8005 block and loads c with $11. The two runs part on
 * the bc pair at the end of the line, which is the first thing the
 * replay compares (tests/z80c/sidebyside.c).
 *
 * Everything not written here is $18 $FE, jr to itself: the two vectors
 * the walk starts from besides the reset one hold it, so discovery stops
 * on them at once and goes no further, and a jump into any byte this
 * file does not fill spins on the spot instead of running the filler as
 * code. The two fillers one would reach for first both walk the whole
 * image instead: halt is an instruction the interpreter keeps, which
 * makes the byte after it a start, and rst $00 is a call, whose return
 * address is a start too -- either one cascades from vector to vector
 * through every byte of the file.
 *
 *   rom_bank <out.sms>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BANK_SIZE 16384UL
#define BANKS     4UL
#define ROM_SIZE  (BANK_SIZE * BANKS)

static unsigned char rom[ROM_SIZE];

/* The filler: jr to itself, an unconditional transfer that names a
   target already walked, so that discovery stops on it and no byte of
   this image is code by accident. Read one byte off, it is a compare of
   an immediate followed by the same jump, which stops just as fast. */
static const unsigned char filler[2] = { 0x18, 0xFE };

/* The image filled with it, two bytes at a time. */
static void
fill(void)
{
  unsigned long at;

  for(at = 0; at < ROM_SIZE; at += sizeof filler)
    memcpy(rom + at,filler,sizeof filler);
}

/* The reset vector, in the fixed first kilobyte of bank 0. */
static const unsigned char boot[] = {
  0xF3,                   /* di                                        */
  0x3E, 0x02,             /* ld a,$02                                  */
  0x32, 0xFF, 0xFF,       /* ld ($FFFF),a   slot 2 <- bank 2           */
  0xB7,                   /* or a           a is 2: not zero           */
  0xCA, 0x05, 0x80,       /* jp z,$8005     walked, never taken        */
  0xC3, 0x00, 0x80        /* jp $8000                                  */
};

/* The body of a bank of slot 2: turn the other bank in, then a byte of
   its own into c. The two differ by the bank they turn in and the byte
   they load, and by nothing else. */
static const unsigned char slot2_body[] = {
  0x3E, 0x00,             /* ld a,<other bank>                         */
  0x32, 0xFF, 0xFF,       /* ld ($FFFF),a   slot 2 <- the other bank   */
  0x0E, 0x00,             /* ld c,<this bank's byte>                   */
  0xC3, 0x00, 0x80        /* jp $8000                                  */
};

/* Where the body is laid down, and with which two values. */
static void
lay_slot2(unsigned long bank, unsigned char other, unsigned char mark)
{
  unsigned long at = bank * BANK_SIZE;

  memcpy(rom + at,slot2_body,sizeof slot2_body);
  rom[at + 1] = other; /* the bank turned in */
  rom[at + 6] = mark;  /* the byte loaded into c */
}

int
main(int argc, char **argv)
{
  FILE *f;

  if(argc != 2)
    {
      fprintf(stderr,"usage: rom_bank <out.sms>\n");
      return 2;
    }

  fill();
  memcpy(rom,boot,sizeof boot);
  lay_slot2(2UL,0x03,0x11);
  lay_slot2(3UL,0x02,0x22);

  f = fopen(argv[1],"wb");
  if(f == NULL)
    {
      fprintf(stderr,"cannot write %s\n",argv[1]);
      return 2;
    }
  if(fwrite(rom,1,sizeof rom,f) != sizeof rom)
    {
      fprintf(stderr,"cannot write %s whole\n",argv[1]);
      fclose(f);
      return 2;
    }
  if(fclose(f) != 0)
    {
      fprintf(stderr,"cannot close %s\n",argv[1]);
      return 2;
    }
  printf("rom_bank: wrote %s %lu bytes, %lu banks\n",
         argv[1],(unsigned long)ROM_SIZE,(unsigned long)BANKS);
  return 0;
}
