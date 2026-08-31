#ifndef SMS3DO_VDP_H
#define SMS3DO_VDP_H

#include "common.h"

/*
 * Video display processor: its state, the four ports and two counters the
 * bus routes to it, the line clock the frame loop drives, and the maskable
 * interrupt line the processor samples.
 *
 * Ownership of sms.vdp: this module writes every field of it, and no other
 * module writes one. Two readers exist outside this file. The processor
 * reads frame_pending, line_pending and two registers through
 * VDP_IRQ_LINE below, at the head of every quota, and writes nothing back
 * -- the line belongs to the device, the processor samples it. The frame
 * loop calls vdp_line once per scanline and vdp_report once per emulated
 * second, and reads nothing at all.
 *
 * What this stage does. It answers on the ports, stores every byte a
 * program sends to video memory, colour memory and the registers, counts
 * lines, raises the VBlank and line interrupts and holds the interrupt
 * line until the status register is read. It also renders the background
 * plane of mode 4: once per line of the picture, from the name table, the
 * patterns and the scroll registers the program wrote, into the packed
 * index buffer the coded cel below reads -- one line per call of
 * vdp_line, composed in a one-byte-per-pixel scratch and packed a word at
 * a time. Then the sprites of that line: the attribute table is walked
 * once, eight of its entries at most are kept, and their opaque pixels
 * are laid into the same scratch -- above the background, except where
 * the priority mask the background just wrote says the background wins.
 * The two sprite bits of the status register are raised there and fall on
 * a status read, like the two interrupt requests.
 * Colour memory is converted at the write, never at the render: the
 * palette of the cel is the image of the colour memory, kept in step by
 * the data write macro.
 *
 * Colour memory here is the Master System's for both profiles: 32
 * bytes, one per colour, indexed on five bits. The Game Gear's format --
 * 64 bytes, two writes per colour (TotalSMS/src/core/sms_vdp.c:512-534)
 * -- comes with the Game Gear profile being applied, later; until then
 * the profile is read and named at init and never acted on, and a Game
 * Gear program shows wrong colours through the Master System table --
 * never an error, the index is always masked.
 *
 * This header is a leaf like cart.h: it includes common.h and names sms.vdp
 * inside macros that are expanded later, from files that have sms.h in
 * scope. It includes neither sms.h nor cart.h, so cart.h may include it to
 * route the hooks of the port map without a cycle.
 */

/*
 * ---------------------------------------------------------------------------
 * Sizes, and where each figure comes from.
 *
 * Video memory is 16 kilobytes, addressed on fourteen bits
 * (docs/sms_gg/SMSOfficialDocs.md:579-740 for the ports; the mask is
 * TotalSMS/src/core/sms_vdp.c:479, :615). Colour memory is 32 entries
 * addressed on five bits (SMSOfficialDocs.md:690-740; sms_vdp.c:540). The
 * register file holds eleven registers, 0 to 10; the number is taken on
 * four bits and the five above 10 are ignored (sms_vdp.c:585-592), so
 * sixteen slots are kept for the mask and the top five never written.
 * ---------------------------------------------------------------------------
 */
#define VDP_VRAM_SIZE 16384UL
#define VDP_VRAM_MASK 0x3FFFUL
#define VDP_CRAM_SIZE 32
#define VDP_CRAM_MASK 0x1FUL
#define VDP_REG_COUNT 16
#define VDP_REG_LAST  10

/*
 * ---------------------------------------------------------------------------
 * The raster, in lines. Three figures, kept here under this module's own
 * names because the module reads them and includes nothing that defines
 * them elsewhere.
 *
 * VDP_ACTIVE_LINES is the height of the picture in the one mode this port
 * renders, 192 lines (sms_vdp.c:64-81, the 192 line table; taller modes
 * are counted and refused, see vdp_report). VDP_LINES_PER_FRAME is the
 * number of lines of a sixty hertz frame, 262, the same figure the frame
 * loop cuts its frame into as MAIN_LINES_PER_FRAME (main.c:68, from
 * TotalSMS/src/core/sms.c:44): the loop calls vdp_line that many times per
 * frame and this module wraps its counter on the same number, so the two
 * constants describe one fact from two sides and must stay equal.
 * VDP_VCOUNT_FOLD is where the value the V counter port returns stops
 * following the line number: on the 192 line raster the port counts 0x00
 * to 0xDA, then jumps back to 0xD5 and runs to 0xFF, 262 values in all
 * (sms_vdp.c:64-81, the table; :442-445, the read).
 * ---------------------------------------------------------------------------
 */
#define VDP_ACTIVE_LINES    192UL
#define VDP_LINES_PER_FRAME 262UL
#define VDP_VCOUNT_FOLD     0xDAUL

