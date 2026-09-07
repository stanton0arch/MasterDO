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
 * line until the status register is read. On the delivered path
 * (common.h, SMS_DECOR_CEL 1) it writes no pixel: it keeps a picture of
 * the name table and a sheet of the sprite patterns in step with the
 * video memory, and builds, once per frame, a list of cels the engine
 * draws -- windows of the picture, sprites, priority tiles, backdrop
 * (VDP_DECOR_*, VDP_SHEET_*). The two sprite bits of the status register
 * are computed per line from a table of the attribute table, without a
 * pixel. On the older path it renders the background plane of mode 4:
 * once per line of the picture, from the name table, the patterns and
 * the scroll registers the program wrote, straight into the
 * one-byte-per-pixel index buffer the coded cel below reads -- one line
 * per call of vdp_line, eight pixels a stroke, two words a stroke. Then
 * the sprites of that line: the attribute table is walked once, eight of
 * its entries at most are kept, and their opaque pixels are laid into the
 * same row -- above the background, except where the priority mask the
 * background just wrote into its scratch says the background wins.
 * The two sprite bits of the status register are raised there on either
 * path and fall on a status read, like the two interrupt requests.
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
/*
 * The screen's colour table as this module builds it: the 32 colours,
 * then one more entry, index 32, which the display takes as its
 * background colour (docs/3do/3do_portfolio_2.5.md:9874, "32 for the
 * background color"). It is built from colour 0: a pixel of index 0
 * leaves the identity palette as the word 0x0000, and the display shows
 * a zero word in its background colour rather than through entry 0 of
 * the table, so the two are kept the same colour and a zero pixel comes
 * out as colour 0 either way.
 */
#define VDP_CLUT_ENTRIES (VDP_CRAM_SIZE + 1)
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
 * The index buffer: the picture as the cel engine reads it. One index per
 * pixel, ONE BYTE each, 256 wide by the 192 lines of the picture, rows
 * rounded up to the word because the engine fetches rows by words and
 * requires each row word aligned and at least two words long
 * (docs/3do/3DO_Development_Notes.md:92-95; 64 words a row satisfies
 * both). Every size below is calculated from width, depth and height --
 * never restated as a figure -- so the day one of the three changes, all
 * of them follow. At eight bits the width already divides by four, so the
 * rounding is a no-op and is written anyway: the day the width stops
 * dividing, the row still comes out whole.
 *
 * Why a byte and not the six bits an index needs. The picture was once
 * written as six bit indexes squeezed four to three words, and moving to
 * a byte took 58 of the 233 cycles off a stroke of eight pixels, a
 * quarter of the background: three fifths of that was the squeezing
 * itself, the rest the register pressure it put on the loop. The display,
 * timed on the console drawing each picture in turn, paid about a third
 * of a millisecond a frame to fetch the wider one (2.8 to 3.1 ms). So a byte
 * carries a five bit index and the three high bits stay zero: the
 * composition stores the word it has in hand and moves on.
 *
 * The palette of the coded cel holds 32 entries of RGB555, one per colour
 * of the emulated palette (include/3do/graphics.h:261 for the packing).
 * ---------------------------------------------------------------------------
 */
#define VDP_PIX_WIDTH  256UL
#define VDP_PIX_BPP    8UL
#define VDP_PIX_ROW_BYTES ((((VDP_PIX_WIDTH * VDP_PIX_BPP) + 31UL) / 32UL) * 4UL)
#define VDP_PIX_BUF_BYTES (VDP_PIX_ROW_BYTES * VDP_ACTIVE_LINES)
#define VDP_PLUT_ENTRIES 32

/*
 * The composition works eight pixels at a time -- one stroke, two words of
 * four indexes -- so the width must divide by eight for the row to come
 * out whole. Refused at compile time rather than truncated.
 */
#if (VDP_PIX_WIDTH % 8UL) != 0UL
#error "the composition lays 8 pixels per stroke: the width must be a multiple of 8"
#endif

/*
 * ---------------------------------------------------------------------------
 * The priority scratch of one line: one byte per pixel of the picture,
 * plus a lead of eight bytes before it and a tail of sixteen after it. The
 * background is composed tile by tile, eight pixels a stroke, and under a
 * fine horizontal scroll the leftmost stroke starts before pixel 0: its
 * mask is written whole into the lead rather than tested pixel by pixel,
 * which is what makes a write outside the picture impossible by
 * construction, with no compare in the loop.
 *
 * IT IS NOT WRITTEN ON EVERY LINE, and that is the thing to know before
 * reading it anywhere. It has exactly one reader, the sprite composition,
 * which leaves a background pixel of priority alone. So a line that
 * carries no sprite does not write it at all: it lays its strokes straight
 * into the row the engine reads and leaves the scratch holding whatever an
 * earlier line put there. A line that carries sprites lays the mask here
 * as it composes. What the scratch holds after a rendered line therefore
 * says nothing about that line unless the line carried a sprite.
 *
 * There is no index scratch beside it any more. The picture itself is one
 * byte a pixel, so the composition writes its final indexes into the row
 * and the sprite pass draws over them there; a second copy of the line
 * would be a copy and nothing else.
 *
 * Pixel x of the picture is scratch[line_org + x], NOT a fixed offset: the
 * fine scroll moves the origin rather than each stroke, for the alignment
 * reason set out below.
 *
 * The tail is margin and nothing writes it. The 33 strokes stop at byte
 * 263 and the picture ends at byte 263 at the latest, so bytes 264 upward
 * are never written; they are kept as slack on the buffer every mask store
 * reaches by word rather than shrunk to nothing. The bench asserts nothing
 * writes them.
 * ---------------------------------------------------------------------------
 */
#define VDP_LINE_LEAD    8UL
#define VDP_LINE_TAIL    16UL
#define VDP_LINE_SCRATCH (VDP_LINE_LEAD + VDP_PIX_WIDTH + VDP_LINE_TAIL)

/*
 * The scratch is written a word at a time and read a byte at a time, so it
 * is held as words and viewed as bytes. What forces it: the processor
 * refuses a word store on an address that is not a multiple of four, and a
 * stroke shifted right by the fine scroll would land on any of eight.
 *
 * So the fine scroll does not shift the destination -- it shifts the
 * origin of the picture inside the scratch. Stroke c stands at byte c * 8,
 * always aligned, and pixel x of the picture is at line_org + x with
 * line_org = VDP_LINE_LEAD - fine. The row of the picture, on the other
 * hand, has a fixed origin -- pixel x is byte x -- so the strokes written
 * there are RECUT by the fine scroll out of the words in hand (below).
 * The lead holds the stroke that runs in from the left, the tail the one
 * that runs out to the right, and both are what makes a write outside the
 * scratch impossible by construction: the highest byte a stroke touches is
 * 33 * 8 - 1.
 */
#if (VDP_LINE_SCRATCH % 4UL) != 0UL
#error "the line scratch is laid a word at a time: its length must be a multiple of four"
#endif
#if (33UL * 8UL) > VDP_LINE_SCRATCH
#error "the 33 strokes of a scrolled line must fit the scratch"
#endif
#define VDP_LINE_WORDS (VDP_LINE_SCRATCH / 4UL)

