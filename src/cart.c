#include "cart.h"
#include "sms.h"
#include "sys.h"
#include "log.h"

/* For the page tables and their shape: this module is their one writer (cart.h, z80.h). */
#include "z80.h"

/*
 * Block-level file access of the utility library: open, size, close
 * (include/3do/blockfile.h:49-51) and the whole-file load into a caller's
 * buffer (:64). Both libraries it needs are already linked
 * (Makefile:117,122). The file system's own error numbers and the layout of
 * an error word come with it (include/3do/filesystem.h:62,
 * include/3do/operror.h:27-50).
 */
#include "blockfile.h"
#include "filesystem.h"
#include "operror.h"

/*
 * Formatting of the two screen lines. The same kernel sprintf log.c already
 * uses (include/3do/stdio.h), reached through the quoted include so that the
 * convention is the one of the library actually linked.
 */
#include "stdio.h"

/*
 * The pair of names tried at boot, in this order, and the directory they sit
 * in. Three separate constants rather than one path split at run time: the
 * screen names the directory and the bare file names, the file system wants
 * the joined path, and neither should be derived from the other by code that
 * only runs when something has already gone wrong.
 */
#define CART_BOOT_DIR      "roms"
#define CART_BOOT_SMS      "rom.sms"
#define CART_BOOT_GG       "rom.gg"
#define CART_BOOT_PATH_SMS CART_BOOT_DIR "/" CART_BOOT_SMS
#define CART_BOOT_PATH_GG  CART_BOOT_DIR "/" CART_BOOT_GG

/*
 * Screen lines: 37 characters plus the terminator, the bound log_fatal
 * publishes (log.h). Static rather than on the stack so that the strings
 * survive until log_fatal has painted them, which is after this module's
 * functions have returned into cart_boot. A line is never formatted straight
 * into them: it is built in the wider scratch below and then cut to the
 * bound, so that a figure longer than planned shortens the text instead of
 * overrunning the buffer.
 */
#define CART_SCREEN_LINE_MAX 37
#define CART_SCRATCH_MAX     127
static char cart_screen_line1[CART_SCREEN_LINE_MAX + 1];
static char cart_screen_line2[CART_SCREEN_LINE_MAX + 1];
static char cart_scratch[CART_SCRATCH_MAX + 1];

/*
 * The one text each code of the cartridge range is painted with. The same
 * code always shows the same words (log.h, the registry): main.c paints the
 * allocation failure with the two lines below, and the safety-net branch of
 * cart_boot has to repeat them verbatim.
 */
#define CART_SCREEN_NOT_FOUND_1 "no rom found in directory roms"
#define CART_SCREEN_NOT_FOUND_2 "expected rom.sms or rom.gg"
#define CART_SCREEN_ALLOC_1     "cannot allocate the rom buffer"
#define CART_SCREEN_ALLOC_2     "the console refused 1 megabyte"

/*
 * What the last refusal or read failure recorded, for cart_boot to put on
 * the screen. cart_load traces the cause where it is found; the figures are
 * kept here so that the screen can repeat them without cart_load having to
 * know that it is running at boot. Cleared on entry to cart_load, so that a
 * screen never shows the figures of an earlier attempt.
 *
 * A read failure has two shapes and they are kept apart: a negative code the
 * library returned, or a byte count that fell short of the size announced.
 * Neither is ever shown as the other -- a count is not a code, and zero is
 * not an error number.
 */
static uint32      cart_fail_size = 0;
static uint32      cart_fail_limit = 0;
static const char *cart_fail_screen_reason = "";
static int32       cart_fail_err = 0;   /* negative library code, or 0 */
static uint32      cart_fail_got = 0;   /* bytes returned by a short read */
static int32       cart_fail_short = 0; /* 1 when the failure is a short read */

/*
 * The five bounds, as data: the trace reason is the one the matrix of the
 * cartridge module names, the screen reason is a shorter form that fits the
 * painted line beside a seven-digit limit -- the line has 37 columns, so the
 * two texts differ on purpose. Lower case like every other painted string
 * of this program: the font draws every letter as a capital anyway (log.h).
 */
#define CART_REASON_EMPTY   0
#define CART_REASON_BELOW   1
#define CART_REASON_ABOVE   2
#define CART_REASON_BANK    3
#define CART_REASON_POWER   4
#define CART_REASON_COUNT   5

#if LOG_ENABLE && ((LOG_LVL_ERR) <= (LOG_LEVEL))
static const char *const cart_reason_trace[CART_REASON_COUNT] =
  {
    "empty",
    "below minimum",
    "above maximum",
    "not a multiple of bank",
    "not a power of two"
  };
#endif

static const char *const cart_reason_screen[CART_REASON_COUNT] =
  {
    "empty",
    "below minimum",
    "above maximum",
    "not bank aligned",
    "not power of two"
  };

/*
 * Whether an error word says "no such file", as opposed to any other way an
 * open can fail. The file system publishes its error numbers
 * (include/3do/filesystem.h:62, ER_Fs_NoFile) and the error word carries the
 * number in its low eight bits with the module's identifier in bits 13 to 24
 * (include/3do/operror.h:27-50, ER_FSYS at :98). Only those two fields are
 * compared: the severity and class fields are the library's to choose, and a
 * missing file is a missing file whichever it picked.
 */
static int32
cart_err_is_no_file(Err err)
{
  uint32 word = (uint32)err;
  uint32 id = (word >> ERR_IDSHIFT) & ((1UL << ERR_IDSIZE) - 1UL);
  uint32 code = word & ((1UL << ERR_ERRSIZE) - 1UL);

  return (id == (uint32)ER_FSYS) && (code == (uint32)ER_Fs_NoFile);
}

/*
 * The extension of a name, deduced by looking at its last characters only:
 * the file system does not tell what kind of file it holds, and the name is
 * the one thing the caller has chosen. Case is respected as given -- the
 * boot names are lower case and a later selector will pass names as they
 * are on the disc.
 */
static int32
cart_name_ends_with(const char *name,
                    const char *suffix)
{
  uint32 name_len = 0;
  uint32 suffix_len = 0;
  uint32 i;

  while(name[name_len] != '\0')
    name_len++;
  while(suffix[suffix_len] != '\0')
    suffix_len++;

  if(suffix_len > name_len)
    return 0;

  for(i = 0; i < suffix_len; i++)
    if(name[name_len - suffix_len + i] != suffix[i])
      return 0;

  return 1;
}

static int32
cart_system_from_name(const char *name)
{
  if(cart_name_ends_with(name,".sms"))
    return SYS_SMS;
  if(cart_name_ends_with(name,".gg"))
    return SYS_GG;
  return SYS_NONE;
}

/*
 * Names for the trace only, and built only when a trace that reads them is:
 * a silent build keeps neither the strings nor the functions.
 *
 * cart_system_name is public (cart.h): it is the one spelling of the profile,
 * for this module's lines and for the init line of every subsystem brought up
 * after the load. Guarded by LOG_ENABLE alone because it serves every level.
 * cart_system_ext stays private and guarded at WARN, the lowest level that
 * reads it -- the mismatch warning; a forced build reads it nowhere, since
 * every extension-quoting line sits in the unforced branch.
 */
