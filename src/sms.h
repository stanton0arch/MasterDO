#ifndef SMS_H
#define SMS_H

/*
 * Sega Master System machine for the Z80 translator: cartridge memory and
 * mapper (smsmem.c), VDP (vdp.c), I/O ports (pad, memory control, I/O
 * control) and PSG writes, which are recorded but not played yet.
 *
 * Port decoding follows the SMS 2: address bits 7, 6 and 0 select
 *   $00-$3F  write: memory control (even), I/O control (odd); read: $FF
 *   $40-$7F  write: PSG; read: V counter (even), H counter (odd)
 *   $80-$BF  VDP data (even), VDP control / status (odd)
 *   $C0-$FF  read: I/O port A/B (even), port B / misc (odd); write: none,
 *            except $FD, the data port of the SDSC debug console used by
 *            test programs: its text goes to the debug log
 */

#include "types.h"
#include "z80jit.h"
#include "smsmem.h"
#include "vdp.h"

/* Ports reading the V counter, whose value only changes at line
 * boundaries (for the translator's line-wait loops). */
#define SMS_PORT_VCOUNTER(p) (((p) & 0xC1u) == 0x40u)

/* Buttons of pad 1 (sms_frame argument). */
#define SMS_PAD_UP     0x01
#define SMS_PAD_DOWN   0x02
#define SMS_PAD_LEFT   0x04
#define SMS_PAD_RIGHT  0x08
#define SMS_PAD_1      0x10
#define SMS_PAD_2      0x20

/* Port accesses handled in C, by kind (the VDP data and control ports
 * are counted by the VDP, see vdp_state). */
typedef struct {
    uint32 vdp_stat_r;
    uint32 counter_r;       /* V and H counters */
    uint32 psg_w;
    uint32 pad_r;
    uint32 other;           /* memory and I/O control, unused ports */
    uint32 leaves;          /* VDP control writes that left the block */
} sms_io_stats;

typedef struct {
    sms_mem      mem;       /* first member: holds the translator callbacks */
    vdp_state    vdp;
    z80j_ctx    *ctx;
    uint32       mem_ctrl;  /* port $3E */
    uint32       io_ctrl;   /* port $3F */
    uint32       pad;       /* SMS_PAD_* held */
    uint32       psg_latch; /* register selected by the last latch byte */
    uint32       psg_reg[8];/* tone 0, volume 0, ..., noise, volume 3 */
    sms_io_stats io;
    char         sdsc[80];  /* SDSC debug console line being written */
    uint32       sdsc_len;
    /* Port handlers of the translator (z80jit.h). */
    uint32       port_in[Z80J_PORTS];
    uint32       port_out[Z80J_PORTS];
    uint32       port_outn[Z80J_PORTS];
} sms_machine;

/* Sets up the machine on ctx for a cartridge image, then resets it;
 * lines is VDP_LINES_NTSC or VDP_LINES_PAL. */
void sms_init(sms_machine *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
              uint32 lines, uint8 *cart_ram, uint32 cart_ram_size);

/* Power-on state, as the BIOS leaves it for the cartridge. */
void sms_reset(sms_machine *s);

/* Runs one frame with the buttons held (SMS_PAD_*); pause presses the
 * PAUSE button (NMI). */
void sms_frame(sms_machine *s, uint32 pad, uint32 pause);

/* Port handlers in assembly (sms_io.s). */
void sms_vdp_data_w(void);
void sms_vdp_data_r(void);
void sms_vdp_stat_r(void);
void sms_vdp_event_a(void);
void sms_vdp_vc_r(void);
void sms_vdp_hc_r(void);
void sms_vdp_ctrl_w(void);
void sms_vdp_data_wn(void);

#endif /* SMS_H */