/*
 * Both ways of rendering a line cut each stroke's eight picture pixels out
 * of four words in hand -- the two of the stroke before and the two of
 * this one -- and pick which pair by (VDP_LINE_LEAD - fine) >> 2. That
 * holds because the lead is eight and the fine scroll is under eight, so
 * the origin never sits more than two words back. A lead of twelve would
 * put it three words back and every group would be cut from the wrong
 * stroke, over the whole picture, in silence. Refused rather than
 * commented, like the two lengths above.
 */
#if VDP_LINE_LEAD != 8UL
#error "the strokes are cut from four words in hand: the lead must be eight"
#endif

/*
 * ---------------------------------------------------------------------------
 * A word of four indexes, and the ONE place in the program where a pixel
 * index is moved inside a word instead of as a byte.
 *
 * Why it has to be said here rather than at the use. Every index the
 * render produces is written as a byte -- the decoded row cache lays its
 * eight indexes byte by byte on purpose -- so the order of the four
 * indexes inside a word is the byte order of the machine, and the two
 * machines this code is built for disagree: the lane carrying pixel 0 is
 * the most significant byte of the word on the big endian target and the
 * least significant one on a little endian host. A recut that moves the
 * picture by whole bytes inside a word has to move them TOWARDS pixel 0,
 * and which way that is depends on the order. So the lane order is named
 * once, here, and every path that recuts a word obeys it.
 *
 * The switch defaults to the target and is held against the machine at
 * init rather than trusted: a build that guessed wrong draws a scrambled
 * picture, which is a slow and confusing way to learn it. It can also be
 * forced from the command line, which is what lets a host build compile
 * and exercise the order it is not running on.
 *
 * BOTH orders are spelled out below, unconditionally, and the switch only
 * chooses which one the render calls. That is not a matter of taste: a
 * form written behind an #if that no machine in the loop preprocesses is a
 * form nothing compiles and nothing tests, and a mistake in it would ride
 * all the way to the console with every bench green. Named apart, the two
 * are both compiled everywhere and the bench puts each against the byte
 * run it is meant to reproduce, laid out the way its own order lays it.
 * ---------------------------------------------------------------------------
 */
#ifndef VDP_LANE_MSB_FIRST
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || \
    defined(_M_X64) || defined(__ARMEL__) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
     ((__BYTE_ORDER__) == (__ORDER_LITTLE_ENDIAN__)))
#define VDP_LANE_MSB_FIRST 0
#else
#define VDP_LANE_MSB_FIRST 1
#endif
#endif

/*
 * The four bytes that begin r bytes into the word a, taken from a and from
 * the word b that follows it: the recut a fine horizontal scroll asks for,
 * the picture beginning r bytes into the stroke while the strokes stand on
 * word boundaries. The two shift counts are computed once for a whole line
 * -- sl is 8 * r and sr is 31 - sl -- and r is constant down the line, so
 * the recut is exact by construction and not an approximation: it is the
 * same run of bytes read from another rank.
 *
 * A shift of thirty-two is undefined in C and r == 0 would ask for one, so
 * the following word is shifted down by one first. With r == 0 that leaves
 * nothing of it at all and the join is the word a, which is the answer
 * wanted there.
 */
#define VDP_LANE_JOIN_MSB(a,b,sl,sr) (((a) << (sl)) | (((b) >> 1) >> (sr)))
#define VDP_LANE_JOIN_LSB(a,b,sl,sr) (((a) >> (sl)) | (((b) << 1) << (sr)))

#if VDP_LANE_MSB_FIRST
#define VDP_LANE_JOIN(a,b,sl,sr) VDP_LANE_JOIN_MSB(a,b,sl,sr)
#else
#define VDP_LANE_JOIN(a,b,sl,sr) VDP_LANE_JOIN_LSB(a,b,sl,sr)
#endif

/*
 * Eight pixels -- two lane words in picture order -- laid into the row.
 * The one emitter of the module: the line that carries sprites and the
 * line that carries none both come through here, so a picture composed
 * two ways cannot come out two ways.
 *
 * It is two word stores and nothing else, and that is right in BOTH byte
 * orders, which is worth saying because it looks like a big endian
 * shortcut. Each lane word came out of bytes -- read whole from the
 * decoded row cache, or recut by the join of this machine's order -- so
 * its lanes sit in the machine's own order; stored as a word, it lays
 * those same four bytes back down in memory order, pixel 0 first, on
 * either machine. What the bench holds is the recut, in each order, against
 * the byte run it must reproduce; the store is exercised for real by every
 * scene it renders, on the host in one order and on the console in the
 * other.
 *
 * Named rather than written out at the two call sites for the reason the
 * join is named: one spelling, held by one bench.
 */
#define VDP_EMIT8(a,b,dst)                                          \
  do                                                                \
    {                                                               \
      (dst)[0] = (a);                                               \
      (dst)[1] = (b);                                               \
    }                                                               \
  while(0)

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
 * The decoded row cache. A pattern row is four bytes of video memory, one
 * per plane, and the eight indexes it stands for are the same eight every
 * time that row is met: the picture shows a few hundred distinct rows and
 * asks for them tens of thousands of times a frame. Decoded once and kept,
 * a row costs two word reads instead of thirty-two table reads.
 *
 * The key is the row's own video address divided by four, so the table has
 * one entry per row the video memory can hold and no lookup structure of
 * its own. Vertical flip needs no entry: it reads another row of the same
 * pattern, so another address, so another entry.
 *
 * An entry is eight indexes, one byte each, held as two words because the
 * composition reads and lays them a word at a time. The indexes are stored
 * bare, without the palette bank: the bank belongs to the name table entry
 * and not to the row, and two columns of different banks share one entry.
 *
 * Validity is one byte a row and not one bit: the test sits on the hottest
 * read of the render, where a bit costs a shift and a mask, and the four
 * kilobytes it spends are inside the ceiling. Both blocks are taken as one
 * allocation, the words first so that the whole thing is word aligned.
 */
#define VDP_TC_ROW_BYTES   4UL
#define VDP_TC_ROWS        (VDP_VRAM_SIZE / VDP_TC_ROW_BYTES)
#define VDP_TC_WORDS       2UL
#define VDP_TC_BYTES       (VDP_TC_ROWS * VDP_TC_WORDS * 4UL)
#define VDP_TC_VALID_BYTES VDP_TC_ROWS
#define VDP_TC_TOTAL_BYTES (VDP_TC_BYTES + VDP_TC_VALID_BYTES)

/*
 * The ceiling the cache was granted, and it is a refusal rather than a
 * comment: the free memory of the machine is shared with the sound and the
 * saves, and a table that grew past this would spend theirs. The figure is
 * a share of the 229376 bytes of DRAM the boot trace reports free once
 * every other block is taken -- a little under a third, chosen so that
 * more than 150 kilobytes are left for the parts that have spent nothing
 * yet. Raising it is not a code decision; the trace says what is left.
 */
