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
 * address in both banks -- so on a real ROM the increment can be deleted
 * and the check stays green: the guard is never seen to bite. This image
 * makes it bite.
 *
 * How it is judged. The check compares the pictures of every frame, the
 * two runs going free, with one frame of shift allowed (tests/z80c/
 * sidebyside.c): neither the memory nor the instruction a frame ends on
 * is compared. So the image behaves as a game does and shows its result
 * on the screen. It does one pass of its work per frame, folds the byte
 * it loaded with the bank it has just turned in, writes that into
 * register 7 -- the backdrop, the whole screen while the display is off --
 * and halts until the frame interrupt. A byte loaded from the right bank
 * folds to 0 on every frame, a black screen; a byte loaded from the wrong
 * bank folds to 1, a white one. Since the right result is the same on
 * every frame, a shift of one frame cannot hide a wrong one.
 *
 * The shape. Four banks of 16k, the smallest power of two that gives the
 * second slot two different banks to hold.
 *
 *   bank 0, at 0x0000 -- the reset vector, inside the fixed first
 *     kilobyte of slot 0, so it renders no successor past it:
 *       di
 *       ld sp,$DFF0        the stack in the work RAM, clear of the
 *                          mapper's registers at the top of the space
 *       im 1
 *       ld a,$20
 *       out ($BF),a
 *       ld a,$81
 *       out ($BF),a        register 1 <- $20: the frame interrupt on,
 *                          the display off
 *       ld a,$10
 *       out ($BF),a
 *       ld a,$C0
 *       out ($BF),a        the colour memory addressed at 16
 *       ld a,$00
 *       out ($BE),a        colour 16 <- black: backdrop 0
 *       ld a,$3F
 *       out ($BE),a        colour 17 <- white: backdrop 1
 *       ld a,$02
 *       ld ($FFFF),a       slot 2 <- bank 2
 *       or a               a is 2, so the test below is not taken
 *       jp z,$8005         never taken at run time; walked, so that
 *                          $8005 of bank 2 is a block start and the
 *                          block below closes on the mapper write
 *       ei
 *       jp $8000
 *
 *   bank 0, at 0x0038 -- the frame interrupt:
 *       in a,($BF)         the request acknowledged
 *       ei
 *       reti
 *
 *   bank 2, at $8000 -- reached with slot 2 holding bank 2:
 *       ld a,$03
 *       ld ($FFFF),a       slot 2 <- bank 3: its OWN window, mid-block
 *       --- block ends here, its linear successor rendered as the table
 *           entry of $8005 IN BANK 2, which is no longer what $8005 holds
 *       ld c,$02           bank 2's byte, the wrong one from here on
 *       xor c
 *       ld ($C000),a
 *       di                 no interrupt between the two writes below:
 *                          the handler's in a,($BF) would reset the
 *                          control port's latch and the second byte
 *                          would be taken as a first
 *       out ($BF),a
 *       ld a,$87
 *       out ($BF),a        register 7 <- a
 *       ei
 *       halt               until the next frame
 *       jp $8000
 *
 *   bank 3, at $8000 -- the same, the other way round, so the two banks
 *     trade places on every frame and the guard is asked on every other:
 *       ld a,$02
 *       ld ($FFFF),a       slot 2 <- bank 2
 *       ld c,$03           bank 3's byte
 *       xor c
 *       ... as above
 *
 * What the check sees. With the epoch stepped, the chain drops the
 * rendered successor, the live page table is asked, nothing is written
 * at bank 3's $8005 -- the walk never reaches bank 3, since an address in
 * slot 2 is planned into bank 2 -- and the interpreter takes over, loads
 * $03 and folds it with $03: the backdrop stays 0. With the increment
 * deleted, the chain walks into bank 2's own $8005 block, loads $02 and
 * folds it with $03: the backdrop is 1 on every other frame, and no frame
 * of the interpreter's is ever white.
 *
 * Everything not written here is $18 $FE, jr to itself: the non-maskable
 * vector the walk starts from besides the two above holds it, so
 * discovery stops on it at once and goes no further, and a jump into any
 * byte this file does not fill spins on the spot instead of running the
 * filler as code. The two fillers one would reach for first both walk the
 * whole image instead: halt is an instruction the interpreter keeps,
 * which makes the byte after it a start, and rst $00 is a call, whose
 * return address is a start too -- either one cascades from vector to
 * vector through every byte of the file.
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
  0x31, 0xF0, 0xDF,       /* ld sp,$DFF0                               */
  0xED, 0x56,             /* im 1                                      */
  0x3E, 0x20,             /* ld a,$20                                  */
  0xD3, 0xBF,             /* out ($BF),a                               */
  0x3E, 0x81,             /* ld a,$81                                  */
  0xD3, 0xBF,             /* out ($BF),a    register 1 <- $20          */
  0x3E, 0x10,             /* ld a,$10                                  */
  0xD3, 0xBF,             /* out ($BF),a                               */
  0x3E, 0xC0,             /* ld a,$C0                                  */
  0xD3, 0xBF,             /* out ($BF),a    colour memory from $10     */
  0x3E, 0x00,             /* ld a,$00                                  */
  0xD3, 0xBE,             /* out ($BE),a    colour 16 <- black         */
  0x3E, 0x3F,             /* ld a,$3F                                  */
  0xD3, 0xBE,             /* out ($BE),a    colour 17 <- white         */
  0x3E, 0x02,             /* ld a,$02                                  */
  0x32, 0xFF, 0xFF,       /* ld ($FFFF),a   slot 2 <- bank 2           */
  0xB7,                   /* or a           a is 2: not zero           */
  0xCA, 0x05, 0x80,       /* jp z,$8005     walked, never taken        */
  0xFB,                   /* ei                                        */
  0xC3, 0x00, 0x80        /* jp $8000                                  */
};

/* The frame interrupt, at its vector in the same kilobyte. */
#define IRQ_VECTOR 0x38UL
static const unsigned char irq[] = {
  0xDB, 0xBF,             /* in a,($BF)     the request acknowledged   */
  0xFB,                   /* ei                                        */
  0xED, 0x4D              /* reti                                      */
};

/* The body of a bank of slot 2: turn the other bank in, then a byte of
   its own into c, folded with the bank just turned in, kept in the work
   RAM and shown as the backdrop, and wait for the next frame. The two
   differ by the bank they turn in and the byte they load, and by nothing
   else. */
static const unsigned char slot2_body[] = {
  0x3E, 0x00,             /* ld a,<other bank>                         */
  0x32, 0xFF, 0xFF,       /* ld ($FFFF),a   slot 2 <- the other bank   */
  0x0E, 0x00,             /* ld c,<this bank's byte>                   */
  0xA9,                   /* xor c          0 unless c is another bank's */
  0x32, 0x00, 0xC0,       /* ld ($C000),a                              */
  0xF3,                   /* di             the pair below uninterrupted */
  0xD3, 0xBF,             /* out ($BF),a                               */
  0x3E, 0x87,             /* ld a,$87                                  */
  0xD3, 0xBF,             /* out ($BF),a    register 7 <- a: backdrop  */
  0xFB,                   /* ei             taken after the halt below */
  0x76,                   /* halt                                      */
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
  memcpy(rom + IRQ_VECTOR,irq,sizeof irq);
  /* Each bank's byte is its own number: the byte loaded after bank n is
     turned in is n, and folded with n it is 0. */
  lay_slot2(2UL,0x03,0x02);
  lay_slot2(3UL,0x02,0x03);

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
