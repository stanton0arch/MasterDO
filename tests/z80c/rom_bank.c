/*
 * The cartridge this repository writes itself: the one program the check
 * of the translated code plays on every checkout, without a real ROM
 * beside it, and the only one whose every wait, write and picture is
 * known to the byte. It is built to make the guards bite -- the epoch
 * of the mapper, the marking of the waits, the memory held beside the
 * picture, and the direct path to the work RAM -- so that a check of
 * nothing but real ROMs cannot go green with one of them deleted.
 *
 * THE EPOCH. A block renders its linear successor as a table entry
 * whenever the next instruction is in the same 16k bank
 * (tests/z80c/translate.c, succ_expr): the linear case carries no window
 * test at all, because a position in the block's own bank sits at the
 * same offset whatever slot that bank is turned into. A block that
 * writes the mapper's registers closes on the write (the tool proves
 * the address), and renders that successor like any other. The only
 * thing standing between it and the bytes the mapper has just put in
 * its place is the epoch: src/cart.c steps z80c_map_epoch on every bank
 * change and src/z80c.c, z80c_run, drops the successor and asks the
 * live page table again when the epoch has moved. Real cartridge code
 * never pulls that rug from under itself -- a program that changes the
 * bank it is executing in would have to have identical bytes at the
 * address in both banks -- so on a real ROM the increment can be
 * deleted and the check stays green: the guard is never seen to bite.
 * This image moves the very window it runs in, in the middle of a
 * chain.
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
 * THE DIRECT PATH. The translator proves which accesses land in the
 * work RAM and emits them without the page tables (translate.c, the
 * direct path; src/z80_ops.h, the Z80_RAM_ and Z80_STK_ forms). This
 * image walks every form the translator can emit, and the edges it
 * must keep off the direct path, so that run_z80c.sh can hold the
 * translator's report of it to exact figures and the emitted C to a
 * floor per form (a proof that goes silent -- everything emitted on
 * the full path -- draws the same frames), and gives the mutations of
 * run_z80c.sh a form to break:
 *
 *   - the stack: SP is set once by LD SP,nn into the RAM, so the stack
 *     is proved; a PUSH BC before the fill and a POP BC after it carry
 *     the backdrop across it, and the mutation "stack" (the order of
 *     the two bytes of the direct POP, broken in the core's header)
 *     shows the wrong byte as the backdrop; PUSH HL / EX (SP),HL /
 *     POP HL then LD ($C00C),HL keep the exchanged word where the
 *     memory digest reads it; CALL sub with RET NZ and RET walk the
 *     call, the taken conditional return and the plain one;
 *   - a read of the ROM by absolute address: the mask of the frame
 *     counter is a data byte after the code, read by LD A,(nn), and the
 *     mutation "direct" (every absolute read of the ROM turned into the
 *     RAM form) reads an empty byte of RAM instead, so the counter no
 *     longer shows;
 *   - a write through a pair the tool knows, three ways: LD HL,$C004
 *     then LD (HL),D; INC HL then LD (HL),D (the pair stepped); LD L,n
 *     then LD (HL),D (a half set); and through an index it knows, LD
 *     IX,$C00A then LD (IX+1),A;
 *   - a pair the tool must FORGET: LD DE,$8000 then EX DE,HL, then LD
 *     (HL),D -- a write to the ROM, absorbed by the interpreter. A tool
 *     that kept HL known would emit it direct into the RAM, and the
 *     memory digest would differ;
 *   - the direct words: LD ($C008),HL then LD HL,($C008), both bytes in
 *     the RAM below the seam;
 *   - the seam of the mirror: LD HL,($DFFF) reads $DFFF and $E000, the
 *     latter being $C000 seen again -- two pages, kept on the full
 *     path; the word lands in HL and stays there through the wait, in
 *     the memory held beside the picture;
 *   - the write to the mapper: LD ($FFFF),A is proved to land on a
 *     register, keeps the full path with its trigger, and closes the
 *     block; its entry carries the flag, and the mutation "bankend"
 *     (the flag cleared) gets the program refused on the PC, where a
 *     block that sees the epoch move its own bytes without the flag is
 *     refused.
 *
 * THE VARIANTS. Two more images, each held to one refusal of the PC and
 * to nothing else (run_z80c.sh):
 *
 *   - "indirect" as a second argument: the mapper is written through HL
 *     loaded in one block and dereferenced in the next (LD HL,$FFFF;
 *     JP next; next: LD (HL),A): the tool cannot prove the address, the
 *     block goes on after the write on the bytes of the bank that left,
 *     and the PC refuses the program ("bank switch inside block");
 *
 *   - "underflow": the boot sets SP to $C001 instead of $DFF0 -- inside
 *     the work RAM, so the stack is still proved -- and the first PUSH
 *     BC of the body puts its low byte at $BFFF, under the RAM: the
 *     check the host runners make on every direct stack access refuses
 *     the program ("stack outside ram"). The console makes no such
 *     check; this image shows the PC does.
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
 *                          mapper's registers at the top of the space:
 *                          the one write of SP, which proves the stack
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
 *       ld ($FFFF),a       slot 2 <- bank 2, the bank already there:
 *                          the block closes on it all the same
 *       or a               a is 2, so the test below is not taken
 *       jp z,$8005         never taken at run time; walked, so that
 *                          $8005 of bank 2 is a start whatever the
 *                          layout
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
 *       ld ($FFFF),a       slot 2 <- bank 3: its OWN window; the block
 *                          closes here, its linear successor rendered
 *                          as the table entry of the next byte IN BANK
 *                          2, which is no longer what the address
 *                          holds -- the epoch drops it
 *       ld c,$02           bank 2's byte, the wrong one from here on
 *       xor c              the fold: 0 unless the stale successor ran
 *       ld c,a
 *       ld a,(mask)        the mask, a byte of ROM after the code: $01
 *       ld d,a
 *       ld hl,$C004
 *       ld (hl),d          a write through a pair the tool knows
 *       inc hl
 *       ld (hl),d          $C005: the pair stepped
 *       ld l,$06
 *       ld (hl),d          $C006: a half set
 *       ld ($C008),hl      a direct word written: $06 $C0
 *       ld hl,($C008)      and read back
 *       ld ix,$C00A
 *       ld (ix+1),a        $C00B: an index the tool knows
 *       ld de,$8000
 *       ex de,hl           hl is $8000 now, and the tool must know it
 *                          knows nothing
 *       ld (hl),d          a write to the ROM: absorbed, full path
 *       ex de,hl           hl back to $C006
 *       ld a,($C002)
 *       and d              bit 0 of the frame counter
 *       xor c              the backdrop of this frame
 *       ld c,a             kept in c across the fill
 *       ld ($C000),a       and kept in RAM: the byte the mutation moves
 *       ld b,$80
 *       push bc            the stack, proved
 *       ld hl,$C100
 *   fl: ld a,($C000)       the fill: a fixed byte of RAM read, memory
 *       ld (hl),a          written -- not a wait, though it reads one;
 *       inc hl             hl is not known here: fl starts a block
 *       djnz fl            128 turns, about 5000 T-states
 *       pop bc             b and c back
 *       push hl            hl is $C180 after the fill
 *       ex (sp),hl         the same word, through the stack
 *       pop hl
 *       ld ($C00C),hl      kept where the memory digest reads it
 *       ld hl,($DFFF)      the seam of the mirror: the full path
 *       ld a,c             the backdrop, from the register
 *       call sub           $C00E <- the backdrop; ret nz on the odd
 *                          frames, ret on the even ones
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
 *   mask:
 *       db $01
 *   sub: ld ($C00E),a
 *       or a
 *       ret nz
 *       ret
 *
 *   bank 3, at $8000 -- the same, the other way round, so the two banks
 *     trade places on every frame and the guard is asked on every other:
 *       ld a,$02
 *       ld ($FFFF),a       slot 2 <- bank 2
 *       ld c,$03           bank 3's byte
 *       xor c
 *       ... as above
 *
 *   the variant: in both banks the mapper write is
 *       ld hl,$FFFF
 *       jp next            the block ends: what hl holds is lost
 *   next:
 *       ld (hl),a          the register written through a pointer
 *
 * What the checks see. With the epoch stepped, the chain drops the
 * rendered successor, the live page table is asked, and the interpreter
 * takes over at bank 3's second instruction -- the walk never reaches
 * bank 3, since an address in slot 2 is planned into bank 2, but the run
 * records the positions it interpreted and the next walk starts from
 * them (the seeds), so bank 3 is translated from the second round on --
 * loads $03 and folds it with $03: the fold is 0, the backdrop
 * alternates with the counter. With the increment deleted, the chain
 * walks into bank 2's own block, loads $02 and folds it with $03: the
 * fold is 1, the pattern is inverted from frame 0, and the byte at $C000
 * with it. With the wait unmarked, the loop at wait never ends its line
 * and the program is refused. With the fill marked, every turn of it
 * ends a line, the backdrop is written 128 lines late, inside the
 * picture, and the classic interpreter's frames no longer match. With
 * the write of $C000 moved, the picture stands and the memory does not.
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
 *   rom_bank <out.sms>            the cartridge
 *   rom_bank <out.sms> indirect   the mapper written through a pointer,
 *                                 refused on the PC
 *   rom_bank <out.sms> underflow  the stack set at $C001, refused on
 *                                 the PC at its first push
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

/* The emitter of a bank's body: a cursor, bytes, three byte forms with
   a sixteen bit operand, and the displacement of a relative branch
   from the byte after its operand to a label. */