#define VDP_TC_CEILING_BYTES 65536UL
#if VDP_TC_TOTAL_BYTES > VDP_TC_CEILING_BYTES
#error "the decoded row cache is over its ceiling: shrink it rather than take the memory"
#endif

/*
 * The key of the row that holds video address a, and the lane constant a
 * composition needs to spread a bank or a priority bit over four pixels
 * at once.
 *
 * A pattern index is below 16 by construction, and the sprite pass leans
 * on it: an index in the row is the pattern's four bits plus sixteen for
 * the second bank, so its low four bits are zero exactly when the pixel
 * is transparent, and that is how "opaque" is read off the row with no
 * mask of its own. "Below 16" is a property of the plane table and not a
 * hope: an index is the OR of one byte per plane, each weighing one bit,
 * so four planes give four bits. A fifth would put a bit where the bank
 * lives and make a transparent pixel of the second bank read as opaque,
 * which is why it is refused here rather than commented.
 */
#if VDP_PLANES_COUNT > 4UL
#error "the opacity test reads the low four bits of an index: more than four planes overlaps the bank"
#endif
#define VDP_TC_KEY(a)   (((a) & VDP_VRAM_MASK) >> 2)

/*
 * The same key from what the render has in hand: a pattern number and a
 * row, which stand for the video address pattern * 32 + row * 4. Written
 * as its own macro so that the read side and the write side of the table
 * cannot drift apart -- the bench holds the two against each other over
 * every pattern and every row. The largest key a name table entry can ask
 * for is refused below if it ever stopped fitting the table.
 */
#define VDP_TC_KEY_TILE(pattern,row) (((pattern) << 3) + (row))

#if (((0x1FFUL << 5) + (7UL << 2)) >> 2) >= VDP_TC_ROWS
#error "the largest pattern row a name table entry names is past the end of the row cache"
#endif
#define VDP_TC_LANE_ONE 0x01010101UL

/*
 * The name table rows the scroll wraps on, and the picture rows: 28 rows
 * of 8 lines, 224 lines, the height of the whole table in the 192 line
 * mode (SMSOfficialDocs.md:870-896; TotalSMS/src/core/sms_vdp.c:803-824).
 */
#define VDP_NT_LINES 224UL

/*
 * ---------------------------------------------------------------------------
 * The background as the cel engine draws it (common.h, SMS_DECOR_CEL).
 *
 * The picture is the whole name table rendered once: 32 by 28 tiles of 8
 * pixels, 256 by 224 bytes, one index per pixel, with the same row pitch
 * as the index buffer above -- the same preamble arithmetic serves both.
 * It is kept in step tile by tile: a tile whose name table word moved, or
 * whose pattern was rewritten, is converted again from the decoded row
 * cache, and nothing else of the picture is touched. The engine draws it
 * by windows: a source pointer into the picture, a width, a height, and
 * a position on the screen, the pitch staying the picture's. A scroll is
 * two windows meeting at the join; a locked row band or column band is a
 * window of its own; a band of lines drawn with an older video memory is
 * a set of windows too. So the pixels read per frame stay the 256 by 192
 * of the visible picture, plus the alignment read below.
 *
 * A source pointer is a word address (docs/3do/3DO_Development_Notes.md:
 * 87-89), so a window that must start on pixel column px starts on the
 * column px - (px & 3) instead, is (px & 3) pixels wider, and stands
 * (px & 3) pixels further left: its first pixels are those of the
 * columns before, and they fall outside the picture's area of the
 * screen, where the clip rectangle set at init erases them. At most
 * three columns per window, which the picture check counts against the
 * pixels a frame reads.
 *
 * The picture and the cel control blocks of the windows share one page
 * of 64 kilobytes, the grain the allocator was measured to spend in: the
 * picture at the head, the blocks behind it. The block count is the
 * arena's capacity, sized for the worst frame: a band draws at most nine
 * windows -- three for the locked top rows (a vertical join, no
 * horizontal one), six for the scrolled rows (a join each way, and the
 * locked right columns split by the vertical join) -- so the capacity is
 * nine per band, and a window past it is refused and counted rather than
 * written past the page. The page test below is what holds the count.
 *
 * The journal holds the video memory writes that landed on the visible
 * background while the picture was being scanned: line, address, the
 * byte before and the byte after. It is undone at the head of the
 * presentation, so that the picture stands as the video memory stood
 * when the frame began, and replayed band by band, so that each band is
 * drawn with the memory as it was at its first line; its capacity bounds
 * the bands to the distinct lines it holds, capped at the band count.
 *
 * Every size is calculated from the constants above; the page and the
 * block size are hardware facts written once. The block size is the
 * console's cel control block, seventeen words
 * (include/3do/graphics_ccb.h:8-33); a host with wider pointers has a
 * wider block, and the init holds the real size against the page as
 * well, so a page that stopped fitting is refused there with a reason.
 * ---------------------------------------------------------------------------
 */
#define VDP_DECOR_LINES     VDP_NT_LINES
#define VDP_DECOR_BYTES     (VDP_PIX_ROW_BYTES * VDP_DECOR_LINES)
#define VDP_DECOR_PAGE      65536UL
#define VDP_LIST_BANDS      8UL
#define VDP_LIST_WINDOWS    (VDP_LIST_BANDS * 9UL)
#define VDP_JOURNAL_ENTRIES 256UL
#define VDP_CCB_BYTES       68UL
#define VDP_NT_TILES        ((VDP_NT_LINES / 8UL) * 32UL)
#define VDP_NT_CHUNKS       ((VDP_NT_TILES * 2UL) / 32UL)
#define VDP_CHUNKS          (VDP_VRAM_SIZE / 32UL)

#if (VDP_PIX_WIDTH % 4UL) != 0UL
#error "a window's source pointer is a word address: the width must be a multiple of 4"
#endif

#if (VDP_DECOR_BYTES % 4UL) != 0UL
#error "the cel control blocks follow the picture in its page: the picture must end on a word"
#endif

#if (VDP_DECOR_BYTES + (VDP_LIST_WINDOWS * VDP_CCB_BYTES)) > VDP_DECOR_PAGE
#error "the picture and the window blocks no longer fit one page of 64 kilobytes"
#endif

#if (VDP_NT_TILES != 896UL) || (VDP_NT_CHUNKS != 56UL) || (VDP_CHUNKS != 512UL)
#error "the tile, name table chunk and chunk counts are written into the journal's arithmetic"
#endif

