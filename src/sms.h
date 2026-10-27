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
 *
 * Game Gear mode: the same machine with the colour RAM of 12-bit colours
 * (vdp.h) and the registers of ports $00-$06 (initial state C0 7F FF 00
 * FF 00 FF): $00 reads the START button (bit 7, 0 when pressed), the
 * region (bit 6: 0 Japan, 1 overseas) and the TV system (bit 5: 0 NTSC);
 * $01-$05 are the EXT connector and the serial port, stored and read
 * back (nothing is connected); $06 distributes the four PSG channels to
 * the left and right outputs. START is a button there, not the PAUSE NMI.
 */

#include "types.h"
#include "z80jit.h"
#include "smsmem.h"
#include "vdp.h"

/* Ports reading the V counter, whose value only changes at line
 * boundaries (for the translator's line-wait loops). */
#define SMS_PORT_VCOUNTER(p) (((p) & 0xC1u) == 0x40u)

/* VDP data port writes: their handler never leaves the block and their
 * run handler takes the buffered runs (z80j_set_buffer_port). */
#define SMS_PORT_DATA(p) (((p) & 0xC1u) == 0x80u)

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

/* The VDP comes first: the machine is allocated right after the
 * translator's context, where the assembly port handlers address it at a
 * fixed offset from the global pointer (SMS_VDP_OFFSET, checked in
 * sms.c). The callbacks of the translator recover the machine from the
 * mapper, whose first member they point at. */
typedef struct {
    vdp_state    vdp;
    sms_mem      mem;       /* holds the translator callbacks */
    z80j_ctx    *ctx;
    uint32       mem_ctrl;  /* port $3E */
    uint32       io_ctrl;   /* port $3F */
    uint32       pad;       /* SMS_PAD_* held */
    uint32       psg_latch; /* register selected by the last latch byte */
    uint32       psg_reg[8];/* tone 0, volume 0, ..., noise, volume 3 */
    sms_io_stats io;
    char         sdsc[80];  /* SDSC debug console line being written */
    uint32       sdsc_len;
    uint32       gg;        /* Game Gear mode */
    uint32       gg_japan;  /* region reported by port $00: Japan */
    uint32       gg_start;  /* START held */
    uint32       gg_port[7];/* ports $00-$06 ($06: PSG stereo distribution) */
} sms_machine;

/* Offset of the VDP state from the context (its size): the machine must
 * be placed right after the context, as game.c allocates them, for the
 * assembly port handlers (sms_io.s, which address it from the global
 * pointer, 0x400 into the context). */
#define SMS_VDP_OFFSET  0x44400

/* Region code of the cartridge header ("TMR SEGA" at $7FF0, $3FF0 or
 * $1FF0, the high nibble of its byte $0F): 3 and 4 Master System, 5, 6
 * and 7 Game Gear (Japan, export, international); 0 without a header. */
uint32 sms_rom_region(const uint8 *rom, uint32 rom_size);
#define SMS_REGION_GG(r)    ((r) >= 5 && (r) <= 7)

/* Sets up the machine on ctx for a cartridge image, then resets it;
 * lines is VDP_LINES_NTSC or VDP_LINES_PAL; gg selects the Game Gear
 * mode. */
void sms_init(sms_machine *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
              uint32 lines, uint8 *cart_ram, uint32 cart_ram_size, uint32 gg);

/* Power-on state, as the BIOS leaves it for the cartridge. */
void sms_reset(sms_machine *s);

/* Runs one frame with the buttons held (SMS_PAD_*); pause presses the
 * PAUSE button (NMI), or in Game Gear mode is the START button held. */
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
void sms_psg_w(void);

/* Offsets of the PSG fields from the machine, for sms_psg_w (sms_io.s). */
#define SMS_PSG_REG     0x5200
#define SMS_PSG_W       0x5228

#endif /* SMS_H */
