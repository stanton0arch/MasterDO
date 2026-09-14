/* The console stubs the host programs of this directory share (host.h). */
#include "sms.h"
#include "cart.h"
#include "vdp.h"
#include "log.h"
#include "blockfile.h"
#include "operror.h"
#include "filesystem.h"
#include "host.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The category names below are indexed by the log's own numbering. */
#if LOG_CAT_COUNT != 11
#error "the log categories changed: the names in cat_name[] must follow"
#endif

const char *host_rom_path = NULL;
int host_booted = 0;

static long rom_size = 0;

/* ---- the disc: one file, the ROM named on the command line ---- */

/* Whether a name ends in the extension, letter case set aside. */
static int has_ext(const char *name, const char *ext)
{
  size_t n = strlen(name), e = strlen(ext), i;
  if(n < e) return 0;
  for(i = 0; i < e; i++)
    {
      int c = name[n - e + i], d = ext[i];
      if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if(c != d) return 0;
    }
  return 1;
}

Err OpenBlockFile(char *name, BlockFilePtr bf)
{
  FILE *f;
  (void)bf;
  /* No ROM named: the boot finds no file, as the console would. */
  if(host_rom_path == NULL)
    return MAKEFERR(ER_SEVERE,ER_C_NSTND,ER_Fs_NoFile);
  /* The boot tries the SMS name, then the GG name (src/cart.c): the one
     found is the one whose extension is the file's on disc, so that a
     Game Gear image boots with the Game Gear profile and nothing else. */
  if(!(has_ext(name,".sms") && has_ext(host_rom_path,".sms")) &&
     !(has_ext(name,".gg") && has_ext(host_rom_path,".gg")))
    return MAKEFERR(ER_SEVERE,ER_C_NSTND,ER_Fs_NoFile);
  f = fopen(host_rom_path,"rb");
  if(f == NULL)
    return MAKEFERR(ER_SEVERE,ER_C_NSTND,ER_Fs_NoFile);
  fseek(f,0,SEEK_END);
  rom_size = ftell(f);
  fclose(f);
  return 0;
}
void CloseBlockFile(BlockFilePtr bf) { (void)bf; }
int32 GetBlockFileSize(BlockFilePtr bf) { (void)bf; return (int32)rom_size; }
void *LoadFileHere(const char *fname, int32 *pfsize, void *buffer, int32 bufsize)
{
  FILE *f;
  size_t n;
  (void)fname;
  if(host_rom_path == NULL) { *pfsize = -1; return NULL; }
  f = fopen(host_rom_path,"rb");
  if(f == NULL) { *pfsize = -1; return NULL; }
  n = fread(buffer,1,(size_t)bufsize,f);
  fclose(f);
  *pfsize = (int32)n;
  return buffer;
}

/* ---- the console ---- */

void *sys_alloc(const char *name, int32 size, uint32 memtype)
{
  void *p = malloc((size_t)size);
  (void)name; (void)memtype;
  if(p != NULL) memset(p,0,(size_t)size);
  return p;
}
void sys_mem_report(void) {}
void sys_mem_seal(void) {}
int32 sys_width(void) { return 320; }
int32 sys_height(void) { return 240; }
Item sys_bitmap(void) { return 0; }

/* ---- the log: boot in full, then warnings and errors only ---- */

static const char *cat_name[] =
  {"BOOT","SYS","CART","BUS","Z80","VDP","PSG","PAD","SAVE","PERF","GG"};
static const char *lvl_name[] = {"ERR","WARN","INFO","DBG","TRACE"};
static int muted = 0;

void log_begin(int32 cat, int32 lvl)
{
  muted = (host_booted && lvl > LOG_LVL_WARN);
  if(muted) return;
  fprintf(stderr,"[%s][%s] ",cat_name[cat],lvl_name[lvl]);
}
void log_printf(const char *fmt, ...)
{
  va_list a;
  if(muted) return;
  va_start(a,fmt);
  vfprintf(stderr,fmt,a);
  va_end(a);
  fputc('\n',stderr);
}
void log_bind_screen(Item b, Item s) { (void)b; (void)s; }
void log_set_frame(const uint32 *f) { (void)f; }
void log_fatal(int32 cat, int32 code, const char *l1, const char *l2)
{
  fprintf(stderr,"FATAL cat=%ld code=%ld: %s / %s\n",(long)cat,(long)code,l1,l2);
  exit(2);
}

/* ---- helpers ---- */

long host_count_arg(const char *s)
{
  char *end;
  long v;
  if(*s == '\0') return -1;
  v = strtol(s,&end,10);
  if(*end != '\0' || v <= 0) return -1;
  return v;
}

unsigned long host_fnv_begin(void)
{
  return 2166136261UL;
}

unsigned long host_fnv_add(unsigned long h, const unsigned char *p,
                           unsigned long n)
{
  unsigned long i;
  for(i = 0; i < n; i++)
    {
      h ^= (unsigned long)p[i];
      h *= 16777619UL;
      h &= 0xFFFFFFFFUL;
    }
  return h;
}

unsigned long host_fnv_add_u32(unsigned long h, unsigned long v)
{
  unsigned char b[4];
  b[0] = (unsigned char)(v & 0xFFUL);
  b[1] = (unsigned char)((v >> 8) & 0xFFUL);
  b[2] = (unsigned char)((v >> 16) & 0xFFUL);
  b[3] = (unsigned char)((v >> 24) & 0xFFUL);
  return host_fnv_add(h,b,4);
}

void host_present(void)
{
  int32 bands, k;
  bands = vdp_list_begin();
  for(k = 0; k < bands; k++) vdp_list_band(k);
  vdp_list_end();
}