/*
 * ---------------------------------------------------------------------------
 * The index buffer: the picture as the cel engine reads it. One packed 6
 * bit index per pixel, 256 wide by the 192 lines of the picture, rows
 * rounded up to the word because the engine fetches rows by words and
 * requires each row word aligned and at least two words long
 * (docs/3do/3DO_Development_Notes.md:92-95; 48 words a row satisfies
 * both). Every size below is calculated from width, depth and height --
 * never restated as a figure -- so the day one of the three changes, all
 * of them follow.
 *
 * The palette of the coded cel holds 32 entries of RGB555, one per colour
 * of the emulated palette (include/3do/graphics.h:261 for the packing).
 * ---------------------------------------------------------------------------
 */
#define VDP_PIX_WIDTH  256UL
#define VDP_PIX_BPP    6UL
#define VDP_PIX_ROW_BYTES ((((VDP_PIX_WIDTH * VDP_PIX_BPP) + 31UL) / 32UL) * 4UL)
#define VDP_PIX_BUF_BYTES (VDP_PIX_ROW_BYTES * VDP_ACTIVE_LINES)
#define VDP_PLUT_ENTRIES 32

/*
 * The packer works sixteen pixels at a time -- three words of six bit
 * indexes -- so the width must divide by sixteen for the row to come out
 * whole. Refused at compile time rather than truncated.
 */
#if (VDP_PIX_WIDTH % 16UL) != 0UL
#error "the row packer takes 16 pixels per 3 words: the width must be a multiple of 16"
#endif

/*
 * ---------------------------------------------------------------------------
 * The line scratch: one byte per pixel of the picture, plus a lead of eight
 * bytes before pixel 0 and a tail of sixteen after pixel 255. The
 * background is composed tile by tile, eight pixels a stroke, and a fine
 * horizontal scroll shifts every stroke right by up to seven pixels: the
 * stroke that supplies the leftmost pixels then starts before pixel 0, and
 * the one that ends the row runs past pixel 255. Both are written whole
 * into the lead and the tail rather than tested pixel by pixel -- the lead
 * and the tail are what makes a write outside the picture impossible by
 * construction, with no compare in the loop. Pixel x of the picture is
 * scratch[VDP_LINE_LEAD + x]. The mask scratch has the same shape.
 * ---------------------------------------------------------------------------
 */
#define VDP_LINE_LEAD    8UL
#define VDP_LINE_TAIL    16UL
#define VDP_LINE_SCRATCH (VDP_LINE_LEAD + VDP_PIX_WIDTH + VDP_LINE_TAIL)

/*
 * The bit plane table: for each of the four planes of a pattern row, for
 * each byte value, the eight pixel contributions of that byte -- the bit
 * for pixel x (bit 7 is the left pixel, SMSOfficialDocs.md:505-578)
 * shifted up to the plane's weight. A pixel's index is then the OR of four
 * table bytes and no loop over bits. Sized here, allocated at init.
 */
#define VDP_PLANES_COUNT 4UL
#define VDP_PLANES_BYTES (VDP_PLANES_COUNT * 256UL * 8UL)
#define VDP_PLANES_PLANE (256UL * 8UL)

/*
 * The name table rows the scroll wraps on, and the picture rows: 28 rows
 * of 8 lines, 224 lines, the height of the whole table in the 192 line
 * mode (SMSOfficialDocs.md:870-896; TotalSMS/src/core/sms_vdp.c:803-824).
 */
#define VDP_NT_LINES 224UL

/*
 * ---------------------------------------------------------------------------
 * The sprites. Sixty-four entries, each three bytes spread over two
 * halves of a 256 byte table: the vertical position at base + i, the
 * horizontal position and the pattern number at base + 128 + 2i and the
 * byte after (docs/sms_gg/SMSOfficialDocs.md:401-473). A vertical
 * position of 0xD0 stops the walk on the 192 line raster, and the
 * position on screen is the byte plus one. Eight of them at most are
 * displayed on a line; a ninth raises the overflow bit and is dropped
 * (SMSOfficialDocs.md:363-400; TotalSMS/src/core/sms_vdp.c:1055-1123).
 *
 * A vertical position above VDP_SPR_Y_WRAP belongs to a sprite entering
 * from the top of the picture and counts as a negative line
 * (sms_vdp.c:1090-1093). The figure is 224 like the height of the name
 * table, and for an unrelated reason -- one is where the scroll wraps,
 * the other where a position turns negative -- so it carries its own
 * name: moving one must not move the other.
 * ---------------------------------------------------------------------------
 */
#define VDP_SPR_COUNT        64UL
#define VDP_SPR_MAX_ON_LINE  8UL
#define VDP_SPR_TERMINATOR   0xD0UL
#define VDP_SPR_XN_OFFSET    128UL
#define VDP_SPR_Y_WRAP       224UL