/*
 * ---------------------------------------------------------------------------
 * The sprites and the priority tiles as the cel engine draws them: small
 * cels of the same list, in every band, after the windows of the
 * background.
 *
 * The sprite patterns are converted on demand into a SHEET: 256 pixels
 * wide, 128 lines, one byte a pixel, the row pitch of the picture -- the
 * one pitch the console has been seen to draw -- so that a sprite is a
 * window of the sheet exactly as a scroll region is a window of the
 * picture, through the same cel factory. Pattern p lives at column
 * VDP_SHEET_X(p) and line VDP_SHEET_Y(p): the pair (2k, 2k + 1) is laid
 * as sixteen contiguous lines, so a tall sprite is one window of sixteen
 * rows. Colour 0 is stored as 0, which the identity palette leaves as the
 * 000 the engine paints transparent; colours 1 to 15 are stored as 16 to
 * 31, the second bank of the palette (docs/sms_gg/SMSOfficialDocs.md:
 * 383-385). A pattern's place is converted when a sprite of a band needs
 * it and its validity byte is down; the byte falls with the dirty sweep of
 * the picture, so a game that rewrites its sprite patterns pays eight rows
 * per pattern touched and nothing else.
 *
 * A priority tile needs no conversion: it is a window of the background
 * picture drawn under a SECOND palette, the identity with entry 16 at
 * 000 -- so 0 and 16, the colour 0 of either bank, pass and the fifteen
 * colours of either bank cover the sprites (SMSOfficialDocs.md:474-481:
 * background colour 0 always shows under the sprites). Runs of
 * consecutive priority tiles of a tile row are one window.
 *
 * The sheet and the small cel control blocks share one page of 64
 * kilobytes, as the picture and its window blocks do: the sheet at the
 * head, VDP_LIST_CELS blocks behind it. A cel past the reserve is refused
 * and counted. The rows a program switches the display off on, and the
 * masked left column, are cels too: a column of VDP_COLUMN_W bytes by 192
 * lines filled with the backdrop index, kept in the free tail of the
 * picture's page and refilled when register 7 moves, drawn 1:1 for the
 * column and stretched 32 times wide for a run of rows switched off.
 * ---------------------------------------------------------------------------
 */
#define VDP_SHEET_W       256UL
#define VDP_SHEET_LINES   128UL
#define VDP_SHEET_BYTES   (VDP_SHEET_W * VDP_SHEET_LINES)
#define VDP_SPRITE_PAGE   65536UL
#define VDP_LIST_CELS     480UL
#define VDP_COLUMN_W      8UL
#define VDP_COLUMN_BYTES  (VDP_COLUMN_W * VDP_ACTIVE_LINES)
#define VDP_SHEET_X(p)    ((((p) >> 1) & 31UL) << 3)
#define VDP_SHEET_Y(p)    ((((p) >> 6) << 4) + (((p) & 1UL) << 3))

#if VDP_SHEET_W != VDP_PIX_ROW_BYTES
#error "a sprite is a window of the sheet through the picture's cel factory: the sheet must have the picture's row pitch"
#endif

#if (VDP_CHUNKS * 64UL) != VDP_SHEET_BYTES
#error "the sheet holds one place of 8 by 8 bytes per pattern the video memory can hold"
#endif

#if (VDP_SHEET_BYTES + (VDP_LIST_CELS * VDP_CCB_BYTES)) > VDP_SPRITE_PAGE
#error "the sheet and the small cel blocks no longer fit one page of 64 kilobytes"
#endif

#if (VDP_DECOR_BYTES + (VDP_LIST_WINDOWS * VDP_CCB_BYTES) + VDP_COLUMN_BYTES) > VDP_DECOR_PAGE
#error "the backdrop column no longer fits in the tail of the picture's page"
#endif

/*
 * One window of a band as it was built, kept so that the priority tiles
 * of the band are cut exactly as the windows were: the screen columns
 * and rows it covers, and the picture column and row it shows first. At
 * most nine per band (VDP_LIST_WINDOWS says why).
 */
typedef struct
{
  uint16 xa;
  uint16 xb;
  uint16 ya;
  uint16 yb;
  uint16 px;
  uint16 py;
} vdp_win_t;

#define VDP_BAND_WINDOWS (VDP_LIST_WINDOWS / VDP_LIST_BANDS)

/*
 * One entry of the journal: the line the write landed on, the address,
 * the byte the address held and the byte it took. Two words, no pointer,
 * no padding -- the two bytes are held as halfwords so that the entry
 * is a whole number of words on its own and the compiler has nothing to
 * insert; the array is inside the state and costs what it says.
 */
typedef struct
{
  uint16 line;
  uint16 addr;
  uint16 old;
  uint16 val;
} vdp_journal_t;

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
 * silently stop holding if the constant moved. The composition and the
 * preamble are written for one index per byte, two words to a stroke of
 * eight pixels. The vertical position of a line is the line plus a scroll
 * of at most 255, wrapped on the table height by ONE subtraction, which
 * only wraps a sum below twice that height. And a stroke of eight pixels
 * shifted right by up to seven needs eight bytes of lead before pixel 0
 * and fifteen after pixel 255 for its writes to be inside the scratch.
 */
#if VDP_PIX_BPP != 8UL
#error "the composition and the cel preamble are written for one index per byte"
#endif

#if ((VDP_ACTIVE_LINES - 1UL) + 255UL) >= (2UL * VDP_NT_LINES)
#error "one subtraction no longer wraps the vertical position on the name table"
#endif

/*
 * The stroke that runs in from the left is written whole into the lead, so
 * the lead holds one. Nothing runs off the right any more: the strokes are
 * no longer shifted, they stop at byte 33 * 8 - 1, and the constraint that
 * matters is that the scratch reaches that byte -- checked where the
 * scratch is defined, along with the word alignment the strokes need.
 */
#if VDP_LINE_LEAD < 8UL
#error "the stroke that runs in from the left needs eight bytes of lead"
#endif

/*
 * The preamble fields of the cel are counted from these constants, and
 * their ranges are hardware facts, refused one by one so that the wording
 * of the one that fires is the one that is read. A row must be at least
 * two words for the engine's pipelined fetch; the pixel count field holds
 * eleven bits; and the row offset of a depth of eight bits is read from
 * the TEN bit field, the one eight and sixteen bits share -- anything
 * below eight takes the eight bit field instead (src_exemple/lrex/main.c:
 * 290-291; the pairing spelled as code in src_exemple/3d_3do_logo/main.c:
 * 79-82). A change of constant that breaks any of the three would compile
 * into a cel the engine misreads with no error anywhere -- the row offset
 * silently truncated is what shears a picture into diagonals.
 */
#if (VDP_PIX_ROW_BYTES / 4UL) < 2UL
#error "cel preamble: a row needs 2 words minimum for the engine's pipelined fetch"
#endif

#if (VDP_PIX_WIDTH - 1UL) > 0x7FFUL
#error "cel preamble: the pixel count fits 11 bits"
#endif

#if ((VDP_PIX_ROW_BYTES / 4UL) - 2UL) > 0x3FFUL
#error "cel preamble: the row offset of a depth of 8 or 16 bits is read from a 10 bit field"
#endif

/*
 * And the depth itself, which the three above do not watch: a row of 16 bit
 * pixels is 512 bytes with a row offset of 126, inside all their ranges,
 * and the library would still be handed a depth a CODED cel cannot be built
 * at -- the portfolio is explicit that a coded cel takes 8 bits per pixel at
 * most (docs/3do/3do_portfolio_2.5.md:5433). Refused here rather than at run
 * time, where it arrives as a NULL nobody can read a reason out of. The
 * composition's guard above already refuses every depth but eight; this
 * one stays for the cel's own reason, so that a depth of sixteen is
 * refused on both counts.
 */
#if VDP_PIX_BPP > 8UL
#error "cel preamble: a coded cel takes 8 bits per pixel at most"
#endif

