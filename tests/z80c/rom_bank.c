/*
 * The cartridge this repository writes itself: the one program the check
 * of the translated code plays on every checkout, without a real ROM
 * beside it, and the only one whose every wait, write and picture is
 * known to the byte. It is built to make three guards bite -- the epoch
 * of the mapper, the marking of the waits, and the memory held beside
 * the picture -- so that a check of nothing but real ROMs cannot go
 * green with one of them deleted.
 *
 * THE EPOCH. A block renders its linear successor as a table entry
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
 * moves the very window it runs in, in the middle of a chain.
 *
 * THE WAITS. Once a table is armed, a line of the picture is the program
 * run until it waits (src/z80.h, z80_run_events): a halt, or a short
 * loop the translator marked (translate.c, wait_of) -- one that reads a
 * fixed byte of the work RAM, the status port or the line counter, and
 * writes nothing to memory nor to a port. A real ROM's wait can be a
 * halt, which the interpreter keeps whatever the table says; so a program
 * that halts proves nothing about the marking, and the four lines of the
 * core that end a line on a marked wait could be deleted with every real
 * ROM still green. This image never halts: it waits by a loop on a byte
 * of RAM the frame interrupt sets, which the core MUST end the line on
 * (a line that never ends is refused, and on this image a refusal is a
 * failure, tests/z80c/run_z80c.sh). And it carries one loop of the other
 * kind, 128 turns long, that reads a fixed byte of RAM AND writes memory:
 * a fill, not a wait. Marked by mistake it would cost 128 lines, and the
 * write of the backdrop below would land inside the visible lines of the
 * next frame, which the comparison with the classic interpreter sees.
 *
 * THE MEMORY. The byte the program keeps at $C000 is what the mutation
 * "memory" of run_z80c.sh redirects: the picture does not read it back,
 * so only the memory held beside the picture can tell.
 *
 * How it is judged. The translated code is held to the interpreter led
 * by the same clock, frame n against frame n, no shift: the rows of every
 * frame, the colours of every line, and the memory the program keeps at
 * the end of every frame (tests/z80c/sidebyside.c, tests/z80c/
 * colour_tap.c). Beside that verdict, on this image and on it alone, the
 * frames of the classic interpreter -- quotas of T-states, no table --
 * are demanded identical too, frame n against frame n: every write this
 * program makes to the video part falls inside the vertical blank under
 * either clock (the work of a frame is some 5000 T-states, 22 lines of
 * the 70 the blank holds), so the two clocks draw the same frames, unless
 * a wait is marked where the program does not wait.
 *
 * What it shows. One pass of work per frame: the byte it loaded is folded
 * with the bank it has just turned in -- 0 when the byte is that bank's,
 * 1 when the stale successor ran the other bank's code -- and that fold
 * is folded again with bit 0 of a frame counter the interrupt steps, so
 * that the backdrop (register 7: the whole screen while the display is
 * off) alternates black and white frame by frame when the bank is right,
 * and shows the inverted pattern when it is wrong. A picture that stands
 * still, or that changes on the wrong line, is seen against the classic
 * interpreter; the wrong bank is seen against the reference.
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
 *       xor a
 *       ld ($C001),a       the flag the interrupt raises, down
 *       ld ($C002),a       the frame counter, at zero
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
 *       ld a,($C002)
 *       inc a
 *       ld ($C002),a       one more frame
 *       ld a,$01
 *       ld ($C001),a       the flag up: the loop below sees it
 *       ei
 *       reti
 *
 *   bank 2, at $8000 -- reached with slot 2 holding bank 2:
 *       ld a,$03
 *       ld ($FFFF),a       slot 2 <- bank 3: its OWN window, mid-block
 *       --- block ends here, its linear successor rendered as the table
 *           entry of $8005 IN BANK 2, which is no longer what $8005 holds
 *       ld c,$02           bank 2's byte, the wrong one from here on
 *       xor c              the fold: 0 unless the stale successor ran
 *       ld c,a
 *       ld a,($C002)
 *       and $01            bit 0 of the frame counter
 *       xor c              the backdrop of this frame
 *       ld c,a             kept in c across the fill
 *       ld ($C000),a       and kept in RAM: the byte the mutation moves
 *       ld hl,$C100
 *       ld b,$80
 *   fl: ld a,($C000)       the fill: a fixed byte of RAM read, memory
 *       ld (hl),a          written -- not a wait, though it reads one
 *       inc hl
 *       djnz fl            128 turns, about 5000 T-states
 *       ld a,c             the backdrop, from the register
 *       di                 no interrupt between the two writes below:
 *                          the handler's in a,($BF) would reset the
 *                          control port's latch and the second byte
 *                          would be taken as a first
 *       out ($BF),a
 *       ld a,$87
 *       out ($BF),a        register 7 <- a
 *       ei
 *   wait:
 *       ld a,($C001)       the wait: a fixed byte of RAM read, nothing
 *       or a               written, the loop closed on its start,
 *       jr z,wait          three instructions -- the shape the
 *                          translator marks
 *       xor a
 *       ld ($C001),a       the flag down for the next frame
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
 * What the checks see. With the epoch stepped, the chain drops the
 * rendered successor, the live page table is asked, and the interpreter
 * takes over at bank 3's $8005 -- the walk never reaches bank 3, since an
 * address in slot 2 is planned into bank 2, but the run records the
 * positions it interpreted and the next walk starts from them (the
 * seeds), so bank 3 is translated from the second round on -- loads $03
 * and folds it with $03: the fold is 0, the backdrop alternates with the
 * counter. With the increment deleted, the chain walks into bank 2's own
 * $8005 block, loads $02 and folds it with $03: the fold is 1, the
 * pattern is inverted from frame 0, and the byte at $C000 with it. With
 * the wait unmarked, the loop at wait never ends its line and the
 * program is refused. With the fill marked, every turn of it ends a
 * line, the backdrop is written 128 lines late, inside the picture, and
 * the classic interpreter's frames no longer match. With the write of
 * $C000 moved, the picture stands and the memory does not.
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
  0xAF,                   /* xor a                                     */
  0x32, 0x01, 0xC0,       /* ld ($C001),a   the flag down              */
  0x32, 0x02, 0xC0,       /* ld ($C002),a   the frame counter at zero  */
  0x3E, 0x02,             /* ld a,$02                                  */
  0x32, 0xFF, 0xFF,       /* ld ($FFFF),a   slot 2 <- bank 2           */
  0xB7,                   /* or a           a is 2: not zero           */
  0xCA, 0x05, 0x80,       /* jp z,$8005     walked, never taken        */
  0xFB,                   /* ei                                        */
  0xC3, 0x00, 0x80        /* jp $8000                                  */
};