/*
 * The pixel taken scratch is written and read a byte at a time and
 * cleared a word at a time, so it is declared as words: a byte array
 * between other byte arrays of the state carries no alignment the clear
 * could rely on. Two properties have to hold for the two views to be the
 * same object -- the word count times the word size must be the pixel
 * count, and the pixel count must divide by the word size -- and both are
 * refused here rather than assumed.
 */
#if ((VDP_PIX_WIDTH % 4UL) != 0UL)
#error "the taken scratch is cleared a word at a time: the width must be a multiple of 4"
#endif

/*
 * Three more refusals, in the spirit of the preamble guard above: each of
 * the three constants they watch is written into arithmetic that would
 * silently stop holding if the constant moved. The packer and the
 * preamble are written for six bit indexes, three words to sixteen
 * pixels. The vertical position of a line is the line plus a scroll of
 * at most 255, wrapped on the table height by ONE subtraction, which only
 * wraps a sum below twice that height. And a stroke of eight pixels
 * shifted right by up to seven needs eight bytes of lead before pixel 0
 * and fifteen after pixel 255 for its writes to be inside the scratch.
 */
#if VDP_PIX_BPP != 6UL
#error "the row packer and the cel preamble are written for 6 bit indexes"
#endif

#if ((VDP_ACTIVE_LINES - 1UL) + 255UL) >= (2UL * VDP_NT_LINES)
#error "one subtraction no longer wraps the vertical position on the name table"
#endif

#if (VDP_LINE_LEAD < 8UL) || (VDP_LINE_TAIL < 15UL)
#error "a stroke shifted by 7 needs 8 bytes of lead and 15 of tail in the line scratch"
#endif

/*
 * The preamble fields of the cel are counted from these constants, and
 * their ranges are hardware facts: a row must be at least two words for
 * the engine's pipelined fetch, the pixel count field holds eleven bits,
 * and the row offset of a depth of eight bits or less is read from a
 * field EIGHT bits wide. A change of constant that breaks any of the
 * three would compile into a cel the engine misreads with no error
 * anywhere -- the row offset silently truncated is what shears a picture
 * into diagonals -- so all three are refused here.
 */
#if ((VDP_PIX_ROW_BYTES / 4UL) < 2UL) || ((VDP_PIX_WIDTH - 1UL) > 0x7FFUL) \
 || (((VDP_PIX_ROW_BYTES / 4UL) - 2UL) > 0xFFUL)
#error "cel preamble out of range: a row needs 2 words minimum, the pixel count fits 11 bits, the row offset fits 8"
#endif

/*
 * The four codes of the second control byte, bits 7 and 6
 * (SMSOfficialDocs.md:579-740; sms_vdp.c:644-667).
 */
#define VDP_CODE_VRAM_READ  0UL
#define VDP_CODE_VRAM_WRITE 1UL
#define VDP_CODE_REG_WRITE  2UL
#define VDP_CODE_CRAM_WRITE 3UL

/*
 * The counters exist for the periodic line alone and are kept only when it
 * is: the condition of LOG_HOT itself (log.h:151). The data write sits on
 * the processor's port path, the hottest place a counter could be, so the
 * guard is the one the measurement build turns off, not LOG_ENABLE alone --
 * a figure read off that build must not include the counting.
 */
#if LOG_ENABLE && SMS_TELEMETRY
#define VDP_COUNTERS 1
#else
#define VDP_COUNTERS 0
#endif

/*
 * The state. Fields the ports touch on every access are held in words
 * rather than bytes: a byte field between words costs padding, and a word
 * compare is what the compiler emits either way (precedent: the memory
 * control register, cart.h). The inventory of the state -- address, code,
 * memories, registers, line counter, read buffer, control word and latch,
 * the two pending flags -- is TotalSMS/src/core/sms_types.h:282-347; the
 * form is this port's.
 */
