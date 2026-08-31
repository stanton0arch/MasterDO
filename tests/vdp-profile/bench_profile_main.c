/* Host bench for the READING half of the breakdown: the three functions of
 * src/main.c that weigh a window, convert a cost into cycles per pixel and
 * publish a round. They are static, so this file compiles main.c into itself
 * rather than linking against it -- which is the point: what is exercised is
 * the real source, not a copy that can drift from it.
 *
 * main() is renamed out of the way and never called. The stubs below exist
 * only to satisfy the linker for that dead function; none of them runs. What
 * the three functions under test actually touch is integer arithmetic and the
 * logging macros, and the logging is captured here line by line so that the
 * published table can be asserted on as text.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* CCB reaches main.c through the real graphics header on the target; the
 * stub set keeps the opaque typedef in hardware.h, which is all main.c
 * wants -- it never dereferences one. */
#include "hardware.h"

#define main main_not_run_by_this_bench
#include "main.c"
#undef main

#if !MAIN_PROFILE
#error "this bench needs SMS_VDP_PROFILE=1: it exists to exercise the profile reader"
#endif

/* ---- captured log ---- */

#define CAP_MAX  64
#define CAP_LEN 256
static char cap[CAP_MAX][CAP_LEN];
static int  cap_n = 0;

static void cap_reset(void) { cap_n = 0; }

void log_begin(int32 cat, int32 lvl)
{ (void)cat; (void)lvl; if(cap_n < CAP_MAX) cap[cap_n][0] = '\0'; }

void log_printf(const char *fmt, ...)
{
  va_list a;
  va_start(a,fmt);
  if(cap_n < CAP_MAX)
    { vsprintf(cap[cap_n],fmt,a); cap_n++; }
  va_end(a);
}

/* How many captured lines begin with the given text. */
static int cap_starting(const char *pfx)
{ int i, n = 0; size_t l = strlen(pfx);
  for(i = 0; i < cap_n; i++) if(strncmp(cap[i],pfx,l) == 0) n++;
  return n; }

/* How many captured lines contain the given text anywhere. */
static int cap_holding(const char *needle)
{ int i, n = 0;
  for(i = 0; i < cap_n; i++) if(strstr(cap[i],needle) != NULL) n++;
  return n; }

static void cap_dump(void)
{ int i; for(i = 0; i < cap_n; i++) printf("      | %s\n",cap[i]); }

/* ---- stubs for the renamed main(), none of which runs ---- */

void log_bind_screen(Item b, Item s) { (void)b; (void)s; }
void log_set_frame(const uint32 *frame) { (void)frame; }
void log_fatal(int32 c, int32 e, const char *a, const char *b)
{ (void)c; (void)e; (void)a; (void)b; }
const char *log_build_date(void) { return "bench"; }
const char *log_level_name(void) { return "bench"; }

void *sys_alloc(const char *n, int32 s, uint32 t) { (void)n; (void)s; (void)t; return NULL; }
void sys_mem_report(void) {}
void sys_mem_seal(void) {}
int32 sys_width(void) { return 320; }
int32 sys_height(void) { return 240; }
Item sys_bitmap(void) { return 0; }
Item sys_screen(void) { return 0; }
int32 sys_screen_count(void) { return 1; }
int32 sys_screen_index(void) { return 0; }
Err sys_display_open(void) { return 0; }
Err sys_display_show(void) { return 0; }
Err sys_audio_open(void) { return 0; }
Err sys_vbl_open(void) { return 0; }
Err sys_vbl_wait(uint32 fields) { (void)fields; return 0; }
uint32 sys_vbl_count(void) { return 0; }
Err sys_fill_screen(int32 index, Color color) { (void)index; (void)color; return 0; }
Err sys_fill(Color color) { (void)color; return 0; }
Err sys_text(int32 x, int32 y, const char *text, Color color)
{ (void)x; (void)y; (void)text; (void)color; return 0; }
uint32 sys_usec(void) { return 0; }