static unsigned long at;

static void
put(unsigned char b)
{
  rom[at++] = b;
}

static void
put3(unsigned char op, unsigned long nn)
{
  put(op);
  put((unsigned char)(nn & 0xFFUL));
  put((unsigned char)((nn >> 8) & 0xFFUL));
}

/* The operand of a relative branch whose opcode has just been put: the
   target minus the address of the next instruction. */
static void
put_rel(unsigned long target)
{
  long d = (long)target - (long)(at + 1UL);

  put((unsigned char)(d & 0xFFL));
}

/* The body of a bank of slot 2 (the listing above): turn the other bank
   in -- by the proved absolute write, or through a pointer for the
   variant -- then a byte of its own into c, folded with the bank just
   turned in and with the frame counter, kept in the work RAM, carried
   across the fill on the stack, shown as the backdrop, and the wait
   for the next frame. The two banks differ by the bank they turn in
   and the byte they load, and by nothing else. */
static void
lay_slot2(unsigned long bank, unsigned char other, unsigned char mark, int indirect)
{
  unsigned long base = bank * BANK_SIZE;
  unsigned long maskref, subref, fl, wait, mask, sub;

  at = base;
  put(0x3E); put(other);                      /* ld a,other            */
  if(!indirect)
    put3(0x32,0xFFFFUL);                      /* ld ($FFFF),a          */
  else
    {
      put3(0x21,0xFFFFUL);                    /* ld hl,$FFFF           */
      put3(0xC3,0x8000UL + (at + 3UL - base)); /* jp next              */
      put(0x77);                              /* next: ld (hl),a       */
    }
  put(0x0E); put(mark);                       /* ld c,mark             */
  put(0xA9);                                  /* xor c                 */
  put(0x4F);                                  /* ld c,a                */
  maskref = at;
  put3(0x3A,0UL);                             /* ld a,(mask), patched  */
  put(0x57);                                  /* ld d,a                */
  put3(0x21,0xC004UL);                        /* ld hl,$C004           */
  put(0x72);                                  /* ld (hl),d             */
  put(0x23);                                  /* inc hl                */
  put(0x72);                                  /* ld (hl),d      $C005  */
  put(0x2E); put(0x06);                       /* ld l,$06              */
  put(0x72);                                  /* ld (hl),d      $C006  */
  put3(0x22,0xC008UL);                        /* ld ($C008),hl         */
  put3(0x2A,0xC008UL);                        /* ld hl,($C008)         */
  put(0xDD); put3(0x21,0xC00AUL);             /* ld ix,$C00A           */
  put(0xDD); put(0x77); put(0x01);            /* ld (ix+1),a    $C00B  */
  put3(0x11,0x8000UL);                        /* ld de,$8000           */
  put(0xEB);                                  /* ex de,hl              */
  put(0x72);                                  /* ld (hl),d      $8000  */
  put(0xEB);                                  /* ex de,hl              */
  put3(0x3A,0xC002UL);                        /* ld a,($C002)          */
  put(0xA2);                                  /* and d                 */
  put(0xA9);                                  /* xor c                 */
  put(0x4F);                                  /* ld c,a                */
  put3(0x32,0xC000UL);                        /* ld ($C000),a          */
  put(0x06); put(0x80);                       /* ld b,$80              */
  put(0xC5);                                  /* push bc               */
  put3(0x21,0xC100UL);                        /* ld hl,$C100           */
  fl = at;
  put3(0x3A,0xC000UL);                        /* fl: ld a,($C000)      */
  put(0x77);                                  /* ld (hl),a             */
  put(0x23);                                  /* inc hl                */
  put(0x10); put_rel(fl);                     /* djnz fl               */
  put(0xC1);                                  /* pop bc                */
  put(0xE5);                                  /* push hl               */
  put(0xE3);                                  /* ex (sp),hl            */
  put(0xE1);                                  /* pop hl                */
  put3(0x22,0xC00CUL);                        /* ld ($C00C),hl         */
  put3(0x2A,0xDFFFUL);                        /* ld hl,($DFFF)         */
  put(0x79);                                  /* ld a,c                */
  subref = at;
  put3(0xCD,0UL);                             /* call sub, patched     */
  put(0xF3);                                  /* di                    */
  put(0xD3); put(0xBF);                       /* out ($BF),a           */
  put(0x3E); put(0x87);                       /* ld a,$87              */
  put(0xD3); put(0xBF);                       /* out ($BF),a           */
  put(0xFB);                                  /* ei                    */
  wait = at;
  put3(0x3A,0xC001UL);                        /* wait: ld a,($C001)    */
  put(0xB7);                                  /* or a                  */
  put(0x28); put_rel(wait);                   /* jr z,wait             */
  put(0xAF);                                  /* xor a                 */
  put3(0x32,0xC001UL);                        /* ld ($C001),a          */
  put3(0xC3,0x8000UL);                        /* jp $8000              */
  mask = at;
  put(0x01);                                  /* mask: db $01          */
  sub = at;
  put3(0x32,0xC00EUL);                        /* sub: ld ($C00E),a     */
  put(0xB7);                                  /* or a                  */
  put(0xC0);                                  /* ret nz                */
  put(0xC9);                                  /* ret                   */

  /* The mask's and the subroutine's addresses, as the code sees them
     in slot 2. */
  rom[maskref + 1] = (unsigned char)((0x8000UL + (mask - base)) & 0xFFUL);
  rom[maskref + 2] = (unsigned char)(((0x8000UL + (mask - base)) >> 8) & 0xFFUL);
  rom[subref + 1] = (unsigned char)((0x8000UL + (sub - base)) & 0xFFUL);
  rom[subref + 2] = (unsigned char)(((0x8000UL + (sub - base)) >> 8) & 0xFFUL);
}

