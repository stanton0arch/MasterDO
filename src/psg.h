#ifndef PSG_H
#define PSG_H

/*
 * SN76489 PSG voices on the DSP of the console, through the audio folio.
 *
 * The machine (sms.c) keeps the eight PSG registers as the game writes
 * them. Once per frame, the state they describe is sent to five DSP
 * instruments: three square wave oscillators for the tone channels, a
 * sample-and-hold noise source for the white noise of channel 3 and a
 * narrow pulse oscillator for its periodic noise, mixed into the DAC by
 * an 8-input mixer. Only the knobs whose value changes are tweaked, so a
 * frame costs a few folio calls at most, and none when nothing changed.
 *
 * Tone channel: frequency = clock / (32 x period). A period of 0 is a
 * constant level on the hardware and periods up to 5 give tones above
 * 22 kHz: both are silenced. Noise channel: the shift register advances
 * at clock / (32 x N) with N = 16, 32, 64 or the period of tone 2; white
 * noise is a new random value at that rate, periodic noise a pulse of one
 * sixteenth duty at a sixteenth of that rate. Volume: 2 dB per attenuation
 * step, 15 is silence.
 *
 * Limitations: a register is sent as it stands at the end of the frame,
 * so writes made several times within a frame (sample playback through
 * the volume registers, envelopes stepped faster than the frame) collapse
 * to their last value; the noise is an approximation of the LFSR, and the
 * shift register reset on a noise register write is not modelled.
 *
 * The module is only linked into the ISO: the analysis harness stubs it.
 */

#include "types.h"

#define PSG_CLOCK_NTSC  3579545u
#define PSG_CLOCK_PAL   3546893u

#define PSG_VOICES      5       /* tone 0-2, white noise, periodic noise */
#define PSG_WHITE       3
#define PSG_PERIODIC    4

typedef struct {
    uint32 ok;              /* the voices run; silence otherwise */
    Item   mixer;
    Item   voice[PSG_VOICES];
    Item   amp[PSG_VOICES]; /* Amplitude knobs */
    Item   freq[PSG_VOICES];/* Frequency knobs */
    int32  amp_val[PSG_VOICES];     /* values the knobs hold */
    uint32 period[PSG_VOICES];      /* period the frequency knob was set from */
    uint32 tweaks;          /* knob tweaks sent since the voices were opened */
    int32  amplitude;       /* DSP amplitude allocated */
} psg_dsp;

/* Opens the audio folio and loads the voices (silent). On failure the
 * structure is left closed: psg_update and psg_mute then do nothing. */
Err    psg_open(psg_dsp *d);
void   psg_close(psg_dsp *d);

/* Sends the PSG registers (tone 0, volume 0, ... noise, volume 3, as
 * sms_machine keeps them) to the voices; returns the knob tweaks made. */
uint32 psg_update(psg_dsp *d, const uint32 reg[8], uint32 clock);

/* Silences every voice (pause, menus). */
void   psg_mute(psg_dsp *d);

#endif /* PSG_H */