Err cart_init(void) { return 0; }
Err cart_boot(void) { return 0; }
void cart_io_report(void) {}

Err z80_init(void) { return 0; }
void z80_reset(void) {}
int32 z80_run(int32 quota) { (void)quota; return 0; }
int32 z80_is_stopped(void) { return 0; }
uint16 z80_pc(void) { return 0; }

int32 vdp_init(void) { return 0; }
void vdp_line(void) {}
void vdp_report(void) {}
void *vdp_cel(void) { return NULL; }
uint16 vdp_backdrop(void) { return 0; }
void vdp_backdrop_repainted(void) {}
void vdp_profile_select(uint32 v) { (void)v; }
uint32 vdp_profile_reps(uint32 p) { (void)p; return 1UL; }

/* The one library call the dead main() makes. Declared in the real
 * graphics header as Err DrawCels(Item, CCB *); the stub set keeps CCB
 * opaque, so the definition here matches on the pointer alone. */
Err DrawCels(Item bitmapItem, CCB *ccb) { (void)bitmapItem; (void)ccb; return 0; }

/* ---- the bench proper ---- */

static int failed = 0;
static void check(int cond, const char *what)
{ printf("  [%s] %s\n", cond ? "OK" : "FAIL", what); if(!cond) failed++; }

static void check_eq(unsigned long got, unsigned long want, const char *what)
{ printf("  [%s] %s (got %lu, want %lu)\n", (got == want) ? "OK" : "FAIL",
         what, got, want);
  if(got != want) failed++; }

/*
 * One window of the established regime of the reference run, built so that
 * every figure below is arrived at BY HAND and not by re-running the code
 * under test:
 *   frame   1 000 000 us over 2 frames -> 500.0 ms          -> frame10 5000
 *   stretch   974 000 us over 2 frames -> 487.0 ms          -> emul10  4870
 *   draw        6 000 us over 2 frames ->   3.0 ms          -> draw10    30
 *   sum       4870 + 30 = 4900, i.e. 98 pct of 5000         -> healthy
 *   sides     z80 mean 100 us, vdp mean 600 us, share 700
 *   z80 part  4870 * 100 / 700 = 695.71 -> 695
 *   vdp part  4870 - 695 = 4175, i.e. 417.5 ms
 */
#define W_USEC     1000000UL
#define W_FRAMES         2UL
#define W_EMUL      974000UL
#define W_EMULF          2UL
#define W_Z80         1600UL
#define W_Z80S          16UL
#define W_VDP         9600UL
#define W_VDPS          16UL
#define W_DRAW        6000UL
#define W_VDP10       4175UL

static int win(uint32 usec, uint32 frames, uint32 emul, uint32 emulf,
               uint32 z80, uint32 z80s, uint32 vdp, uint32 vdps,
               uint32 draw, uint32 *out)
{ return (int)main_profile_window(usec,frames,emul,emulf,z80,z80s,
                                  vdp,vdps,draw,out); }