/* The frame interrupt, at its vector in the same kilobyte: the request
   acknowledged, the frame counted, the flag the wait loop reads raised.
   It clobbers a, which nothing holds across the wait. */
#define IRQ_VECTOR 0x38UL
static const unsigned char irq[] = {
  0xDB, 0xBF,             /* in a,($BF)     the request acknowledged   */
  0x3A, 0x02, 0xC0,       /* ld a,($C002)                              */
  0x3C,                   /* inc a                                     */
  0x32, 0x02, 0xC0,       /* ld ($C002),a   one more frame             */
  0x3E, 0x01,             /* ld a,$01                                  */
  0x32, 0x01, 0xC0,       /* ld ($C001),a   the flag up                */
  0xFB,                   /* ei                                        */
  0xED, 0x4D              /* reti                                      */
};

/* The body of a bank of slot 2: turn the other bank in, then a byte of
   its own into c, folded with the bank just turned in and with the frame
   counter, kept in the work RAM, copied 128 times by the fill, shown as
   the backdrop, and the wait for the next frame. The two banks differ by
   the bank they turn in and the byte they load, and by nothing else.
   The displacements: djnz at $801D closes on fl at $8018 ($8018 - $801F
   = -7, $F9); jr z at $802C closes on wait at $8028 ($8028 - $802E = -6,
   $FA). */
static const unsigned char slot2_body[] = {
  0x3E, 0x00,             /* $8000 ld a,<other bank>                   */
  0x32, 0xFF, 0xFF,       /* $8002 ld ($FFFF),a   slot 2 <- the other  */
  0x0E, 0x00,             /* $8005 ld c,<this bank's byte>             */
  0xA9,                   /* $8007 xor c          the fold             */
  0x4F,                   /* $8008 ld c,a                              */
  0x3A, 0x02, 0xC0,       /* $8009 ld a,($C002)   the frame counter    */
  0xE6, 0x01,             /* $800C and $01                             */
  0xA9,                   /* $800E xor c          the backdrop         */
  0x4F,                   /* $800F ld c,a         kept across the fill */
  0x32, 0x00, 0xC0,       /* $8010 ld ($C000),a   kept in RAM          */
  0x21, 0x00, 0xC1,       /* $8013 ld hl,$C100                         */
  0x06, 0x80,             /* $8016 ld b,$80                            */
  0x3A, 0x00, 0xC0,       /* $8018 fl: ld a,($C000)                    */
  0x77,                   /* $801B ld (hl),a      memory written       */
  0x23,                   /* $801C inc hl                              */
  0x10, 0xF9,             /* $801D djnz fl        128 turns            */
  0x79,                   /* $801F ld a,c         the backdrop         */
  0xF3,                   /* $8020 di             the pair uninterrupted */
  0xD3, 0xBF,             /* $8021 out ($BF),a                         */
  0x3E, 0x87,             /* $8023 ld a,$87                            */
  0xD3, 0xBF,             /* $8025 out ($BF),a    register 7 <- a      */
  0xFB,                   /* $8027 ei                                  */
  0x3A, 0x01, 0xC0,       /* $8028 wait: ld a,($C001)                  */
  0xB7,                   /* $802B or a                                */
  0x28, 0xFA,             /* $802C jr z,wait      the wait             */
  0xAF,                   /* $802E xor a                               */
  0x32, 0x01, 0xC0,       /* $802F ld ($C001),a   the flag down        */
  0xC3, 0x00, 0x80        /* $8032 jp $8000                            */
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