/*
 * The last one watches two constants against each other rather than a
 * constant against the hardware. The composition walks 32 strokes and each
 * writes two words; the row is sized from the width. Nothing else ties the
 * two, and they agree today by arithmetic that is written in two places --
 * the loop bound is a literal. A width that moved would leave the strokes
 * writing past the end of their row and into the next line, quietly, with
 * a picture that still looks like a picture.
 */
#if ((VDP_PIX_WIDTH / 8UL) * 2UL) != (VDP_PIX_ROW_BYTES / 4UL)
#error "the strokes of a line no longer fill exactly one row"
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
   *
   * ONE path writes it: VDP_IO_DATA_WRITE below, which throws away the
   * decoded row that byte belongs to. Anything else that ever writes here
   * -- a loader, a save state, a debug poke -- must throw the rows away
   * too, or the picture will show tiles that no longer exist. The init
   * clear does it by invalidating the whole table.
   */
  uint8 *vram;

  /*
   * Colour memory, 32 bytes, stored as written and read by nothing per
   * pixel or per line. Its image is the screen's colour table below, which
   * vdp_clut_take rebuilds once per frame when the flag beside it is up:
   * a colour write here is a byte stored and a flag raised, nothing more.
   */
  uint8 cram[VDP_CRAM_SIZE];

  /*
   * Up when a colour byte has been written since the table was last
   * rebuilt, down once vdp_clut_take has rebuilt it. Written and not
   * compared: the data write macro stores it on the port path and a
   * store is all it does there. A word, like the other flags of this
   * structure, so that no padding is added around it.
   */
  uint32 cram_dirty;

  /*
   * The screen's colour table, 33 packed entries in the form the display
   * takes them (include/3do/graphics.h:264, MakeCLUTColorEntry): the
   * index in the high byte, then red, green and blue at eight bits each,
   * one of four levels apiece; the last entry is the display's background
   * colour (include/3do/graphics.h:272, MakeCLUTBackgroundEntry), the
   * colour of a zero word, built from colour 0 so that a pixel of index 0
   * shows as colour 0. Rebuilt from the colour memory by vdp_clut_take
   * and read through vdp_clut by the frame loop, which loads it on each
   * screen of the rotation just before that screen is drawn. This module
   * never loads it anywhere: it says what the colours are, and the caller
   * sets them.
   */
  uint32 clut[VDP_CLUT_ENTRIES];

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
   * The palette of the coded cel, RGB555, and it is the IDENTITY: entry n
   * holds n on each of its three components, filled once at init and
   * written by nothing afterwards. A coded cel has no form without a
   * palette, so it keeps one, but this one decides no colour: a pixel of
   * index n comes out of the cel as (n, n, n), each five bit component
   * indexes the screen's colour table above, and that table -- the image
   * of the colour memory -- names the colour shown. Entry 0 is 0x0000,
   * which the background flag of the cel paints opaque, as before. The
   * cel engine reads it through the cel's palette pointer on every draw.
   */
  uint16 plut[VDP_PLUT_ENTRIES];

  /*
   * The priority mask of one line, one byte per pixel: 1 where the
   * background COLUMN carries priority, or where the left column is
   * masked; 0 elsewhere. Read by the sprite stage, which leaves a
   * background pixel alone where the mask is up AND the pixel in the row
   * is opaque -- the second half is read off the row, whose index has its
   * low four bits at zero exactly when it is transparent. The indexes
   * themselves go straight into the row of the picture, which is one
   * byte a pixel too.
   *
   * Held as words and viewed as bytes: the background lays a stroke a
   * word at a time. Shape, alignment and the origin of the picture inside
   * it: VDP_LINE_* above.
   */
  uint32 prio_w[VDP_LINE_WORDS];

  /*
   * Where pixel 0 of the picture sits in the scratch above, in bytes.
   * VDP_LINE_LEAD minus the fine scroll of the line being composed
   * (VDP_LINE_* above says why), and VDP_LINE_LEAD on a line with the
   * display off. Written by the render at the head of a line and read by
   * everything that reads the mask in picture coordinates: the sprite
   * pass and the masked left column.
   */
  uint32 line_org;

  /*
   * The decoded row cache and the byte per row that says whether it still
   * stands. One allocation, sized and keyed by VDP_TC_* above; the
   * validity bytes follow the words inside the same block.
   */
  uint32 *tc;
  uint8 *tc_valid;

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
   * The index buffers, one frame of one byte indexes each, allocated
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

  /*
   * Where the picture sits on the screen, in bitmap pixels: the offset
   * that centres the 256 by 192 view in the raster the console built.
   * Read through vdp_view by the frame loop, which sets the clip
   * rectangle of every screen on it; the cel positions of this module
   * are then relative to that rectangle, since the folio takes (0,0) as
   * the top-left corner of the clip window
   * (docs/3do/3do_portfolio_2.5.md:10984).
   */
  int32 view_x;
  int32 view_y;