static void test_window(void)
{
  uint32 v;

  printf("main_profile_window\n");

  v = 0xDEADUL;
  check(win(W_USEC,W_FRAMES,W_EMUL,W_EMULF,W_Z80,W_Z80S,W_VDP,W_VDPS,W_DRAW,&v)
        == 1,"a healthy window of the reference regime is kept");
  check_eq((unsigned long)v,(unsigned long)W_VDP10,
           "and hands back the render figure computed by hand");

  /* The two host-clock jumps of the reference run: a frame of seconds
     against a stretch of milliseconds. */
  v = 0xDEADUL;
  check(win(2169800UL,1UL,487000UL,1UL,W_Z80,W_Z80S,W_VDP,W_VDPS,3000UL,&v)
        == 0,"a window a clock jump stretched to 2169.8 ms is refused");
  v = 0xDEADUL;
  check(win(4293678000UL,1UL,487000UL,1UL,W_Z80,W_Z80S,W_VDP,W_VDPS,3000UL,&v)
        == 0,"a window a clock jump stretched to 4293678.0 ms is refused");

  /* A window with no sample on one side or the other: the share cannot be
     weighed, and a render figure of zero must not be counted as a render
     that cost nothing. */
  check(win(W_USEC,W_FRAMES,W_EMUL,W_EMULF,W_Z80,0UL,W_VDP,W_VDPS,W_DRAW,&v)
        == 0,"a window with no processor sample is refused");
  check(win(W_USEC,W_FRAMES,W_EMUL,W_EMULF,W_Z80,W_Z80S,W_VDP,0UL,W_DRAW,&v)
        == 0,"a window with no render sample is refused");
  check(win(W_USEC,W_FRAMES,0UL,W_Z80S,W_VDP,W_VDPS,W_VDP,W_VDPS,W_DRAW,&v)
        == 0,"a window whose processor side means zero is refused");
  check(win(W_USEC,W_FRAMES,W_EMUL,W_EMULF,W_Z80,W_Z80S,0UL,W_VDPS,W_DRAW,&v)
        == 0,"a window whose render side means zero is refused");

  /* The two edges of the band, and the two guards before it. */
  check(win(W_USEC,0UL,W_EMUL,W_EMULF,W_Z80,W_Z80S,W_VDP,W_VDPS,W_DRAW,&v)
        == 0,"a window with no frame is refused");
  check(win(999999UL,W_FRAMES,W_EMUL,W_EMULF,W_Z80,W_Z80S,W_VDP,W_VDPS,W_DRAW,&v)
        == 0,"a window shorter than the period is refused");
  /* draw pushed to 1200.0 ms a frame: the parts overrun the whole */
  check(win(W_USEC,W_FRAMES,W_EMUL,W_EMULF,W_Z80,W_Z80S,W_VDP,W_VDPS,
            2400000UL,&v) == 0,"a window whose parts overrun the whole is refused");
  /* the stretch shrunk to 200.0 ms a frame: the parts fall far short */
  check(win(W_USEC,W_FRAMES,400000UL,W_EMULF,W_Z80,W_Z80S,W_VDP,W_VDPS,W_DRAW,&v)
        == 0,"a window whose parts fall short of the whole is refused");
}

static void test_cpp10(void)
{
  printf("main_profile_cpp10\n");
  /*
   * Walked by hand, the chain the comment describes:
   * 10.0 ms = 10 000 us; 10 000 us * 12.5 MHz = 125 000 cycles;
   * 125 000 / 49 152 pixels = 2.543 cycles a pixel, i.e. 25 tenths.
   */
  check_eq((unsigned long)main_profile_cpp10(100UL),25UL,
           "10.0 ms a frame is 2.5 cycles a pixel");
  /* 371.4 ms: 37 140 000 cycles over 49 152 pixels = 755.6... no:
     371 400 us * 12.5 = 4 642 500 cycles / 49 152 = 94.45 */
  check_eq((unsigned long)main_profile_cpp10(3714UL),944UL,
           "the published render figure is 94.4 cycles a pixel");
  /* the 7 ms target: 87 500 cycles over 49 152 pixels = 1.78 */
  check_eq((unsigned long)main_profile_cpp10(70UL),17UL,
           "the 7 ms target is 1.7 cycles a pixel");
  check_eq((unsigned long)main_profile_cpp10(0UL),0UL,"a post of nothing is nothing");
  check_eq((unsigned long)MAIN_FRAME_PIXELS,49152UL,
           "the pixels of a picture are calculated, not restated");
}

/* ctrl 371.4 ms, and three posts displaced by 228.6, 38.6 and 58.6 ms:
   they sum to 325.8, which the grouped variant is made to match exactly. */