typedef struct
{
  /*
   * Video memory, one block taken through the allocator of sys.c at init
   * so that it shows in the boot footprint like every other block. The
   * address that indexes it is kept masked on every path that moves it.
   */
  uint8 *vram;

  /*
   * Colour memory, 32 bytes, stored as written and converted at the write
   * into the palette below. Nothing reads it per pixel or per line: the
   * palette is its image, and the cel engine reads the palette.
   */
  uint8 cram[VDP_CRAM_SIZE];

  /*
   * The registers, stored as written. Read here: bit 4 of register 0 and
   * bit 5 of register 1 (interrupt enables), bits 2 and 1 of register 0
   * with bits 4 and 3 of register 1 (the mode and the height), register
   * 10 (the line counter reload), and by the background render bits 7, 6
   * and 5 of register 0, bit 6 of register 1, registers 2, 7, 8 and 9.
   */
  uint8 reg[VDP_REG_COUNT];

  /*
   * The two scroll latches the render reads instead of the registers.
   * The vertical one is register 9 as it stood when the frame wrapped:
   * the document latches it in vertical blanking (SMSOfficialDocs.md:895)
   * and a write during the picture is seen a frame later. The horizontal
   * one is register 8 as it stood at the end of the previous line: the
   * value becomes effective one line late (docs/sms_gg/GGOfficialDocs.md:
   * 1438), so a write during line y moves line y + 1.
   */
  uint32 vscroll;
  uint32 hscroll;

  /* Address, code and the two halves of the control sequence. */
  uint32 addr;
  uint32 code;
  uint32 ctrl_word;
  uint32 latch;

  /* The read buffer: what a data read returns, filled one access ahead. */
  uint32 read_buf;

  /* The line count, 0 to 261, and the line interrupt counter. */
  uint32 vcount;
  uint32 line_ctr;

  /*
   * The two interrupt requests. Each rises on its own event and both fall
   * on a status read; the maskable line is their conjunction with the two
   * enable bits (VDP_IRQ_LINE), so a status read drops the line without
   * anything else being told.
   */
  uint32 frame_pending;
  uint32 line_pending;

  /*
   * The palette of the coded cel, RGB555, owned here and written by one
   * pen only: the conversion of the emulated colour memory, at init over
   * the zeroed colour memory and then at every colour write through the
   * data write macro below -- and nothing else, ever. The cel engine
   * reads it through the cel's palette pointer, loaded on every draw.
   */
  uint16 plut[VDP_PLUT_ENTRIES];

  /*
   * The composition scratch of one line, one index per pixel, and the
   * mask that goes with it: 1 where the background pixel is priority and
   * non-zero, or where the left column is masked; 0 elsewhere. The line
   * is packed from the scratch once composed; the mask is written here
   * for the sprite stage and read by nothing yet. Shape and offsets:
   * VDP_LINE_* above.
   */
  uint8 line[VDP_LINE_SCRATCH];
  uint8 prio[VDP_LINE_SCRATCH];

  /*
   * The two sprite bits of the status register. Each rises while a line
   * is composed -- the overflow when a ninth sprite falls on it, the
   * collision when two opaque sprite pixels land on the same place -- and
   * both fall on a status read, with the two interrupt requests and
   * nowhere else.
   */
  uint32 spr_overflow;
  uint32 spr_collision;

  /*
   * The sprites kept for the line being composed: how many, which entry
   * of the attribute table each one is, and the screen line each one
   * starts on. That last one is signed: a vertical position above the
   * table height belongs to a sprite entering from the top, so it counts
   * as a negative line (TotalSMS/src/core/sms_vdp.c:1090-1093).
   */
  uint32 spr_count;
  uint8 spr_sel[VDP_SPR_MAX_ON_LINE];
  int32 spr_top[VDP_SPR_MAX_ON_LINE];

  /*
   * One flag per pixel of the line: set when a sprite has already taken
   * that pixel. It is what makes the first sprite of the table win and
   * what a collision is read off -- a second opaque pixel on a taken
   * place, whether or not it ends up drawn. Cleared only on the lines
   * that have sprites at all.
   */
  uint32 spr_taken[VDP_PIX_WIDTH / 4UL];

  /* The bit plane table, VDP_PLANES_BYTES, allocated at init. */
  uint8 *planes;

  /*
   * The index buffers, one frame of packed 6 bit indexes each, allocated
   * in DRAM at init (DRAM is the preferred source for cel data,
   * docs/3do/3DO_Development_Notes.md:38). The count is a build constant
   * (common.h, SMS_VDP_BUFFERS) and the measurements have been taken: it
   * is one, and the render indexes buffer zero without rotating. The
   * useful double buffering is the screen's, which the system part now
   * turns on every presentation, so the cel is never read out of a
   * picture being composed. The array stays an array so that the day a
   * measured tear reopens the question, the shape it would need is
   * already there.
   */
  uint8 *pixels[SMS_VDP_BUFFERS];

  /*
   * The cel control block the frame loop hands to the draw call, kept as
   * an opaque pointer: this header is a leaf that includes common.h only,
   * so the concrete type is named where the block is built and where it
   * is drawn, never here. Read through vdp_cel below, once, at boot.
   */
  void *cel;

#if VDP_COUNTERS
  /*
   * The aggregates the periodic line reports, cleared by it. The figures
   * feed a line and nothing else, hence the guard: a silent or a measured
   * build counts nothing.
   */
  uint32 cnt_reg_w;
  uint32 cnt_vram_w;
  uint32 cnt_cram_w;
  uint32 cnt_status_r;
  uint32 cnt_data_r;
  uint32 cnt_vcnt_r;
  uint32 cnt_hcnt_r;
  uint32 cnt_reg_oob;
  uint32 cnt_mode;
  /*
   * The four mode bits of the last unsupported request, as the warning
   * prints them: bit 3 is register 0 bit 2 (M4), bit 2 is register 0
   * bit 1 (M2), bit 1 is register 1 bit 3 (M3), bit 0 is register 1 bit 4
   * (M1). Mode 4 is never reported here, whatever its height: a taller
   * mode 4 picture is counted apart, below.
   */
  uint32 mode_last;
  /*
   * Mode 4 asked at 224 or 240 lines: counted on its own, the height last
   * asked kept for the warning, and the picture rendered at 192 lines
   * regardless. Never an error, never fatal.
   */
  uint32 cnt_height;
  uint32 height_last;
  /*
   * The sprites of the window: the largest number kept on one line, how
   * many LINES of it overflowed and how many had a collision, and whether
   * magnification was on for any line composed in it. Lines and not
   * events, on purpose twice over: a scrum of sprites would otherwise publish a
   * collision per pixel, and counting only the rise of the status bit
   * would publish nothing at all for a program that never reads the
   * status -- which is the program the line exists to describe.
   */
  uint32 cnt_spr_max;
  uint32 cnt_spr_ovf;
  uint32 cnt_spr_col;
  uint32 cnt_spr_zoom;
  /*
   * How many times the surround of the picture was repainted in the
   * window, as the frame loop reported it through vdp_backdrop_repainted.
   * A figure of zero is the nominal one -- a program that leaves its
   * background colour alone pays no paint at all -- and a figure that
   * grows with the frames names a program writing register 7 or the
   * colour entry it points at over and over.
   */
  uint32 cnt_backdrop;
#endif
} vdp_t;