#if SMS_DECOR_CEL
  /*
   * The background picture and the window blocks, in one page taken at
   * init (VDP_DECOR_*): the picture at the head, VDP_LIST_WINDOWS cel
   * control blocks behind it. The blocks are held as an opaque pointer
   * for the reason the cel above is.
   */
  uint8 *decor;
  void  *windows;

  /*
   * The journal of the writes that touched the visible background while
   * the picture was being scanned, in the order they landed -- which is
   * line order, since the line count only grows inside a frame. The
   * count is how many stand; the replay cursor is how many the bands
   * drawn so far have put back.
   */
  vdp_journal_t journal[VDP_JOURNAL_ENTRIES];
  uint32 journal_count;
  uint32 journal_replayed;

  /*
   * Up once a write of this picture found the journal full. The journal
   * is then not a record of the picture's writes, and undoing and
   * replaying what it holds would leave the memory wrong for good -- a
   * lost write on an address the journal also holds would be undone to
   * its old byte and put back to the journaled one, never to the byte
   * really written. So the presentation of such a picture touches the
   * memory not at all: one band, the final state, the journal dropped.
   * Falls at the end of the presentation.
   */
  uint32 journal_overflow;

  /*
   * The name table word each tile of the picture was last converted
   * from, its thirteen significant bits only (pattern, the two flips,
   * the bank, the priority: docs/sms_gg/SMSOfficialDocs.md:321-334; bits
   * 13 to 15 draw nothing and the line render ignores them too), and
   * 0xFFFF for a tile never converted -- a value no masked word can take,
   * so the mark never collides with a real entry. A tile is converted
   * again when the word it shows differs from the masked word the table
   * holds. All 896 are forced to 0xFFFF when register 2 moves the table.
   */
  uint16 decor_word[VDP_NT_TILES];

  /*
   * Per chunk of 32 bytes of video memory -- the size of one pattern,
   * so the chunk index of a pattern's bytes is the pattern number. refs
   * counts the tiles of the picture that show the pattern, kept at the
   * conversion; hot marks a pattern a name table write named since the
   * journal was last emptied, before the conversion could count it;
   * watch is their union with the chunks of the name table itself, the
   * one byte the data write reads to decide whether a write is worth a
   * journal entry. The hot list remembers which entries to clear.
   */
  uint16 refs[VDP_CHUNKS];
  uint8  hot[VDP_CHUNKS];
  uint8  watch[VDP_CHUNKS];
  uint16 hot_list[VDP_CHUNKS];
  uint32 hot_count;

  /*
   * The chunks written since the picture was last brought up to date,
   * one byte each, held as words so that the sweep reads four at a
   * time (VDP_DECOR_DIRTY views them as bytes); the patterns among them
   * that the picture references, marked for the tile sweep of one
   * presentation and cleared after it; the list of the chunks the sweep
   * found dirty, so that the marks are cleared without a second walk.
   */
  uint32 decor_dirty_w[VDP_CHUNKS / 4UL];
  uint8  pat_dirty[VDP_CHUNKS];
  uint16 dirty_list[VDP_CHUNKS];

  /*
   * Up when every tile has to be looked at whatever the dirty marks say:
   * after init, and after register 2 moved the table. The first chunk of
   * the name table, so that a chunk index is tested against the table
   * with one subtraction.
   */
  uint32 decor_sweep_all;
  uint32 nt_chunk0;

  /*
   * The bands of the presentation being drawn: the first line of each
   * and how many there are, from the distinct lines of the journal; and
   * how many window blocks of the arena the bands so far have taken.
   */
  uint32 band_line[VDP_LIST_BANDS];
  uint32 band_count;
  uint32 list_used;

  /*
   * The sheet of converted sprite patterns, the small cel blocks behind
   * it in the same page (VDP_SHEET_*, VDP_LIST_CELS), and the backdrop
   * column in the tail of the picture's page (VDP_COLUMN_*). The blocks
   * are held as an opaque pointer for the reason the windows are. How
   * many blocks the bands of this presentation have taken so far.
   */
  uint8 *sheet;
  void  *cels;
  uint8 *column;
  uint32 cels_used;

  /*
   * Whether the place of pattern p in the sheet holds the pattern as the
   * video memory now holds it: raised by the conversion, dropped by the
   * dirty sweep of the picture for every chunk written since. The chunk
   * index of a pattern's bytes is the pattern number, as for refs.
   */
  uint8 sheet_valid[VDP_CHUNKS];

  /*
   * The second palette, for the priority tiles: the identity, entry 16
   * at 000. Filled once at init, written by nothing afterwards.
   */
  uint16 plut_prio[VDP_PLUT_ENTRIES];

  /*
   * The sprite attribute table: its address (register 5 bits 6 to 1,
   * SMSOfficialDocs.md:827-838), its first chunk for the watch test, and
   * whether a table byte, register 1 or register 5 has moved since the
   * per-line table below was built.
   */
  uint32 sat_base;
  uint32 sat_chunk0;
  uint32 spr_dirty;

  /*
   * The patterns the table names, each once, so that a write of one is
   * journaled like a write of a pattern the picture shows: the marks per
   * chunk, and the list of the marked ones so that they fall without a
   * walk. Rebuilt with the per-line table.
   */
  uint8  spr_named[VDP_CHUNKS];
  uint16 spr_named_list[VDP_SPR_COUNT * 2UL];
  uint32 spr_named_count;

  /*
   * The per-line table, built from the attribute table when it changes,
   * for the line being counted and the lines after it, and read once
   * per line: for each entry before the terminator, the screen line it
   * starts on (signed, as spr_top is), for the collision of the current
   * line; per line of the picture, the entries the hardware admitted
   * when the line was counted -- the first eight in table order, as
   * indexes for the collision, as a bit per entry for the band builder
   * -- how many, and whether a ninth fell on the line. The bands read
   * the admission alone off it: everything else about an entry they
   * take from the video memory as replayed to their first line.
   */
  int16  sat_top[VDP_SPR_COUNT];
  uint32 spr_adm[VDP_ACTIVE_LINES][2];
  uint8  spr_idx[VDP_ACTIVE_LINES][VDP_SPR_MAX_ON_LINE];
  uint8  spr_n[VDP_ACTIVE_LINES];
  uint8  spr_ovf_line[VDP_ACTIVE_LINES];

  /*
   * The lines the display was off on (register 1 bit 6 clear when the
   * line was counted): drawn in the backdrop colour by a stretched cel
   * of the column, over everything else of the band.
   */
  uint8  row_off[VDP_ACTIVE_LINES];

  /* The windows of the band being built, for the priority pass. */
  vdp_win_t win[VDP_BAND_WINDOWS];
  uint32    win_count;
#endif /* SMS_DECOR_CEL */

#if VDP_COUNTERS
  /*
   * The aggregates the periodic line reports, cleared by it. The figures
   * feed a line and nothing else, hence the guard: a silent or a measured
   * build counts nothing.
   */
  uint32 cnt_reg_w;
  uint32 cnt_vram_w;
  uint32 cnt_cram_w;
  /*
   * Of the colour writes, how many landed while the picture was being
   * scanned (line below VDP_ACTIVE_LINES), and how many times the screen
   * table was rebuilt. The first is a journal and not a fault: the table
   * is set once per frame, so a colour written mid-picture shows on the
   * next frame's lines, and a program that does this on purpose -- a
   * palette split -- is what the figure names. The second, against the
   * first line's count, says how many writes one rebuild absorbed.
   */
  uint32 cnt_cram_mid;
  uint32 cnt_clut_upd;
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

  /*
   * What the decoded row cache did over the window: rows served already
   * decoded, rows decoded on first use, and rows a write to the video
   * memory threw away while they still stood. The third is counted
   * without a test -- the validity byte is added before it is cleared --
   * so the write path keeps its shape whether the counters are in or out.
   */
  uint32 cnt_tc_hit;
  uint32 cnt_tc_miss;
  uint32 cnt_tc_inval;

  /*
   * How the lines of the window were rendered: the short way, or with the
   * priority scratch and the sprite pass. A line takes the second way when
   * it carries at least one sprite, since the sprite pass is the only
   * reader of the scratch, and the short way otherwise. The pair is what
   * the cost of a frame is read with -- the work the short way saves is
   * per line, so a figure for the whole frame means nothing without
   * knowing how many lines took it. A line with the display off counts in
   * neither: it fills the row and returns before the choice.
   */
  uint32 cnt_line_fast;
  uint32 cnt_line_scratch;
#if SMS_DECOR_CEL
  /*
   * What the background picture cost over the window: tiles converted,
   * windows built, bands drawn, presentations whose distinct journal
   * lines were more than the bands could take, writes the journal had no
   * room for, and windows the arena had no block for.
   */
  uint32 cnt_decor_tiles;
  uint32 cnt_list_windows;
  uint32 cnt_list_bands;
  uint32 cnt_bands_capped;
  uint32 cnt_journal_full;
  uint32 cnt_list_refused;
  /*
   * Register 0, 2, 8, 1 (size, magnification, display), 5, 6 or 7
   * written to another value while the picture was being scanned. The
   * list is built once, when line 191 is counted, with the last value;
   * the line render took each line with the value of its own line. A
   * picture where this counts is one the two paths can draw apart, and
   * the figure says so rather than letting it pass.
   */
  uint32 cnt_reg_mid;

  /*
   * The small cels of the window: built in all, of which sprite cels,
   * how many more a sprite cost past its first (a refused range, a
   * magnified sprite cut on an odd line, a band boundary), priority
   * runs, and cels the reserve had no block for.
   */
  uint32 cnt_cels;
  uint32 cnt_sprites;
  uint32 cnt_split;
  uint32 cnt_prio;
  uint32 cnt_cels_refused;