#if LOG_ENABLE
const char *
cart_system_name(int32 system)
{
  switch(system)
    {
    case SYS_SMS: return "SMS";
    case SYS_GG:  return "GG";
    default:      return "none";
    }
}
#endif

#if !SMS_FORCE_SYSTEM && LOG_ENABLE && ((LOG_LVL_WARN) <= (LOG_LEVEL))
static const char *
cart_system_ext(int32 system)
{
  switch(system)
    {
    case SYS_SMS: return ".sms";
    case SYS_GG:  return ".gg";
    default:      return "none";
    }
}
#endif

/*
 * Copies a string into a bounded destination, cut at the capacity given
 * (terminator excluded). Used for the name kept in the structure and for the
 * two screen lines: in both places the cut is a bound on what is kept, and
 * a longer text loses its tail rather than the buffer its neighbour.
 */
static void
cart_copy_bounded(char       *dst,
                  const char *src,
                  uint32      max)
{
  uint32 i;

  for(i = 0; (i < max) && (src[i] != '\0'); i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

/*
 * What every failure of cart_load leaves behind: no ROM. The buffer may have
 * been written by a read that then proved short, so the fields that describe
 * its content are the ones cleared -- the buffer pointer and the capacity are
 * cart_init's and stay.
 */
static void
cart_invalidate(void)
{
  sms.cart.size = 0;
  sms.cart.system = SYS_NONE;
  sms.cart.region = 0;
  sms.cart.name[0] = '\0';
  sms.cart.banks = 0;
  sms.cart.bank_mask = 0;
}

/*
 * Applies the five bounds in their fixed order and records the first one
 * broken. Returns the reason index, or -1 when the size is acceptable. Pure
 * arithmetic on the size alone, done before a byte is read: an image is
 * refused for what the file system said of it, never for what a partial
 * read would have shown.
 */
static int32
cart_check_size(uint32  size,
                uint32 *limit)
{
  uint32 power;

  if(size == 0)
    {
      *limit = CART_ROM_MIN;
      return CART_REASON_EMPTY;
    }

  if(size < CART_ROM_MIN)
    {
      *limit = CART_ROM_MIN;
      return CART_REASON_BELOW;
    }

  if(size > CART_ROM_CAPACITY)
    {
      *limit = CART_ROM_CAPACITY;
      return CART_REASON_ABOVE;
    }

  if((size & (CART_BANK_SIZE - 1UL)) != 0)
    {
      *limit = CART_BANK_SIZE;
      return CART_REASON_BANK;
    }

  if((size & (size - 1UL)) != 0)
    {
      /*
       * The limit named is the power of two the image would have had to
       * reach: the nearest one above, so that the reader sees how far the
       * file is from a shape the mapper can mask.
       */
      power = CART_ROM_MIN;
      while(power < size)
        power <<= 1;
      *limit = power;
      return CART_REASON_POWER;
    }

  *limit = 0;
  return -1;
}

/*
 * What the ROM header says, as far as this module reads it: whether the
 * eight-byte magic was found, at which of the three offsets, and what the
 * high nibble of the last header byte carries. The meaning of that nibble --
 * 0x3/0x4 Master System, 0x5/0x6/0x7 Game Gear -- is emulated semantics and
 * nothing else: no Sega document describes a region or system code in the
 * header, and the sole source is TotalSMS (TotalSMS/src/core/sms.c:34-41,
 * :564-567). The low nibble is a size code and is neither read nor
 * validated: the bounds on the size are cart_check_size's, on the file
 * system's figure.
 */
typedef struct
{
  int32  found;  /* the magic sits at off */
  uint32 off;    /* one of the three header offsets */
  uint32 nibble; /* high nibble of the last header byte, 0 until found */
  int32  system; /* SYS_* the nibble names, SYS_NONE when absent or mute */
} cart_header_t;

static void
cart_parse_header(const uint8   *rom,
                  uint32         size,
                  cart_header_t *h)
{
  /*
   * The three offsets the header can sit at, searched in the order the BIOS
   * checks them (TotalSMS/src/core/sms.c:77-100, find_rom_header_offset).
   *
   * Everything is read byte by byte, on purpose and for the big-endian
   * target: the header is emulated data, laid out little-endian whatever
   * the host, and TotalSMS's own word-sized read of it extracts the region
   * only in its little-endian branch -- the big-endian branch, the one
   * matching this target, stops at a todo and reads the size alone
   * (sms.c:238-246). So the magic is compared one byte at a time and the
   * nibble is taken from rom[off + 15] by a shift; no 32-bit word is ever
   * read out of the image.
   */
  static const uint32 offs[3] = { 0x7FF0UL, 0x3FF0UL, 0x1FF0UL };
  static const char magic[9] = "TMR SEGA";
  const uint8 *p;
  uint32 i;
  uint32 k;

  h->found = 0;
  h->off = 0;
  h->nibble = 0;
  h->system = SYS_NONE;

  for(i = 0; i < 3; i++)
    {
      /*
       * The five bounds guarantee 32 kilobytes at least, so all three
       * offsets fit any image this function ever sees; the check stays so
       * that the function assumes nothing about who validated what.
       */
      if(size < offs[i] + 16UL)
        continue;

      p = rom + offs[i];
      for(k = 0; k < 8; k++)
        if(p[k] != (uint8)magic[k])
          break;
      if(k == 8)
        {
          h->found = 1;
          h->off = offs[i];
          break;
        }
    }

  if(!h->found)
    return;

  h->nibble = ((uint32)rom[h->off + 15UL]) >> 4;

  /*
   * Only the five known values conclude (TotalSMS/src/core/sms.c:34-41).
   * TotalSMS ends on a catch-all -- anything else is a Master System
   * (sms.c:568-571) -- which is not followed here: concluding SMS out of a
   * nibble that names nothing would dress the absence of a criterion as a
   * detection. Out of these values the header is mute and the caller falls
   * back to the extension.
   */
  if((h->nibble == 0x3UL) || (h->nibble == 0x4UL))
    h->system = SYS_SMS;
  else if((h->nibble >= 0x5UL) && (h->nibble <= 0x7UL))
    h->system = SYS_GG;
}

/*
 * Header: cart.h, where the once-only rule and the position in the boot
 * sequence are written out.
 */
Err
cart_init(void)
{
  /*
   * Called twice, the second call would take a second megabyte the program
   * has no use for and could not afford. Answered as z80_init answers the
   * same mistake: a warning and the first buffer kept.
   */
  if(sms.cart.rom != NULL)
    {
      LOG_WARN(LOG_CAT_CART,("init already done, ignored"));
      return 0;
    }

  /*
   * A megabyte, whole, through the one door to memory this program has, so
   * that the block is in the boot footprint and inside the seal. The fill
   * flag clears it: what a small ROM leaves unused is then a known value
   * rather than what the previous program left behind (idiom: z80.c, the
   * emulated address space).
   */
  sms.cart.rom = (uint8 *)sys_alloc("cart_rom",
                                    (int32)CART_ROM_CAPACITY,
                                    MEMTYPE_DRAM | MEMTYPE_FILL);
  if(sms.cart.rom == NULL)
    {
      /*
       * sys_alloc has already said how much was asked for and why it failed.
       * What is added is what the block was for, which only this module
       * knows.
       */
      LOG_ERR(LOG_CAT_CART,("init failed: no rom buffer"));
      return CART_ERR_NO_BUFFER;
    }

  sms.cart.capacity = CART_ROM_CAPACITY;
  cart_invalidate();

  return 0;
}

/*
 * Header: cart.h, where the no-byte-read property and the meaning of a
 * missing file are written out.
 */
Err
cart_identify(const char  *name,
              cart_info_t *info)
{
  BlockFile bf;
  Err err;
  int32 size;

  if((name == NULL) || (info == NULL))
    {
      LOG_ERR(LOG_CAT_CART,("identify refused: no name or no record"));
      return CART_ERR_NOT_FOUND;
    }

  /*
   * The open takes a non-const name because the library declares it so
   * (include/3do/blockfile.h:49); nothing writes through it.
   *
   * A file that is not there is one outcome, traced at debug level because
   * the caller may well try another name; any other failure of the open --
   * a device error, a damaged directory -- is not a lookup that came back
   * empty and is reported as a read failure.
   */
  err = OpenBlockFile((char *)name,&bf);
  if(err < 0)
    {
      if(cart_err_is_no_file(err))
        {
          LOG_DBG(LOG_CAT_CART,("%s not found err=0x%lx",
                                name,(unsigned long)err));
          return CART_ERR_NOT_FOUND;
        }

      cart_fail_err = err;
      cart_fail_short = 0;
      LOG_ERR(LOG_CAT_CART,("open %s failed err=0x%lx",
                            name,(unsigned long)err));
      return CART_ERR_READ;
    }

  size = GetBlockFileSize(&bf);
  CloseBlockFile(&bf);

  /*
   * A negative size is the library's way of saying the record could not be
   * read. It is not a size of zero, which would be refused for a different
   * reason with a different picture.
   */
  if(size < 0)
    {
      cart_fail_err = size;
      cart_fail_short = 0;
      LOG_ERR(LOG_CAT_CART,("%s size err=%ld",name,(long)size));
      return CART_ERR_READ;
    }

  info->size = (uint32)size;
  info->system = cart_system_from_name(name);

  return 0;
}

/*
 * Header: cart.h, where the never-partial rule and the order of the bounds
 * are written out.
 */
Err
cart_load(const char *name)
{
  cart_info_t info;
  cart_header_t hdr;
  Err err;
  int32 reason;
  uint32 limit;
  int32 loaded;
  void *got;
  int32 system;

  cart_fail_size = 0;
  cart_fail_limit = 0;
  cart_fail_screen_reason = "";
  cart_fail_err = 0;
  cart_fail_got = 0;
  cart_fail_short = 0;

  if(name == NULL)
    {
      LOG_ERR(LOG_CAT_CART,("load refused: no name"));
      return CART_ERR_NOT_FOUND;
    }

  if(sms.cart.rom == NULL)
    {
      LOG_ERR(LOG_CAT_CART,("load %s refused: no rom buffer",name));
      return CART_ERR_NO_BUFFER;
    }

  /*
   * The file is opened twice: once here, to learn its size without reading
   * a byte, and once more inside the load below. The library's load alone
   * would refuse a file larger than the buffer but not say by how much, and
   * would say nothing of the four other bounds; the size first is what lets
   * every bound be checked, and named, before the first byte moves. Two
   * opens of a file that is loaded once at boot cost nothing worth a third
   * entry point.
   */
  err = cart_identify(name,&info);
  if(err < 0)
    return err;

  /*
   * Bounds first, on the size the file system reported, and before any
   * derived figure -- the bank count, the mask -- is computed from it. An
   * image refused here has cost one open and no read.
   */
  reason = cart_check_size(info.size,&limit);
  if(reason >= 0)
    {
      cart_fail_size = info.size;
      cart_fail_limit = limit;
      cart_fail_screen_reason = cart_reason_screen[reason];
      LOG_ERR(LOG_CAT_CART,("rom refused size=%lu limit=%lu reason=%s",
                            (unsigned long)info.size,
                            (unsigned long)limit,
                            cart_reason_trace[reason]));
      return CART_ERR_SIZE;
    }

  /*
   * The whole file into the resident buffer, with the buffer's own size as
   * the bound the library checks (include/3do/blockfile.h:64;
   * docs/3do/3do_portfolio_2.5.md:2299-2320). From here on the buffer may
   * have been written, so every failure below leaves the structure
   * describing no ROM at all: a read that returns fewer bytes than the size
   * checked above is a failure and not a shorter image.
   */
  loaded = 0;
  got = LoadFileHere(name,&loaded,sms.cart.rom,(int32)sms.cart.capacity);
  if((got == NULL) && (loaded < 0))
    {
      cart_invalidate();
      cart_fail_err = loaded;
      LOG_ERR(LOG_CAT_CART,("read %s failed err=%ld",name,(long)loaded));
      return CART_ERR_READ;
    }

  if((got == NULL) || ((uint32)loaded != info.size))
    {
      cart_invalidate();
      cart_fail_short = 1;
      cart_fail_got = (loaded < 0) ? 0UL : (uint32)loaded;
      cart_fail_size = info.size;
      LOG_ERR(LOG_CAT_CART,("read %s failed: got %ld of %lu bytes",
                            name,(long)loaded,(unsigned long)info.size));
      return CART_ERR_READ;
    }

  /*
   * The image is whole in the buffer: read its header and fix the profile,
   * here and never again -- after this commit the profile is read-only
   * (cart.h). Precedence: the forced build constant, then the header, then
   * the extension. The header speaks for the cartridge itself, which is why
   * it wins over the file name; the imposed system winning over both follows
   * TotalSMS (TotalSMS/src/core/sms.c:560-563). The region kept is the
   * header's nibble when the header decided, 0 otherwise: a nibble that
   * named no system is not a region worth keeping, only worth tracing.
   */
  cart_parse_header(sms.cart.rom,info.size,&hdr);

#if SMS_FORCE_SYSTEM
  system = SMS_FORCE_SYSTEM;
#else
  system = (hdr.system != SYS_NONE) ? hdr.system : info.system;
#endif

  sms.cart.size = info.size;
  sms.cart.system = system;
  sms.cart.region = (hdr.system != SYS_NONE) ? hdr.nibble : 0UL;
  cart_copy_bounded(sms.cart.name,name,CART_NAME_MAX);

  /*
   * The mapper's two figures, from the size alone: a whole number of banks
   * (fourth bound) that is a power of two (fifth), so the mask is full and
   * a bank number wraps by it (docs/pseudocodes/30_mappers.md:197-198).
   */
  sms.cart.banks = info.size / CART_BANK_SIZE;
  sms.cart.bank_mask = sms.cart.banks - 1UL;

  LOG_INFO(LOG_CAT_CART,("load %s size=%lu bytes ok",
                         name,(unsigned long)sms.cart.size));

  if(hdr.found)
    LOG_INFO(LOG_CAT_CART,("header found at 0x%04lx magic=TMR SEGA",
                           (unsigned long)hdr.off));

  /*
   * One system= line, always, and it names the criterion that decided, not
   * just the verdict. The forced branch is under its own #if so that the
   * default binary carries no trace of the constant, not even the string.
   */
#if SMS_FORCE_SYSTEM
  LOG_INFO(LOG_CAT_CART,("system=%s (forced by build)",
                         cart_system_name(sms.cart.system)));
#else
  if(hdr.system != SYS_NONE)
    {
      LOG_INFO(LOG_CAT_CART,("system=%s (header region=0x%lx) region=0x%lx",
                             cart_system_name(sms.cart.system),
                             (unsigned long)hdr.nibble,
                             (unsigned long)hdr.nibble));

      /*
       * A disagreement with the extension does not stop a load that is
       * already whole; it is said once, and the header wins.
       */
      if((info.system != SYS_NONE) && (info.system != hdr.system))
        LOG_WARN(LOG_CAT_CART,("system mismatch header=%s ext=%s - header wins",
                               cart_system_name(hdr.system),
                               cart_system_ext(info.system)));
    }
  else if(hdr.found)
    LOG_INFO(LOG_CAT_CART,("system=%s (ext %s, header mute region=0x%lx)",
                           cart_system_name(sms.cart.system),
                           cart_system_ext(info.system),
                           (unsigned long)hdr.nibble));
  else
    LOG_INFO(LOG_CAT_CART,("system=%s (ext %s, header absent)",
                           cart_system_name(sms.cart.system),
                           cart_system_ext(info.system)));
#endif

  /*
   * Nothing deciding is not a refusal: the list of refusals is the five
   * bounds and nothing else, and the image is loaded whole. What the caller
   * learns is that the system will have to come from elsewhere.
   */
  if(sms.cart.system == SYS_NONE)
    LOG_WARN(LOG_CAT_CART,("%s: unknown extension, system not deduced",name));

  /*
   * What the cartridge is, for the mapper: size, banks, the mapper kept and
   * on what basis, and the RAM. The mapper is never detected -- no source
   * this repository trusts gives a criterion telling a Sega mapper from a
   * Codemasters one or from none (30_mappers.md section 9, note D; human
   * decision of 2026-08-15) -- so the line names the build's choice
   * (SMS_MAPPER, common.h) and says on what basis, rather than claim a
   * detection that did not happen. One text per value, under #if, so
   * that a binary carries only its own. sram=no says the same of the
   * cartridge RAM: nothing in the header declares it; in the Sega build
   * the RAM exists on the bus and its use is traced the day a program
   * maps it in.
   */
#if SMS_MAPPER == MAPPER_SEGA
  LOG_INFO(LOG_CAT_CART,("size=%lu banks=%lu mapper=sega (assumed, v1: not detected) sram=no",
                         (unsigned long)sms.cart.size,
                         (unsigned long)sms.cart.banks));
#elif SMS_MAPPER == MAPPER_CODEMASTERS
  LOG_INFO(LOG_CAT_CART,("size=%lu banks=%lu mapper=codemasters (best effort, build constant) sram=no",
                         (unsigned long)sms.cart.size,
                         (unsigned long)sms.cart.banks));
#else
  LOG_INFO(LOG_CAT_CART,("size=%lu banks=%lu mapper=none (build constant) sram=no",
                         (unsigned long)sms.cart.size,
                         (unsigned long)sms.cart.banks));
#endif

  return 0;
}

/*
 * ---------------------------------------------------------------------------
 * The bus, as the machine wires it before any mapper traffic.
 *
 * Of the 64k the Z80 addresses, 48k are wired to the cartridge -- three 16k
 * slots, whose control registers select banks 0, 1 and 2 at reset -- and
 * the top 16k are the console's work RAM, 8k of real memory seen twice
 * (docs/pseudocodes/30_mappers.md section 1; docs/sms_gg/mappers.md, the
 * community reverse-engineering page it rests on;
 * TotalSMS/src/core/sms_bus.c:161-172 for the fold). At reset the three
 * visible banks are the first three of the image, so the map below is the
 * image laid out linearly -- no bank arithmetic, no mask, nothing selected:
 * pagination begins the day the mapper registers gain their effect, and
 * that day is not this one.
 *
 * Everything is a pointer into something resident. Projecting a bank is
 * changing a table entry, never copying (TotalSMS/src/core/sms_bus.c:50-70);
 * the mirrors of the work RAM are eight entries repeated, by a mask and
 * never a modulo (sms_bus.c:161-172); and the two pages below back what the
 * cartridge does not cover, the same way TotalSMS backs its unmapped space
 * (sms_bus.c:13-14, :123-129).
 * ---------------------------------------------------------------------------
 */

/*
 * The work RAM and the two fixed pages.
 *
 * cart_bus_fixed answers every read of a page the ROM does not reach: const,
 * so the compiler holds that nothing ever writes it, and zero-filled by the
 * language -- a static with no initialiser -- so its value costs no code.
 * The value read is 0x00, the choice TotalSMS makes for the same page
 * (sms_bus.c:13, a const zero bank); what an unwired bus really floats to
 * is not documented anywhere this repository trusts, so the emulated
 * semantics are kept, and kept cited.
 *
 * cart_bus_scrap absorbs every write aimed at the ROM's 48k: the machine
 * has no memory there, the store has to land somewhere host-side, and a
 * page nothing ever reads is that somewhere (sms_bus.c:14). One page, not
 * 48: every write entry of the region points at the same kilobyte.
 *
 * cart_work_ram is the console's 8k, allocated once at the first install
 * and kept across any later one: a second cartridge gets the same RAM, the
 * way the console's own chips survive a cartridge swap. Taken through
 * sys_alloc before the seal, so it stands in the boot footprint.
 */
#define CART_WORK_RAM_SIZE 8192UL

/* Pages per 16k slot, wired ROM pages (three slots), RAM pages above. */
#define CART_BUS_SLOT_PAGES (CART_BANK_SIZE / Z80_PAGE_SIZE)
#define CART_BUS_ROM_PAGES  (3UL * CART_BUS_SLOT_PAGES)
#define CART_BUS_RAM_PAGES  (Z80_PAGE_COUNT - CART_BUS_ROM_PAGES)

/* The fold: 8k of RAM behind 16 entries, one mask and never a modulo. */
#define CART_BUS_RAM_MASK ((CART_WORK_RAM_SIZE / Z80_PAGE_SIZE) - 1UL)

static const uint8 cart_bus_fixed[Z80_PAGE_SIZE];
static uint8       cart_bus_scrap[Z80_PAGE_SIZE];
static uint8      *cart_work_ram = NULL;

/* First page of each slot, and of the work RAM above them. */
#define CART_BUS_SLOT0_PAGE 0UL
#define CART_BUS_SLOT1_PAGE CART_BUS_SLOT_PAGES
#define CART_BUS_SLOT2_PAGE (2UL * CART_BUS_SLOT_PAGES)

#if SMS_MAPPER == MAPPER_SEGA
/* The bits of $FFFC (docs/sms_gg/mappers.md:35-44, community source). */
#define CART_MAPPER_FFFC_ROM_WRITE   0x80U /* ignored, warned */
#define CART_MAPPER_FFFC_RAM_C000    0x10U /* ignored, warned */
#define CART_MAPPER_FFFC_RAM_8000    0x08U /* cartridge RAM on slot 2 */
#define CART_MAPPER_FFFC_RAM_BANK    0x04U /* which of its two banks */
#define CART_MAPPER_FFFC_BANK_SHIFT  0x03U /* ignored, warned, bit by bit */
#endif

#if SMS_MAPPER == MAPPER_CODEMASTERS
/* The on-cart RAM request, bit 7 of the slot 1 register (docs/sms_gg/
   mappers.md:121, community source): declared, stripped, not honoured. */
#define CART_MAPPER_CM_RAM_BIT       0x80U
/* What is left of a written value once that bit is stripped. */
#define CART_MAPPER_CM_BANK_BITS     0x7FU
#endif

#if SMS_MAPPER != MAPPER_NONE
static void cart_mapper_reset(void);
#endif

/*
 * Header: cart.h, where the map this leaves and the moment it runs are
 * written out.
 */
Err
cart_bus_install(void)
{
  uint32 page;
  uint32 rom_pages;

  /*
   * A guard, not a path: cart_boot only calls this after a successful load,
   * and a later selector must hold the same order. Tables filled from no
   * image would be 48 entries of fixed page dressed up as a map.
   */
  if(sms.cart.size == 0)
    {
      LOG_ERR(LOG_CAT_BUS,("install refused: no rom loaded"));
      return CART_ERR_NO_ROM;
    }

  if(cart_work_ram == NULL)
    {
      /*
       * DRAM like every emulated memory, and filled: a boot starts on a RAM
       * of zeroes instead of whatever the previous program left, which is
       * what makes two boots comparable.
       * sys_alloc's own line is the trace of this allocation.
       */
      cart_work_ram = (uint8 *)sys_alloc("work_ram",
                                         (int32)CART_WORK_RAM_SIZE,
                                         MEMTYPE_DRAM | MEMTYPE_FILL);
      if(cart_work_ram == NULL)
        {
          LOG_ERR(LOG_CAT_BUS,
                  ("install failed: no memory for the work ram"));
          return CART_ERR_WORK_RAM;
        }
    }

#if SMS_MAPPER == MAPPER_SEGA
  /*
   * The cartridge RAM, on the same terms: once, filled, traced by the
   * allocator, kept across a later installation the way the RAM soldered
   * on a cartridge would be swapped with it -- which the console cannot
   * tell from keeping it, since nothing persists. Two banks, so that bit 2
   * of $FFFC has something to select (cart.h). The Sega mapper's alone:
   * the other two builds have nothing that would ever map it in.
   */
  if(sms.cart.cart_ram == NULL)
    {
      sms.cart.cart_ram = (uint8 *)sys_alloc("cart_ram",
                                             (int32)CART_RAM_SIZE,
                                             MEMTYPE_DRAM | MEMTYPE_FILL);
      if(sms.cart.cart_ram == NULL)
        {
          LOG_ERR(LOG_CAT_BUS,
                  ("install failed: no memory for the cartridge ram"));
          return CART_ERR_WORK_RAM;
        }
    }
#endif /* SMS_MAPPER == MAPPER_SEGA */

  /*
   * How much of the image the wired 48k shows: all of it when it fits, the
   * first three banks -- the reset banks 0/1/2 -- when it does not. The
   * size has already cleared the five bounds of cart_load, so the shift
   * loses nothing.
   */
  rom_pages = sms.cart.size >> Z80_PAGE_BITS;
  if(rom_pages > CART_BUS_ROM_PAGES)
    rom_pages = CART_BUS_ROM_PAGES;

  /*
   * The wired 48k: reads from the ROM where it reaches, from the fixed page
   * where it does not; writes absorbed whole. Filling shape:
   * TotalSMS/src/core/sms_bus.c:123-129, :132-137.
   */
  for(page = 0; page < CART_BUS_ROM_PAGES; page++)
    {
      z80_rmap[page] = (page < rom_pages)
                       ? (sms.cart.rom + (page * Z80_PAGE_SIZE))
                       : cart_bus_fixed;
      z80_wmap[page] = cart_bus_scrap;
    }

  /*
   * The top 16k: the 8k of work RAM seen twice, read and write alike, each
   * mirror one repointed entry (TotalSMS/src/core/sms_bus.c:161-172). In
   * the Sega build a write to $FFFC-$FFFF lands here like any other --
   * and then, from Z80_WR8 alone, reaches cart_mapper_write below.
   */
  for(page = 0; page < CART_BUS_RAM_PAGES; page++)
    {
      uint8 *slice = cart_work_ram +
                     ((page & CART_BUS_RAM_MASK) * Z80_PAGE_SIZE);

      z80_rmap[CART_BUS_ROM_PAGES + page] = slice;
      z80_wmap[CART_BUS_ROM_PAGES + page] = slice;
    }

  /*
   * The one line of a successful install, in the bus's own category: how
   * many pages the image backs, how many the RAM does, and how many fell to
   * the fixed page -- enough to check the map against the size the load
   * announced without reading a single entry.
   */
  LOG_INFO(LOG_CAT_BUS,("map ok rom_pages=%lu ram_pages=%lu unused=%lu",
                        (unsigned long)rom_pages,
                        (unsigned long)CART_BUS_RAM_PAGES,
                        (unsigned long)(CART_BUS_ROM_PAGES - rom_pages)));

  /*
   * What the build's mapper adds after the map (cart.h, cart_bus_install):
   * the Sega reset; the Codemasters notice then its reset; or, for no
   * mapper, the one warning of the fallback in place of any reset -- the
   * linear map above is the whole of it, and nothing moves it again.
   */
#if SMS_MAPPER == MAPPER_SEGA
  cart_mapper_reset();
#elif SMS_MAPPER == MAPPER_CODEMASTERS
  LOG_INFO(LOG_CAT_CART,("mapper=codemasters (best effort, not a release commitment)"));
  cart_mapper_reset();
#else
  LOG_WARN(LOG_CAT_CART,("mapper unknown, falling back to fixed banks 0/1/2"));
#endif

  return 0;
}

#if SMS_MAPPER != MAPPER_NONE
/*
 * ---------------------------------------------------------------------------
 * The mapper: the cold half of a write at a register address. The hot half
 * is CART_MAPPER_TRIGGER in Z80_WR8 (z80_ops.h); the contract, the
 * semantics and their sources are written out above cart_mapper_write in
 * cart.h. Which mapper is compiled is the build's (SMS_MAPPER, common.h):
 * the projection below serves both, then each mapper has its own block,
 * and a build serving no mapper compiles none of this.
 * ---------------------------------------------------------------------------
 */

/*
 * Points the pages of one slot at one bank of the image. The slot's first
 * page and the first page of the bank it shows are given apart, because
 * the Sega mapper's slot 0 shows a bank from its second kilobyte: page 0
 * of the address space is never repointed there (docs/sms_gg/
 * mappers.md:62). The Codemasters mapper passes 0 and repoints the whole
 * slot. The write entries of a ROM slot stay on the scrap page, which the
 * install left there and the Sega RAM path restores.
 */
static void
cart_mapper_project(uint32 first_page, uint32 page_count,
                    uint32 bank, uint32 bank_first_page)
{
  const uint8 *base;
  uint32 i;

  /*
   * The wrap, here and nowhere else, so that no caller can hand a bank the
   * image does not have: the reset value of $FFFF is 2 whatever the image,
   * and on a 32k image -- two banks -- it is bank 0 the ROM comes back on
   * when the cartridge RAM leaves slot 2 (the mirror, 30_mappers.md
   * section 4). A bank register is stored masked already; the mask is
   * idempotent on it.
   */
  bank &= sms.cart.bank_mask;
  base = sms.cart.rom + (bank * CART_BANK_SIZE);

  for(i = 0; i < page_count; i++)
    {
      z80_rmap[first_page + i] = base + ((bank_first_page + i) * Z80_PAGE_SIZE);
      z80_wmap[first_page + i] = cart_bus_scrap;
    }
}
#endif /* SMS_MAPPER != MAPPER_NONE */

#if SMS_MAPPER == MAPPER_SEGA
/*
 * ---------------------------------------------------------------------------
 * The Sega mapper, the four registers at $FFFC-$FFFF.
 *
 * Semantics emulated, never form borrowed:
 * TotalSMS/src/core/sms_bus.c:73-107 (the three slots, slot 0's fixed
 * first page), :110-121 (the cartridge RAM on slot 2), :375-412 (the four
 * writes). Where TotalSMS divides, this wraps by a mask (30_mappers.md
 * section 4); where it keeps a uint8_t bank count, this keeps a word.
 * ---------------------------------------------------------------------------
 */

/*
 * Slot 2 as the cartridge RAM: the bank bit 2 of $FFFC names, readable and
 * writable, each page its own kilobyte of it (sms_bus.c:110-121).
 */
static void
cart_mapper_project_ram(void)
{
  uint8 *base = sms.cart.cart_ram +
                ((sms.cart.mapper_fffc & CART_MAPPER_FFFC_RAM_BANK)
                 ? CART_RAM_BANK_SIZE : 0UL);
  uint32 i;

  for(i = 0; i < CART_BUS_SLOT_PAGES; i++)
    {
      z80_rmap[CART_BUS_SLOT2_PAGE + i] = base + (i * Z80_PAGE_SIZE);
      z80_wmap[CART_BUS_SLOT2_PAGE + i] = base + (i * Z80_PAGE_SIZE);
    }
}

/*
 * The four registers at their reset values, traced as a state and not as
 * an event, and no page moved: the linear map the install just wrote is
 * that state (cart.h, cart_bus_install). A re-installation over a program
 * that had the cartridge RAM on slot 2 comes through here too: the tables
 * are rewritten whole above, so bit 3 is dropped without a slot=2 off
 * line -- the reset line is the event.
 */
static void
cart_mapper_reset(void)
{
  sms.cart.mapper_fffc = CART_MAPPER_RESET_FFFC;
  sms.cart.mapper_fffd = CART_MAPPER_RESET_FFFD;
  sms.cart.mapper_fffe = CART_MAPPER_RESET_FFFE;
  sms.cart.mapper_ffff = CART_MAPPER_RESET_FFFF;

  LOG_INFO(LOG_CAT_BUS,("mapper reset fffc=0x%02lx fffd=0x%02lx fffe=0x%02lx ffff=0x%02lx",
                        (unsigned long)sms.cart.mapper_fffc,
                        (unsigned long)sms.cart.mapper_fffd,
                        (unsigned long)sms.cart.mapper_fffe,
                        (unsigned long)sms.cart.mapper_ffff));
}

/*
 * The control register. Every effect is on an edge of a bit, so a program
 * rewriting the same value costs the compare above and nothing else, and a
 * program that keeps a bit set while it changes the others repeats no
 * warning.
 */
static void
cart_mapper_write_fffc(uint32 value)
{
  uint32 old = sms.cart.mapper_fffc;
  uint32 changed = value ^ old;
  uint32 rose = value & ~old;

  sms.cart.mapper_fffc = value;

  /*
   * Slot 2 moves on two occasions: bit 3 changing, or bit 2 changing while
   * bit 3 is set. Bit 2 changing while bit 3 is clear is latched and
   * silent: it names the bank the next rising edge of bit 3 will map, and
   * that edge's line names it.
   */
  if((changed & CART_MAPPER_FFFC_RAM_8000) ||
     ((changed & CART_MAPPER_FFFC_RAM_BANK) &&
      (value & CART_MAPPER_FFFC_RAM_8000)))
    {
      if(value & CART_MAPPER_FFFC_RAM_8000)
        {
          cart_mapper_project_ram();
          LOG_INFO(LOG_CAT_BUS,("cart ram slot=2 on bank=%lu",
                                (unsigned long)((value & CART_MAPPER_FFFC_RAM_BANK) ? 1 : 0)));
        }
      else
        {
          cart_mapper_project(CART_BUS_SLOT2_PAGE,CART_BUS_SLOT_PAGES,
                              sms.cart.mapper_ffff,0UL);
          LOG_INFO(LOG_CAT_BUS,("cart ram slot=2 off"));
        }
    }

  if(rose & CART_MAPPER_FFFC_ROM_WRITE)
    LOG_WARN(LOG_CAT_BUS,("mapper feature ignored bit=7"));
  if(rose & CART_MAPPER_FFFC_RAM_C000)
    LOG_WARN(LOG_CAT_BUS,("mapper feature ignored bit=4"));
  if(rose & 0x02U)
    LOG_WARN(LOG_CAT_BUS,("mapper feature ignored bit=1"));
  if(rose & 0x01U)
    LOG_WARN(LOG_CAT_BUS,("mapper feature ignored bit=0"));
}

/*
 * Header: cart.h.
 */
void
cart_mapper_write(uint16 addr, uint8 value)
{
  uint32 bank = (uint32)value & sms.cart.bank_mask;

  switch(addr)
    {
    case 0xFFFD:
      if(bank == sms.cart.mapper_fffd)
        return;
      sms.cart.mapper_fffd = bank;
      LOG_DBG(LOG_CAT_BUS,("bank slot=0 value=0x%02lx masked=0x%02lx",
                           (unsigned long)value,(unsigned long)bank));
      /* Pages 1..15 of the slot, from the bank's second kilobyte on. */
      cart_mapper_project(CART_BUS_SLOT0_PAGE + 1UL,CART_BUS_SLOT_PAGES - 1UL,
                          bank,1UL);
      break;

    case 0xFFFE:
      if(bank == sms.cart.mapper_fffe)
        return;
      sms.cart.mapper_fffe = bank;
      LOG_DBG(LOG_CAT_BUS,("bank slot=1 value=0x%02lx masked=0x%02lx",
                           (unsigned long)value,(unsigned long)bank));
      cart_mapper_project(CART_BUS_SLOT1_PAGE,CART_BUS_SLOT_PAGES,bank,0UL);
      break;

    case 0xFFFF:
      if(bank == sms.cart.mapper_ffff)
        return;
      sms.cart.mapper_ffff = bank;
      LOG_DBG(LOG_CAT_BUS,("bank slot=2 value=0x%02lx masked=0x%02lx",
                           (unsigned long)value,(unsigned long)bank));
      /* Remembered always, projected only while the ROM holds the slot. */
      if(!(sms.cart.mapper_fffc & CART_MAPPER_FFFC_RAM_8000))
        cart_mapper_project(CART_BUS_SLOT2_PAGE,CART_BUS_SLOT_PAGES,bank,0UL);
      break;

    default:
      /* $FFFC, the only other address Z80_WR8 sends here. */
      if((uint32)value == sms.cart.mapper_fffc)
        return;
      cart_mapper_write_fffc((uint32)value);
      break;
    }
}
#endif /* SMS_MAPPER == MAPPER_SEGA */

#if SMS_MAPPER == MAPPER_CODEMASTERS
/*
 * ---------------------------------------------------------------------------
 * The Codemasters mapper, best effort: three registers, one per ROM slot,
 * each covering its whole slot. Contract and sources in cart.h.
 *
 * Semantics emulated, never form borrowed: TotalSMS/src/core/
 * sms_bus.c:174-190 (slot = register, sixteen pages each, no protected
 * first kilobyte), :242-248 (reset 0/1/2). Its on-cart RAM path
 * (:192-232) is not taken: one game, declared and not emulated. Where
 * TotalSMS takes a modulo, this masks (30_mappers.md section 4).
 * ---------------------------------------------------------------------------
 */

/*
 * The three slot registers at their reset values, the on-cart RAM latch
 * clear, traced as a state and not as an event, and no page moved: the
 * linear map the install just wrote is that state (cart.h). The
 * community page gives 0/1/0 (docs/sms_gg/mappers.md:113); 0/1/2 is
 * TotalSMS's and the map already installed -- recorded in cart.h, not
 * arbitrated here.
 */
static void
cart_mapper_reset(void)
{
  sms.cart.mapper_fffc = 0UL;
  sms.cart.mapper_fffd = CART_MAPPER_RESET_FFFD;
  sms.cart.mapper_fffe = CART_MAPPER_RESET_FFFE;
  sms.cart.mapper_ffff = CART_MAPPER_RESET_FFFF;

  LOG_INFO(LOG_CAT_BUS,("mapper reset slot0=0x%02lx slot1=0x%02lx slot2=0x%02lx",
                        (unsigned long)sms.cart.mapper_fffd,
                        (unsigned long)sms.cart.mapper_fffe,
                        (unsigned long)sms.cart.mapper_ffff));
}

/*
 * Header: cart.h. The address is one of the three ROM slots, which is all
 * CART_MAPPER_TRIGGER lets through; the slot is its top two bits. Bit 7
 * of the value is stripped before the bank is taken, so a request for the
 * on-cart RAM turns the bank the low bits name and warns once, on the
 * rising edge of the bit, before the same-value test: the bit is not part
 * of the bank, and a value differing by it alone must still be heard.
 */
void
cart_mapper_write(uint16 addr, uint8 value)
{
  uint32 bank = (uint32)value & CART_MAPPER_CM_BANK_BITS & sms.cart.bank_mask;
  uint32 slot = (uint32)addr >> 14;
  uint32 ram_bit;
  uint32 *reg;

  switch(slot)
    {
    case 0:
      reg = &sms.cart.mapper_fffd;
      break;

    case 1:
      ram_bit = (uint32)value & CART_MAPPER_CM_RAM_BIT;
      if(ram_bit & ~sms.cart.mapper_fffc)
        LOG_WARN(LOG_CAT_BUS,("codemasters on-cart ram ignored bit=7"));
      sms.cart.mapper_fffc = ram_bit;
      reg = &sms.cart.mapper_fffe;
      break;

    case 2:
      reg = &sms.cart.mapper_ffff;
      break;

    default:
      /*
       * The trigger lets $0000-$BFFF through and nothing else; a slot 3
       * would be the work RAM's pages 48-63. Refused, not repointed.
       */
      return;
    }

  if(bank == *reg)
    return;
  *reg = bank;
  LOG_DBG(LOG_CAT_BUS,("bank slot=%lu value=0x%02lx masked=0x%02lx",
                       (unsigned long)slot,(unsigned long)value,
                       (unsigned long)bank));
  cart_mapper_project(slot * CART_BUS_SLOT_PAGES,CART_BUS_SLOT_PAGES,bank,0UL);
}
#endif /* SMS_MAPPER == MAPPER_CODEMASTERS */

/*
 * ---------------------------------------------------------------------------
 * The I/O side of the bus: the memory control register and the aggregate of
 * the accesses that reached an empty hook. The decoding itself is the
 * macro block of cart.h, expanded in the Z80's port functions; what lives
 * here is the cold half -- the two traced events and the boot reset.
 * ---------------------------------------------------------------------------
 */

/*
 * One spelling of the memory control line, shared by the boot reset and by
 * a program's write: memctl=0x<v> cart=<on|off> workram=<on|off>. The two
 * named bits are the two this port would have to honour if it applied the
 * register (cart.h); the others have nothing to act on here.
 */
static void
cart_io_memctl_trace(void)
{
  LOG_INFO(LOG_CAT_BUS,("memctl=0x%02lx cart=%s workram=%s",
                        (unsigned long)sms.cart.memctl,
                        (sms.cart.memctl & 0x40U) ? "off" : "on",
                        (sms.cart.memctl & 0x10U) ? "off" : "on"));
}

/*
 * Header: cart.h. Reached on a change of value only, so the state line is
 * emitted once per change and never per write; each warning fires on the
 * rising edge of its bit alone, so a program that keeps the bit set while
 * it changes the others does not repeat it.
 */
void
cart_io_memctl_write(uint8 value)
{
  uint32 rose;

  rose = (uint32)value & ~sms.cart.memctl;
  sms.cart.memctl = (uint32)value;
  cart_io_memctl_trace();

  if(rose & 0x40U)
    LOG_WARN(LOG_CAT_BUS,("memctl bit6 ignored: no bios/card to fall back on"));
  if(rose & 0x10U)
    LOG_WARN(LOG_CAT_BUS,("memctl bit4 ignored: no bios/card to fall back on"));
}

/*
 * Header: cart.h.
 */
void
cart_io_report(void)
{
#if LOG_ENABLE
  if((sms.cart.io_unrouted_reads == 0) && (sms.cart.io_unrouted_writes == 0))
    return;

  LOG_WARN(LOG_CAT_BUS,("io unrouted reads=%lu writes=%lu (last port=0x%02lx)",
                        (unsigned long)sms.cart.io_unrouted_reads,
                        (unsigned long)sms.cart.io_unrouted_writes,
                        (unsigned long)sms.cart.io_last_port));

  sms.cart.io_unrouted_reads = 0;
  sms.cart.io_unrouted_writes = 0;
#endif
}

/*
 * The boot state of the I/O side, and the line that says the decoding is
 * in place. The register is stored directly rather than through the write
 * path: a reset is not a change a program made, and it is traced as the
 * state, not as an event.
 */
static void
cart_io_reset(void)
{
  sms.cart.memctl = CART_IO_MEMCTL_RESET;
  sms.cart.io_unrouted_reads = 0;
  sms.cart.io_unrouted_writes = 0;
  sms.cart.io_last_port = 0;

  cart_io_memctl_trace();
  LOG_INFO(LOG_CAT_BUS,("io map ok"));
}

/*
 * Builds one screen line out of the scratch buffer, cut to the painted
 * width. Called after sprintf has written the scratch.
 */
static void
cart_screen_set(char *line)
{
  cart_copy_bounded(line,cart_scratch,CART_SCREEN_LINE_MAX);
}

/*
 * Paints the picture of a load that failed, out of the figures recorded on
 * the way, then stops. Does not return.
 */
static void
cart_boot_fatal(Err err)
{
  switch(err)
    {
    case CART_ERR_SIZE:
      sprintf(cart_scratch,"rom refused: size %lu bytes",
              (unsigned long)cart_fail_size);
      cart_screen_set(cart_screen_line1);
      sprintf(cart_scratch,"limit %lu bytes, %s",
              (unsigned long)cart_fail_limit,cart_fail_screen_reason);
      cart_screen_set(cart_screen_line2);
      log_fatal(LOG_CAT_CART,LOG_E_CART_SIZE,
                cart_screen_line1,cart_screen_line2);
      break;

    case CART_ERR_READ:
      if(cart_fail_short)
        {
          sprintf(cart_scratch,"rom read short: %lu of %lu bytes",
                  (unsigned long)cart_fail_got,
                  (unsigned long)cart_fail_size);
          cart_screen_set(cart_screen_line1);
          sprintf(cart_scratch,"the disc gave less than it said");
          cart_screen_set(cart_screen_line2);
        }
      else
        {
          sprintf(cart_scratch,"cannot read the rom file");
          cart_screen_set(cart_screen_line1);
          sprintf(cart_scratch,"error %ld, see the trace",
                  (long)cart_fail_err);
          cart_screen_set(cart_screen_line2);
        }
      log_fatal(LOG_CAT_CART,LOG_E_CART_READ,
                cart_screen_line1,cart_screen_line2);
      break;

    case CART_ERR_NO_BUFFER:
      /*
       * A safety net: main.c stops on cart_init's failure before ever
       * calling cart_boot, so this branch runs only if the sequence is
       * changed. Same words as main.c for the same code.
       */
      log_fatal(LOG_CAT_CART,LOG_E_CART_ALLOC,
                CART_SCREEN_ALLOC_1,CART_SCREEN_ALLOC_2);
      break;

    default:
      log_fatal(LOG_CAT_CART,LOG_E_CART_NOT_FOUND,
                CART_SCREEN_NOT_FOUND_1,CART_SCREEN_NOT_FOUND_2);
      break;
    }
}

/*
 * Header: cart.h, where the order of the two names and the does-not-return
 * property of a failure are written out.
 */
Err
cart_boot(void)
{
  const char *name;
  cart_info_t info;
  Err err;

  /*
   * Only a file that is not there moves the search to the next name. Any
   * other answer of the lookup -- a failed open, a size that could not be
   * read -- is a disc that does not work, and trying a second name on it
   * would hide the cause behind a picture that says "no rom found".
   */
  name = CART_BOOT_PATH_SMS;
  err = cart_identify(name,&info);
  if(err == CART_ERR_NOT_FOUND)
    {
      name = CART_BOOT_PATH_GG;
      err = cart_identify(name,&info);
    }

  if(err == CART_ERR_NOT_FOUND)
    {
      LOG_ERR(LOG_CAT_CART,("no rom found in %s/ (tried %s, %s)",
                            CART_BOOT_DIR,CART_BOOT_SMS,CART_BOOT_GG));
      log_fatal(LOG_CAT_CART,LOG_E_CART_NOT_FOUND,
                CART_SCREEN_NOT_FOUND_1,CART_SCREEN_NOT_FOUND_2);
      return err;
    }

  if(err < 0)
    {
      cart_boot_fatal(err);
      return err;
    }

  err = cart_load(name);
  if(err < 0)
    {
      /*
       * Every refusal has already been traced where it was found; what is
       * built here is the picture. Plain words and digits: no underscore,
       * no path separator, since the font draws neither reliably (log.h,
       * the contract of log_fatal).
       */
      cart_boot_fatal(err);
      return err;
    }

  /*
   * The bus, installed the moment the profile is fixed and before it is
   * announced: the profile line below is the last word of a boot that can
   * run something, and a map that failed after it would give that line the
   * lie. The order the trace shows is the order the work happens in --
   * work RAM allocated, map written, and the reset that starts the program
   * following from the boot sequence once this returns.
   *
   * The one failure is memory refused for the work RAM, and it stops the
   * console: E200 is the emulated address space's memory, and the words
   * under the code name the one block that backs it.
   */
  err = cart_bus_install();
  if(err < 0)
    {
      log_fatal(LOG_CAT_BUS,LOG_E_Z80_RAM,
                "cannot allocate the work ram",
                "the console refused 8 kilobytes");
      return err;
    }

  /*
   * The I/O side of the bus, reset with the map: the memory
   * control register at its boot value and the counters at zero, then the
   * line that says the decoding is in place. Before the profile line for
   * the reason the map is: what follows profile= is a machine that can run.
   */
  cart_io_reset();

  /*
   * The one profile line, emitted at the moment the profile becomes final
   * and from the module that fixed it. In the [SYS] category rather than
   * [CART] because it announces what the whole program will now run as; the
   * subsystems brought up after the load repeat the name in their own init
   * lines, through cart_system_name, when they exist.
   */
  LOG_INFO(LOG_CAT_SYS,("profile=%s",cart_system_name(sms.cart.system)));

  return 0;
}