#if VDP_COUNTERS
#define VDP_COUNT(name) (sms.vdp.cnt_##name++)
#else
#define VDP_COUNT(name) ((void)0)
#endif

/*
 * ---------------------------------------------------------------------------
 * The render broken down into posts, by repetition (common.h,
 * SMS_VDP_PROFILE).
 *
 * Derived exactly like VDP_COUNTERS above, and on one condition more: the
 * breakdown is read off the displacement of the periodic line, so it
 * exists only where that line does.
 * ---------------------------------------------------------------------------
 */
#if LOG_ENABLE && SMS_TELEMETRY && SMS_VDP_PROFILE
#define VDP_PROFILE 1
#else
#define VDP_PROFILE 0
#endif

/*
 * The three repeatable posts of one rendered line, and the five variants
 * the selector steps through. A variant number is the post it repeats,
 * which is what lets the repetition count be one compare: the control
 * repeats nothing, VDP_PROFILE_ALL repeats the three together.
 *
 * The posts are the three that are idempotent: run twice they write the
 * same bytes and leave the same emulated state. The rest of the line --
 * what vdp_line does around the render, the scanline counter, the pending
 * flags, the scroll latches -- is not among them and cannot be: repeating
 * it would advance the raster twice. Its cost is a residual, obtained by
 * subtracting the three from the published figure, and it is named as one
 * wherever it is printed.
 *
 * The blank branch of the render -- register 1 bit 6 clear, the picture
 * off -- carries no post either: it fills the row and returns before any
 * of the three. Lines rendered that way pull every displacement down, so
 * the reference regime is a regime with the picture on.
 */
#define VDP_POST_BG      1UL
#define VDP_POST_SPRITES 2UL
#define VDP_POST_PACK    3UL

#define VDP_PROFILE_CONTROL  0UL
#define VDP_PROFILE_BG       VDP_POST_BG
#define VDP_PROFILE_SPRITES  VDP_POST_SPRITES
#define VDP_PROFILE_PACK     VDP_POST_PACK
#define VDP_PROFILE_ALL      4UL
#define VDP_PROFILE_VARIANTS 5UL

#if VDP_PROFILE
/*
 * Arms a variant for the windows to come. The frame loop owns the cadence
 * and calls this, because the cutting up of a turn belongs to the loop and
 * to it alone; this module obeys a selector and reads no clock of its own.
 * A number past the last variant arms the control rather than trusting it.
 */
void vdp_profile_select(uint32 variant);

/*
 * How many times the named post runs on this line under the armed
 * variant: two when the variant repeats it, one otherwise.
 */
uint32 vdp_profile_reps(uint32 post);
#endif