static void round_of(uint32 *sum10, uint32 *win_, uint32 ctrl, uint32 bg,
                     uint32 spr, uint32 pack, uint32 all)
{
  uint32 i;
  for(i = 0; i < VDP_PROFILE_VARIANTS; i++) win_[i] = 4UL;
  sum10[VDP_PROFILE_CONTROL] = ctrl * 4UL;
  sum10[VDP_PROFILE_BG]      = bg   * 4UL;
  sum10[VDP_PROFILE_SPRITES] = spr  * 4UL;
  sum10[VDP_PROFILE_PACK]    = pack * 4UL;
  sum10[VDP_PROFILE_ALL]     = all  * 4UL;
}

static void test_emit(void)
{
  uint32 sum10[VDP_PROFILE_VARIANTS];
  uint32 win_[VDP_PROFILE_VARIANTS];
  uint32 drop[VDP_PROFILE_VARIANTS];
  uint32 i;

  for(i = 0; i < VDP_PROFILE_VARIANTS; i++) drop[i] = 0;

  printf("main_profile_emit -- additivity that closes\n");
  round_of(sum10,win_,3714UL,6000UL,4100UL,4300UL,6972UL);
  cap_reset();
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_starting("post "),5UL,
           "a round that closes publishes the five post lines");
  check_eq((unsigned long)cap_holding("does not close"),0UL,
           "and says nothing about not closing");
  check_eq((unsigned long)cap_holding("close=100pct"),1UL,
           "the additivity reads 100 pct: 228.6 + 38.6 + 58.6 against 325.8");
  /*
   * Each post pinned to ITS OWN displacement, and the three chosen far
   * apart on purpose: an assertion on one of them alone leaves two
   * transposable without a word.
   */
  check_eq((unsigned long)cap_holding("post bg cost=228.6ms"),1UL,
           "the background post publishes the displacement it was given");
  check_eq((unsigned long)cap_holding("post sprites cost=38.6ms"),1UL,
           "the sprite post publishes its own, not another's");
  check_eq((unsigned long)cap_holding("post pack cost=58.6ms"),1UL,
           "and the packing post its own");
  check_eq((unsigned long)cap_holding("post rest cost=45.6ms"),1UL,
           "the residual is 371.4 less the three that were measured");
  check_eq((unsigned long)cap_holding("post total vdp=371.4ms"),1UL,
           "and the total is the published figure itself");
  /* 228.6 ms a frame over 49 152 pixels: 228 600 us * 12.5 = 2 857 500
     cycles, 58.13 a pixel. Walked by hand. */
  check_eq((unsigned long)cap_holding("post bg cost=228.6ms/frame 58.1 cycles/pixel share=61pct"),1UL,
           "with its cost per pixel and its share, both hand-checked");
  check_eq((unsigned long)cap_holding("post rest"),1UL,
           "the residual is published and named");
  check_eq((unsigned long)cap_holding("residual, not measured"),1UL,
           "and is never presented as a measurement");
  if(failed) cap_dump();

  printf("main_profile_emit -- additivity that does not close\n");
  round_of(sum10,win_,3714UL,6000UL,4100UL,4300UL,8714UL);
  cap_reset();
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_starting("post "),0UL,
           "a round that does not close publishes NO post line");
  check_eq((unsigned long)cap_holding("does not close"),1UL,
           "and says so in one line");
  check_eq((unsigned long)cap_holding("close=65pct"),1UL,
           "with the figure that failed the gate");

  printf("main_profile_emit -- a displacement below zero\n");
  round_of(sum10,win_,3714UL,3600UL,4100UL,4300UL,6972UL);
  cap_reset();
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_holding("gap bg=-11.4ms"),1UL,
           "the gap line shows the sign");
  check_eq((unsigned long)cap_holding("post bg cost="),0UL,
           "and the table refuses to publish a cost for that post");
  check_eq((unsigned long)cap_holding("post bg not measurable"),1UL,
           "saying instead that it is not measurable this round");
  check_eq((unsigned long)cap_holding("cannot be weighed"),1UL,
           "and the round stops there, unweighed");
  check_eq((unsigned long)cap_holding("close="),0UL,
           "no percentage is taken of an incomplete sum");
  check_eq((unsigned long)cap_holding("post rest"),0UL,
           "and no residual is published");
  if(failed) cap_dump();

  printf("main_profile_emit -- the posts overrun the published figure\n");
  /* ctrl 371.4; gaps 228.6 + 128.6 + 128.6 = 485.8, past ctrl. The grouped
     variant is set to the same 485.8 so the gate closes at 100 pct and the
     overrun is what the round has to notice by itself. */
  round_of(sum10,win_,3714UL,6000UL,5000UL,5000UL,8572UL);
  cap_reset();
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_holding("close=100pct"),1UL,
           "the additivity gate is passed");
  check_eq((unsigned long)cap_holding("posts overrun"),1UL,
           "and the round still says the posts overrun the published figure");
  check_eq((unsigned long)cap_holding("post rest cost="),0UL,
           "publishing no residual figure");
  check_eq((unsigned long)cap_holding("post rest not interpretable"),1UL,
           "and saying the residual is not interpretable");
  if(failed) cap_dump();

  printf("main_profile_emit -- where the round stands\n");
  round_of(sum10,win_,3714UL,6000UL,4100UL,4300UL,6972UL);
  for(i = 0; i < VDP_PROFILE_VARIANTS; i++) drop[i] = i;
  cap_reset();
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_holding("round dropped ctrl=0 bg=1 spr=2 pack=3 all=4"),1UL,
           "the windows thrown out are counted per variant, not in one heap");
  check_eq((unsigned long)cap_holding("round win ctrl=4 bg=4 spr=4 pack=4 all=4"),1UL,
           "and so are the ones kept");
  for(i = 0; i < VDP_PROFILE_VARIANTS; i++) drop[i] = 0;
  if(failed) cap_dump();

  printf("main_profile_state -- the sign of life\n");
  cap_reset();
  main_profile_state(win_,drop,120UL,"waiting");
  check_eq((unsigned long)cap_holding("profile waiting win"),1UL,
           "a stalled instrument says where each variant stands");
  check_eq((unsigned long)cap_starting("post "),0UL,
           "and publishes no table while it waits");

  printf("main_profile_name\n");
  check(strcmp(main_profile_name(VDP_PROFILE_CONTROL),"control") == 0,"control");
  check(strcmp(main_profile_name(VDP_PROFILE_BG),"bg") == 0,"bg");
  check(strcmp(main_profile_name(VDP_PROFILE_SPRITES),"sprites") == 0,"sprites");
  check(strcmp(main_profile_name(VDP_PROFILE_PACK),"pack") == 0,"pack");
  check(strcmp(main_profile_name(VDP_PROFILE_ALL),"all") == 0,"all");
  check(strcmp(main_profile_name(99UL),"control") == 0,"anything else reads as the control");

  printf("main_profile_emit -- the price of the clock readings\n");
  round_of(sum10,win_,3714UL,6000UL,4100UL,4300UL,6972UL);
  cap_reset();
  /* 984 samples over 120 frames is 8.2 sampled lines a frame, three
     readings each: 24.6 readings a frame at 32 us, i.e. 787 us = 7 tenths
     of a millisecond a frame. All four figures walked by hand. */
  main_profile_emit(sum10,win_,drop,120UL,984UL,32UL);
  check_eq((unsigned long)cap_holding("reads=24.6/frame"),1UL,
           "the readings a frame are published");
  check_eq((unsigned long)cap_holding("at 32us each"),1UL,
           "with the price this run measured for one");
  check_eq((unsigned long)cap_holding("-> 0.7ms/frame"),1UL,
           "and what the two make per frame");
}

int
main(void)
{
  test_window();
  test_cpp10();
  test_emit();
  printf("\nfailed=%d\n",failed);
  return failed ? 1 : 0;
}