#endif
#endif
} vdp_t;

/*
 * The scratch as bytes. Everything that speaks in pixels goes through
 * this; only the background composition uses the word array directly, and
 * it is the reason it is words.
 */
#define VDP_PRIO_BYTES ((uint8 *)sms.vdp.prio_w)

#if SMS_DECOR_CEL
/* The dirty marks as bytes, one per chunk of video memory. */
#define VDP_DECOR_DIRTY ((uint8 *)sms.vdp.decor_dirty_w)
#endif

#if VDP_COUNTERS
#define VDP_COUNT(name) (sms.vdp.cnt_##name++)
#else
#define VDP_COUNT(name) ((void)0)
#endif

/*
 * Counts a colour write that lands while the picture is being scanned.
 * The test on the line is inside the guard and not around the macro, so
 * that a build without counters carries neither the compare nor the
 * count on the port path.
 */
#if VDP_COUNTERS
#define VDP_COUNT_CRAM_MID()                                      \
  do                                                              \
    {                                                             \
      if(sms.vdp.vcount < VDP_ACTIVE_LINES)                       \
        sms.vdp.cnt_cram_mid++;                                   \
    }                                                             \
  while(0)
#else
#define VDP_COUNT_CRAM_MID() ((void)0)
#endif

/*
 * Adds up the rows an invalidation actually threw away, counting only in
 * the build that keeps the counters: the validity byte is 1 or 0, so
 * adding it before clearing it needs no branch on the write path.
 */
#if VDP_COUNTERS
#define VDP_TC_COUNT_INVAL(k) (sms.vdp.cnt_tc_inval += (uint32)sms.vdp.tc_valid[(k)])
#else
#define VDP_TC_COUNT_INVAL(k) ((void)0)
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
 * The two repeatable posts of one rendered line, and the four variants
 * the selector steps through. A variant number is the post it repeats,
 * which is what lets the repetition count be one compare: the control
 * repeats nothing, VDP_PROFILE_ALL repeats the two together.
 *
 * The posts are the two that are idempotent: run twice they write the
 * same bytes and leave the same emulated state. The rest of the line --
 * what vdp_line does around the render, the scanline counter, the pending
 * flags, the scroll latches -- is not among them and cannot be: repeating
 * it would advance the raster twice. Its cost is a residual, obtained by
 * subtracting the two from the published figure, and it is named as one
 * wherever it is printed.
 *
 * There were three. The third was the packing of six bit indexes into
 * words, a pass over the composed line that the picture format no longer
 * has: the composition writes the row directly. It was measured at 29 ms
 * a frame before the short way absorbed most of it, and it is gone
 * rather than kept as a post that would measure nothing.
 *
 * The blank branch of the render -- register 1 bit 6 clear, the picture
 * off -- carries no post either: it fills the row and returns before
 * either of the two. Lines rendered that way pull every displacement
 * down, so the reference regime is a regime with the picture on.
 *
 * WHICH POSTS A LINE ENTERS DEPENDS ON THE LINE, and the figures cannot be
 * read without knowing it: a line that carries no sprite enters the
 * background post only. The key is the lines fast= / scratch= tally, which
 * the counters of the report publish (cnt_line_fast, cnt_line_scratch
 * above) -- a different report from this breakdown, and the two are read
 * together or not at all.
 */
#define VDP_POST_BG      1UL
#define VDP_POST_SPRITES 2UL

#define VDP_PROFILE_CONTROL  0UL
#define VDP_PROFILE_BG       VDP_POST_BG
#define VDP_PROFILE_SPRITES  VDP_POST_SPRITES
#define VDP_PROFILE_ALL      3UL
#define VDP_PROFILE_VARIANTS 4UL

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
 * A colour write converts nothing: the byte is stored, the dirty flag is
 * raised, and the conversion of the 32 entries into the screen's colour
 * table waits for the end of the frame (vdp_clut_take), once, whether
 * one colour moved or all of them. No library call ever sits on this
 * path. The cel's own palette is the identity and is not written here or
 * anywhere after init.
 *
 * A video memory write throws away the decoded row that byte belongs to,
 * and that is the whole of the cache's upkeep: one shift and one store,
 * no test, on the branch that already exists. This macro is the one place
 * in the program that writes the video memory, which is what makes a
 * single point of invalidation enough.
 *
 * The argument is evaluated more than once: the expansion site passes a
 * plain parameter. Nothing here touches sms.z80, the obligation every port
 * hook carries (z80.c, the input and output space).
 * ---------------------------------------------------------------------------
 */
#if SMS_DECOR_CEL
/*
 * The list's share of a video memory write, before the byte lands: the
 * chunk is marked dirty -- one store, no test; the dirty sweep of the
 * next presentation drops the decoded sheet place of that chunk with it
 * -- and, when the chunk is one the list watches (a pattern the picture
 * shows or was just told to show, a pattern the sprite table names, the
 * name table or the sprite table itself) and the byte really changes,
 * the write is handed to vdp_decor_note, which journals it if the
 * picture is being scanned, marks the pattern a name table word or a
 * sprite entry now names, and marks the per-line sprite table stale
 * when the byte is one of the sprite table's. One load and one compare
 * on the port path for every write that is not watched, which is what
 * the unused patterns of a game are. The address is read before the
 * store, since the journal keeps the byte that was there.
 */
#define VDP_DECOR_NOTE(a,v)                                             \
  do                                                                    \
    {                                                                   \
      VDP_DECOR_DIRTY[(a) >> 5] = 1;                                    \
      if((sms.vdp.watch[(a) >> 5] != 0) && (sms.vdp.vram[(a)] != (v)))  \
        vdp_decor_note((a),(v));                                        \
    }                                                                   \
  while(0)
#else
#define VDP_DECOR_NOTE(a,v) ((void)0)
#endif