int
main(int argc, char **argv)
{
  FILE *f;
  int indirect = 0;
  int underflow = 0;

  if(argc == 3 && strcmp(argv[2],"indirect") == 0)
    indirect = 1;
  else if(argc == 3 && strcmp(argv[2],"underflow") == 0)
    underflow = 1;
  else if(argc != 2)
    {
      fprintf(stderr,"usage: rom_bank <out.sms> [indirect|underflow]\n");
      return 2;
    }

  fill();
  memcpy(rom,boot,sizeof boot);
  memcpy(rom + IRQ_VECTOR,irq,sizeof irq);
  /* The stack at $C001 for the underflow variant: the operand of the
     ld sp,nn at the second byte of the boot, still in the work RAM. */
  if(underflow)
    {
      rom[2] = 0x01;
      rom[3] = 0xC0;
    }
  /* Each bank's byte is its own number: the byte loaded after bank n is
     turned in is n, and folded with n it is 0. */
  lay_slot2(2UL,0x03,0x02,indirect);
  lay_slot2(3UL,0x02,0x03,indirect);

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
  printf("rom_bank: wrote %s %lu bytes, %lu banks%s\n",
         argv[1],(unsigned long)ROM_SIZE,(unsigned long)BANKS,
         indirect ? ", the mapper written through a pointer" :
         underflow ? ", the stack set at $C001" : "");
  return 0;
}