/*
 * The wrapper a repeatable post is written inside. With the switch off it
 * is a bare do/while(0) around the post -- a compound statement run once,
 * with a condition the preprocessor has already made constant -- so the
 * delivered object is the one that was there before. Not taken on trust
 * from the optimiser: checked by comparing the objects byte for byte,
 * which is the whole reason the off form is this and not an empty macro
 * with the post left loose. The pattern is VDP_COUNT's, one step further:
 * a macro that vanishes instead of a call that vanishes.
 *
 * A post wrapped this way must be idempotent AND self-contained: whatever
 * it advances -- a cursor, an index -- has to be set up inside the
 * wrapper, or the second pass would start where the first one stopped.
 */
#if VDP_PROFILE
#define VDP_REPEAT_BEGIN(post) do { uint32 vdp_rep_; \
          for(vdp_rep_ = vdp_profile_reps(post); vdp_rep_ != 0UL; vdp_rep_--) {
#define VDP_REPEAT_END      } } while(0)
#else
#define VDP_REPEAT_BEGIN(post) do {
#define VDP_REPEAT_END      } while(0)
#endif

/*
 * ---------------------------------------------------------------------------
 * The backdrop entry: the colour memory entry register 7 names, taken on
 * its low four bits and read out of the second bank of the palette.
 * Register 7 sets the border colour and takes it from the second bank of
 * sixteen (docs/sms_gg/SMSOfficialDocs.md:861-864), which is where the
 * sixteen comes from; the same document says it once more beside the
 * colour memory layout, border and sprite colours coming from that second
 * group (:734). The take on four bits is
 * TotalSMS/src/core/sms_vdp.c:320.
 *
 * The one definition of it in the program. The render uses it for a line
 * with the display switched off and for the masked left column, and the
 * frame loop uses it through vdp_backdrop below for the ground it paints
 * around the picture: one expression, so the picture and its surround can
 * never name two different colours.
 * ---------------------------------------------------------------------------
 */
#define VDP_BACKDROP_INDEX() (16UL + ((uint32)sms.vdp.reg[7] & 15UL))

/*
 * ---------------------------------------------------------------------------
 * The data write, port $BE, as a macro with no call in it: it is the one
 * access a program makes thousands of times per frame -- every tile, every
 * name, every colour goes through it -- so it expands in place inside the
 * processor's port function.
 *
 * Semantics (TotalSMS/src/core/sms_vdp.c:601-637): the control latch
 * falls, the read buffer takes the value, the byte lands in colour memory
 * when the code is 3 and in video memory otherwise -- codes 0, 1 and 2 all
 * write video memory -- and the address steps by one, wrapped on fourteen
 * bits. The colour index is the low five bits of the address
 * (SMSOfficialDocs.md:690-740; sms_vdp.c:540).
 *
 * A colour write also converts: the palette entry of the same index takes
 * the RGB555 of the six bit colour, out of the table below. One load and
 * one store more on the cold branch -- a program writes at most 32
 * colours per palette change -- and nothing on the video memory branch.
 * This is the one pen of the palette; no render reads the colour memory.
 *
 * The argument is evaluated more than once: the expansion site passes a
 * plain parameter. Nothing here touches sms.z80, the obligation every port
 * hook carries (z80.c, the input and output space).
 * ---------------------------------------------------------------------------
 */
/*
 * The six bit colour to RGB555 table, 64 entries, filled at init from the
 * four documented levels and read by the macro below wherever it expands.
 * Defined in vdp.c; a data table with external linkage because the macro
 * expands inside the processor's port function, in another file.
 */
extern uint16 vdp_cram_rgb[64];

#define VDP_IO_DATA_WRITE(v)                                            \
  do                                                                    \
    {                                                                   \
      sms.vdp.latch = 0;                                                \
      sms.vdp.read_buf = (uint32)(uint8)(v);                            \
      if(sms.vdp.code == VDP_CODE_CRAM_WRITE)                           \
        {                                                               \
          sms.vdp.cram[sms.vdp.addr & VDP_CRAM_MASK] = (uint8)(v);      \
          sms.vdp.plut[sms.vdp.addr & VDP_CRAM_MASK] =                  \
            vdp_cram_rgb[(v) & 0x3FU];                                  \
          VDP_COUNT(cram_w);                                            \
        }                                                               \
      else                                                              \
        {                                                               \
          sms.vdp.vram[sms.vdp.addr & VDP_VRAM_MASK] = (uint8)(v);      \
          VDP_COUNT(vram_w);                                            \
        }                                                               \
      sms.vdp.addr = (sms.vdp.addr + 1UL) & VDP_VRAM_MASK;              \
    }                                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The maskable interrupt line, as the processor samples it: up when a
 * frame interrupt is pending and register 1 bit 5 enables it, or when a
 * line interrupt is pending and register 0 bit 4 enables it
 * (TotalSMS/src/core/sms_vdp.c:218-226, :1301-1308; the enable bits,
 * SMSOfficialDocs.md:756-964). A read of the level and nothing more: the
 * processor never writes here, and this module never writes the
 * processor. Sampled once per scanline, at the head of each quota
 * (z80.c), so the flag vdp_line raises at the end of a line is seen at the
 * start of the next -- the status latched at HBlank of
 * SMSOfficialDocs.md:1490, at the scanline grain.
 * ---------------------------------------------------------------------------
 */
#define VDP_IRQ_LINE()                                                  \
  ((uint8)(((sms.vdp.frame_pending != 0UL) &&                           \
            ((sms.vdp.reg[1] & 0x20U) != 0U)) ||                        \
           ((sms.vdp.line_pending != 0UL) &&                            \
            ((sms.vdp.reg[0] & 0x10U) != 0U))))

/*
 * The failures of vdp_init, one per allocation it makes: the video memory
 * could not be had, an index buffer could not, or the bit plane table
 * could not. The caller tells the pixel buffer apart to paint its own
 * screen and paints the video memory screen for the other two; the trace
 * names the block in every case.
 */
#define VDP_ERR_NO_VRAM   (-1)
#define VDP_ERR_NO_PIXELS (-2)
#define VDP_ERR_NO_PLANES (-3)

/*
 * Brings the video part up: takes the video memory, the index buffers and
 * the bit plane table through sys_alloc, so before sys_mem_seal, and puts
 * every field at its power-on value -- the buffers zeroed, the palette
 * the conversion of the zeroed colour memory, both scroll latches from
 * the register table. It also builds the drawing side, once: the colour
 * table, the plane table, the coded cel -- created through the library
 * first, by hand when the library refuses, with the background and
 * load-palette flags set explicitly either way. Nothing of this runs per
 * frame: what runs per frame is the line render inside vdp_line and the
 * draw call, which belongs to the frame loop, not to this module.
 *
 * The register values are the power-on table of the official document
 * (SMSOfficialDocs.md:948-957): R0 0x36, R1 0xA0, R2 to R5 0xFF, R6 0xFB,
 * R7 to R9 0x00, R10 0xFF. TotalSMS starts R1 at 0x80 instead
 * (sms_vdp.c:1564-1569, a value it labels as after the BIOS); the document
 * outranks it here and its 0xA0 -- frame interrupts enabled from the start
 * -- is what the hardware table says. A program that enables interrupts
 * before writing R1 therefore takes one at the first VBlank, which is the
 * machine. The line counter starts at 0xFF (sms_vdp.c:1571).
 *
 * Called after the cartridge boot, because its init line names the profile
 * that boot fixed, and in every configuration: buffers and cel exist in
 * every build so the same init is what every bench and every run
 * exercises. Returns 0, or one of the negative codes above after
 * tracing why; the caller paints the stop. After a failure the structure
 * is undefined -- no memory pointer, no register values -- and no port may
 * be reached: the caller stops the console rather than run on.
 */
int32 vdp_init(void);

/*
 * The cel control block vdp_init built, as an opaque pointer; NULL until
 * init has succeeded. One reader exists: the frame loop takes it once at
 * boot, casts it where the concrete type is in scope, and hands it to its
 * one draw call per frame. This module never draws and never waits.
 */
void *vdp_cel(void);

/*
 * The resolved backdrop colour, RGB555: the palette entry the index above
 * names, which is the colour the hardware shows outside the picture. Read
 * once per frame by the frame loop, on the cold side of its line loop,
 * and never by the render -- the render has the index in a local already.
 *
 * The colour and not the index, because a program may leave register 7
 * alone and rewrite the colour memory entry it points at: comparing
 * indexes would leave the surround a palette behind. Comparing the
 * sixteen bits catches both causes for the same one load.
 *
 * This module never draws and never waits: it says what the colour is,
 * and the caller paints.
 */
uint16 vdp_backdrop(void);

/*
 * Notes that the caller has just repainted the surround with the current
 * backdrop colour. Two things happen and neither is a paint: the window's
 * repaint count steps, and the index in force is named in the trace at
 * most once per report window -- a program that writes register 7 on
 * every frame would otherwise pay a blocking serial write per frame,
 * which is the one thing the periodic aggregates exist to avoid.
 *
 * Cold by construction: the caller only repaints when the colour changed,
 * once per screen of its rotation. Without the counters the BODY is
 * empty, not the call: this is an out of line function in another
 * translation unit and nothing here folds one away, so a silent or a
 * measured build still pays the call. It is paid a handful of times a run
 * and never on a path that is timed, which is why the guard is left where
 * it is rather than pushed into the caller.
 */
void vdp_backdrop_repainted(void);

/*
 * The control port, $BF written (TotalSMS/src/core/sms_vdp.c:639-675).
 * First byte: low address and the latch rises. Second byte: the code in
 * bits 7 and 6, the high address in the rest, and the latch falls. Code 0
 * sets the address, fills the read buffer from it and steps it; code 1
 * and code 3 set the address; code 2 writes the register named by the low
 * four bits of the second byte with the first byte, registers 0 to 10 kept
 * and 11 to 15 ignored and counted (sms_vdp.c:585-592). A cold call: a
 * program writes a handful of control pairs per frame.
 */
void vdp_io_ctrl_write(uint8 value);

/*
 * The data port, $BE read (sms_vdp.c:472-482): returns the buffer, refills
 * it from the current address, steps the address and drops the latch.
 */
uint8 vdp_io_data_read(void);

/*
 * The status port, $BF read (sms_vdp.c:484-510; SMSOfficialDocs.md:220,
 * :1490). Bit 7 is the frame interrupt request; bit 6 is sprite overflow
 * and bit 5 sprite collision, both raised by the line composition; bits
 * 4 to 0 read as ones, which is what TotalSMS returns in mode 4 and what
 * no program is documented to read. Both interrupt requests fall here, so
 * the line falls with them, both sprite bits fall too, and the latch
 * drops -- the only place any of the four falls. A line request pending on
 * its own therefore reads with bit 7 clear -- the bit names the frame
 * interrupt, and a clear bit under an active line means the line
 * interrupt (SMSOfficialDocs.md:220) -- and still drops the line: the
 * read clears both.
 */
uint8 vdp_io_status_read(void);

/*
 * The V counter, $7E read: the line number folded as the 192 line table
 * folds it, 0x00 to 0xDA for lines 0 to 218, 0xD5 to 0xFF for lines 219
 * to 261 (sms_vdp.c:64-81, :442-445).
 */
uint8 vdp_io_vcounter_read(void);

/*
 * The H counter, $7F read, and it is a best effort with no better local
 * source. TotalSMS derives it from the time elapsed inside the line
 * (sms_vdp.c:447-470), a quantity this port has no clock for: time is cut
 * at the scanline and the processor's own counter is stale inside its
 * quota, by the rule of z80.c. The value returned is a constant, the
 * start of the line, and the read is counted, so that a program which
 * relies on it -- a light gun, a raster effect finer than a line -- shows
 * up in the periodic line before it shows up on screen.
 */
uint8 vdp_io_hcounter_read(void);

/*
 * One scanline elapsed. Called by the frame loop after each quota, 262
 * times per frame. First, while the count is inside the picture (0 to
 * 191) and the display is on, the line is rendered: the background of
 * that line, from the name table, the patterns, the scroll latches and
 * the inhibit bits of register 0, then the sprites of that line over it,
 * into the index buffer at that row -- with the display off, the row is
 * filled with the border colour and neither the name table nor the
 * attribute table is read.
 * Then the semantics of TotalSMS/src/core/sms_vdp.c:1466-1513 transposed
 * to the scanline grain: the line count steps; on reaching 193 -- the
 * line after the 192 of the picture -- the frame interrupt request rises;
 * on every line of the picture and the one after it the line counter runs
 * down and, at zero, reloads from register 10 and raises the line
 * interrupt request; on reaching 262 the count wraps to zero, the counter
 * reloads and the vertical scroll latch takes register 9. Last, the
 * horizontal scroll latch takes register 8, for the next line. A call per
 * line and not per instruction, which is the grain the frame loop already
 * runs at; one call per line and nothing called per pixel or per tile.
 */
void vdp_line(void);

/*
 * Emits the aggregates of the closing window and clears them: one debug
 * line with the register, video memory, colour memory, status and
 * accepted interrupt counts, a second with the rarer reads, a third --
 * every time, in the builds with telemetry (VDP_COUNTERS: so not in the
 * measurement build) -- with what the background render
 * is asked to show: name table base, both scroll latches, the two inhibit
 * bits and the left column mask; a fourth, on the same terms, with what
 * the sprites of the window did -- the busiest line, the two bits raised
 * and whether magnification is on; a fifth, only when the window
 * repainted the surround of the picture at all, with how many times and
 * with the backdrop index in force; then a warning naming an unsupported
 * mode if one was written, and another naming a 224 or 240 line height
 * if one was asked. The counted lines are not emitted when every figure
 * is zero. Cold: the frame loop calls it where and as often
 * as it calls cart_io_report, once per emulated second, on the far side
 * of the pacing wait. Compiles to nothing without the counters.
 *
 * The interrupt count is read off sms.z80, the one field the processor
 * keeps for a reader outside itself; this module never writes it, and
 * reports the difference since its previous call.
 */
void vdp_report(void);

#endif /* SMS3DO_VDP_H */