#define VDP_IO_DATA_WRITE(v)                                            \
  do                                                                    \
    {                                                                   \
      sms.vdp.latch = 0;                                                \
      sms.vdp.read_buf = (uint32)(uint8)(v);                            \
      if(sms.vdp.code == VDP_CODE_CRAM_WRITE)                           \
        {                                                               \
          sms.vdp.cram[sms.vdp.addr & VDP_CRAM_MASK] = (uint8)(v);      \
          sms.vdp.cram_dirty = 1;                                       \
          VDP_COUNT(cram_w);                                            \
          VDP_COUNT_CRAM_MID();                                         \
        }                                                               \
      else                                                              \
        {                                                               \
          VDP_DECOR_NOTE(sms.vdp.addr & VDP_VRAM_MASK,(uint8)(v));      \
          sms.vdp.vram[sms.vdp.addr & VDP_VRAM_MASK] = (uint8)(v);      \
          VDP_TC_COUNT_INVAL(VDP_TC_KEY(sms.vdp.addr));                 \
          sms.vdp.tc_valid[VDP_TC_KEY(sms.vdp.addr)] = 0;               \
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
 * The failures of vdp_init. The first four are one per allocation it
 * makes: the video memory could not be had, an index buffer could not, the
 * bit plane table could not, or the decoded row cache could not. The fifth
 * is not an allocation at all -- the byte order this build assumes is not
 * the machine's. The sixth and seventh are the two pages of the cel list:
 * the picture's and the sprite sheet's.
 *
 * The caller paints a screen per code: its own for the pixel buffer, for
 * the row cache and for the byte order, and the video memory screen for
 * what is left. What is left is the video memory itself and the plane
 * table, which has no screen of its own yet and so is named wrongly -- a
 * defect older than any of this and carried knowingly. The trace names the
 * cause in every case.
 */
#define VDP_ERR_NO_VRAM      (-1)
#define VDP_ERR_NO_PIXELS    (-2)
#define VDP_ERR_NO_PLANES    (-3)
#define VDP_ERR_NO_TILECACHE (-4)
#define VDP_ERR_LANE_ORDER   (-5)
#define VDP_ERR_NO_DECOR     (-6)
#define VDP_ERR_NO_SPRITES   (-7)

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
 * The backdrop as the ground is to be filled with, RGB555: the index the
 * macro above names, repeated on the three components, exactly as the
 * identity palette of the cel emits a pixel of that index. The fill lays
 * a number and not a colour; the screen's colour table turns it into the
 * colour of that entry, so the surround and the picture go through the
 * same table and can never disagree. Read once per frame by the frame
 * loop, on the cold side of its line loop, and never by the render -- the
 * render has the index in a local already.
 *
 * It moves only when register 7 moves. A program that rewrites the colour
 * memory entry it points at repaints nothing here: the table changes, and
 * the ground already filled with the number follows it.
 *
 * This module never draws and never waits: it says what the value is,
 * and the caller paints.
 */
uint16 vdp_backdrop(void);

/*
 * Whether the screen's colour table has to be set again: 1 when a colour
 * byte has moved since the last call, and the 32 entries have then been
 * rebuilt from the colour memory, the flag cleared and the rebuild
 * counted; 0 otherwise, with nothing touched. Called once per frame by
 * the frame loop, at the end of the frame, before the draw: a program
 * that writes its whole palette every frame costs one rebuild per frame,
 * one that leaves it alone costs a load and a compare.
 *
 * The rebuild is here and not on the write path on purpose: 32 colour
 * writes in a frame are one rebuild and not 32, and the port path keeps
 * to a store and a flag. Without the counters the count is not made; the
 * rebuild is, in every build.
 */
int32 vdp_clut_take(void);

/*
 * The screen's colour table as last rebuilt, VDP_CLUT_ENTRIES packed
 * entries -- the 32 colours and then the background entry, index 32 -- in
 * the form SetScreenColors takes (include/3do/graphics.h:829). Valid from
 * init on -- the init rebuilds it over the zeroed colour memory -- and
 * current once vdp_clut_take has been called. A pointer to this module's
 * own array, read by the frame loop and by nothing else; never written
 * through.
 */
const uint32 *vdp_clut(void);

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
 * Where the picture sits on the screen and how big it is, in bitmap
 * pixels: the frame loop sets the clip rectangle of every screen on it
 * once, after init. Every cel position this module writes is then
 * relative to that rectangle. Valid from init on; the four are written
 * and never NULL-tested, a caller passes its own four words.
 */
void vdp_view(int32 *x, int32 *y, int32 *w, int32 *h);

#if SMS_DECOR_CEL
/*
 * A video memory write that the picture watches, called from the data
 * write macro and from nowhere else, with the byte not yet stored. Two
 * things, neither of them a pixel: if the picture is being scanned
 * (line below VDP_ACTIVE_LINES) the write is journaled -- or counted as
 * lost when the journal is full, in which case the picture simply shows
 * its final value from the first band; and if the address is in the name
 * table, the pattern the word names once the byte lands is marked hot,
 * so that a later write of that pattern in the same picture is journaled
 * too, before the conversion has counted the reference.
 */
void vdp_decor_note(uint32 addr, uint32 value);

/*
 * The per-line table of the sprites rebuilt from the attribute table as
 * the video memory holds it, for the lines from the one given on: one
 * walk of the sixty-four entries, stopped on the terminator, each entry
 * marked on the lines it touches -- the first eight of a line admitted,
 * a ninth raising the line's overflow mark -- and the patterns the table
 * names marked for the watch. Called by the line clock when a table
 * byte, register 1, 5 or 6 moved since the last build, before the
 * line's flags are read off it, with that line; and by nothing per
 * pixel. The lines before it keep their admission, which the bands of
 * the presentation still draw. The flags of a line then cost a load
 * each, and the collision costs a decoded row per sprite only on the
 * lines where two admitted sprites overlap horizontally.
 */
void vdp_sprite_scan(uint32 from);

/*
 * The presentation of the background, in three calls the frame loop
 * makes when line 191 has been counted and before the blanking lines.
 *
 * vdp_list_begin undoes the journal, so that the video memory stands as
 * it stood when the frame began, converts every tile the frame's writes
 * left stale, and cuts the picture into bands at the distinct lines of
 * the journal -- one band with no write, up to VDP_LIST_BANDS with
 * seven or more, the lines past the seventh merged into the last band
 * and the merge counted. Returns the band count, at least 1. The tile
 * conversions are here and nowhere else in the frame, which is what the
 * frame loop times as the tiles' cost.
 *
 * vdp_list_band(k) replays the journal up to the first line of band k,
 * converts what those writes touched, and builds the windows of the
 * band's lines: one chain of cel control blocks ending on CCB_LAST, its
 * head returned as an opaque pointer for the draw call, NULL only when
 * the arena had no block left for the band's first window (counted).
 * The caller draws it before asking for band k + 1: the blocks are
 * taken from one arena and the memory is replayed in place. Called with
 * k from 0 to the count minus one, in order.
 *
 * vdp_list_end closes the presentation: the journal is emptied, the hot
 * marks fall, and the writes of the blanking lines start the next one.
 * The video memory stands in its final state: the replay of the last
 * band put it back there, or, when the journal had overflowed, nothing
 * ever moved it (the overflow flag, which falls here).
 */
int32 vdp_list_begin(void);
void *vdp_list_band(int32 k);
void vdp_list_end(void);
#endif /* SMS_DECOR_CEL */

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
 * 191) and the display is on, the line is rendered: on the older path,
 * the background of that line, from the name table, the patterns, the
 * scroll latches and the inhibit bits of register 0, then the sprites of
 * that line over it, into the index buffer at that row -- with the
 * display off, the row is filled with the border colour and neither the
 * name table nor the attribute table is read; on the delivered path, no
 * pixel, only the two sprite bits of the line off the per-line table
 * (vdp_sprite_scan), and a line with the display off marked for the
 * backdrop.
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
