/*
 * SN76489 PSG voices on the DSP through the audio folio. See psg.h.
 */

#include "psg.h"

#include "audio.h"
#include "stdio.h"
#include "string.h"

#define VOICE_PRIORITY  100
#define MIXER_PRIORITY  50      /* after the voices in the DSP frame */
#define TONE_MIN_PERIOD 6       /* shorter periods are tones above 22 kHz */
#define PULSE_WIDTH     (-0x7000)   /* one sixteenth of the cycle: the
                                     * PulseWidth knob runs from -0x8000 to
                                     * 0x7FFF, 0 being a square wave */

static const char *const voice_file[PSG_VOICES] = {
    "square.dsp", "square.dsp", "square.dsp", "randomhold.dsp", "pulse.dsp"
};
static const char *const mixer_input[PSG_VOICES] = {
    "Input0", "Input1", "Input2", "Input3", "Input4"
};
static const char *const mixer_left[PSG_VOICES] = {
    "LeftGain0", "LeftGain1", "LeftGain2", "LeftGain3", "LeftGain4"
};
static const char *const mixer_right[PSG_VOICES] = {
    "RightGain0", "RightGain1", "RightGain2", "RightGain3", "RightGain4"
};

/* Gains of the mixer inputs (0x7FFF is unity): the three tones at 7/32
 * and the noise at 10/32 (only one of the two noise voices sounds at a
 * time), so that full-scale voices on every input add up below the range
 * of the DAC, with the noise a little louder than a tone, as it is heard
 * from the console's own output. */
static const int32 mixer_gain[PSG_VOICES] = { 0x1C00, 0x1C00, 0x1C00, 0x2800, 0x2800 };

/* Amplitude for each attenuation value: 2 dB per step from full scale,
 * 15 is off. */
static const int32 vol_amp[16] = {
    32767, 26028, 20675, 16422, 13045, 10362, 8231, 6538,
    5193, 4125, 3277, 2603, 2067, 1642, 1304, 0
};

/* Frequency in 16.16 Hz of a counter of the given period (6 or more) on
 * the PSG clock: clock / (32 x period), computed as (clock x 64 / period)
 * x 32 to stay within 32 bits. */
static uint32 hz16(uint32 clock, uint32 period)
{
    return ((clock * 64u) / period) << 5;
}

static void set_amp(psg_dsp *d, uint32 v, int32 amp)
{
    if (d->amp_val[v] != amp) {
        d->amp_val[v] = amp;
        d->tweaks++;
        TweakRawKnob(d->amp[v], amp);
    }
}

/* The Frequency knob of the oscillators takes 16.16 Hz through
 * TweakKnob(), which converts it to the phase increment of the DSP. */
static void set_hz(psg_dsp *d, uint32 v, uint32 hz)
{
    d->tweaks++;
    TweakKnob(d->freq[v], (int32)hz);
}

/* Bits of the stereo distribution enabling voice v on the left and on
 * the right (the two noise voices share channel 3's). */
static uint32 stereo_bits(uint32 v)
{
    uint32 c = v < 3 ? v : 3;

    return (0x10u << c) | (1u << c);
}

/* Mixer gains of the voices whose distribution changed. */
static void set_stereo(psg_dsp *d, uint32 stereo)
{
    uint32 changed = stereo ^ d->stereo;
    uint32 v;

    d->stereo = stereo;
    for (v = 0; v < PSG_VOICES; v++) {
        uint32 b = stereo_bits(v);

        if (changed & b & 0xF0) {
            d->tweaks++;
            TweakRawKnob(d->gain_l[v], (stereo & b & 0xF0) ? mixer_gain[v] : 0);
        }
        if (changed & b & 0x0F) {
            d->tweaks++;
            TweakRawKnob(d->gain_r[v], (stereo & b & 0x0F) ? mixer_gain[v] : 0);
        }
    }
}

static void fail(psg_dsp *d, const char *what, const char *name, Err err)
{
    printf("Sound: %s failed for %s (0x%lx), the run is silent\n", what, name,
           (unsigned long)err);
    psg_close(d);
}

Err psg_open(psg_dsp *d)
{
    Err err;
    uint32 v;

    memset(d, 0, sizeof(*d));
    d->mixer = -1;
    for (v = 0; v < PSG_VOICES; v++) {
        d->voice[v] = -1;
        d->amp[v] = -1;
        d->freq[v] = -1;
        d->gain_l[v] = -1;
        d->gain_r[v] = -1;
    }
    d->stereo = 0xFF;
    err = OpenAudioFolio();
    if (err < 0) {
        printf("Sound: OpenAudioFolio failed (0x%lx), the run is silent\n", (unsigned long)err);
        return err;
    }
    d->ok = 1;      /* so that psg_close releases what follows */
    d->amplitude = AllocAmplitude(0x7FFF);

    d->mixer = LoadInstrument("mixer8x2.dsp", 0, MIXER_PRIORITY);
    if (d->mixer < 0) {
        fail(d, "LoadInstrument", "mixer8x2.dsp", d->mixer);
        return d->mixer;
    }
    for (v = 0; v < PSG_VOICES; v++) {
        d->voice[v] = LoadInstrument((char *)voice_file[v], 0, VOICE_PRIORITY);
        if (d->voice[v] < 0) {
            fail(d, "LoadInstrument", voice_file[v], d->voice[v]);
            return d->voice[v];
        }
        d->amp[v] = GrabKnob(d->voice[v], "Amplitude");
        d->freq[v] = GrabKnob(d->voice[v], "Frequency");
        if (d->amp[v] < 0 || d->freq[v] < 0) {
            err = d->amp[v] < 0 ? d->amp[v] : d->freq[v];
            fail(d, "GrabKnob", voice_file[v], err);
            return err;
        }
        err = ConnectInstruments(d->voice[v], "Output", d->mixer, (char *)mixer_input[v]);
        if (err < 0) {
            fail(d, "ConnectInstruments", mixer_input[v], err);
            return err;
        }
        d->gain_l[v] = GrabKnob(d->mixer, (char *)mixer_left[v]);
        d->gain_r[v] = GrabKnob(d->mixer, (char *)mixer_right[v]);
        err = d->gain_l[v] < 0 ? d->gain_l[v] : d->gain_r[v];
        if (err >= 0)
            err = TweakRawKnob(d->gain_l[v], mixer_gain[v]);
        if (err >= 0)
            err = TweakRawKnob(d->gain_r[v], mixer_gain[v]);
        if (err < 0) {
            fail(d, "mixer gain", mixer_left[v], err);
            return err;
        }
        TweakRawKnob(d->amp[v], 0);
        d->amp_val[v] = 0;
    }
    {
        Item width = GrabKnob(d->voice[PSG_PERIODIC], "PulseWidth");

        if (width < 0) {
            fail(d, "GrabKnob", "PulseWidth", width);
            return width;
        }
        TweakRawKnob(width, PULSE_WIDTH);
        ReleaseKnob(width);
    }
    err = StartInstrument(d->mixer, NULL);
    for (v = 0; v < PSG_VOICES && err >= 0; v++)
        err = StartInstrument(d->voice[v], NULL);
    if (err < 0) {
        fail(d, "StartInstrument", "the voices", err);
        return err;
    }
    printf("Sound: PSG on the DSP, %d voices (square, randomhold, pulse) into mixer8x2, "
           "amplitude %ld allocated\n", PSG_VOICES, (long)d->amplitude);
    return 0;
}

void psg_close(psg_dsp *d)
{
    uint32 v;

    if (!d->ok)
        return;
    for (v = 0; v < PSG_VOICES; v++) {
        if (d->voice[v] >= 0) {
            StopInstrument(d->voice[v], NULL);
            DisconnectInstruments(d->voice[v], "Output", d->mixer, (char *)mixer_input[v]);
            UnloadInstrument(d->voice[v]);  /* its knobs go with it */
            d->voice[v] = -1;
        }
    }
    if (d->mixer >= 0) {
        StopInstrument(d->mixer, NULL);
        UnloadInstrument(d->mixer);
        d->mixer = -1;
    }
    if (d->amplitude > 0)
        FreeAmplitude(d->amplitude);
    CloseAudioFolio();
    d->ok = 0;
}

uint32 psg_update(psg_dsp *d, const uint32 reg[8], uint32 clock, uint32 stereo)
{
    uint32 t0 = d->tweaks;
    uint32 c;
    uint32 mode;
    uint32 n;
    int32 amp;

    if (!d->ok)
        return 0;
    if ((stereo & 0xFF) != d->stereo)
        set_stereo(d, stereo & 0xFF);
    for (c = 0; c < 3; c++) {
        uint32 p = reg[2 * c] & 0x3FF;

        amp = p < TONE_MIN_PERIOD ? 0 : vol_amp[reg[2 * c + 1] & 15];
        if (amp != 0 && p != d->period[c]) {
            d->period[c] = p;
            set_hz(d, c, hz16(clock, p));
        }
        set_amp(d, c, amp);
    }
    /* Noise: bits 0-1 give the rate, bit 2 white (1) or periodic (0). */
    mode = reg[6];
    amp = vol_amp[reg[7] & 15];
    n = (mode & 3) == 3 ? (reg[4] & 0x3FF) : (16u << (mode & 3));
    if (n == 0)
        amp = 0;        /* tone 2 at period 0: the shift register never advances */
    else if (n < TONE_MIN_PERIOD)
        n = TONE_MIN_PERIOD;
    if (mode & 4) {
        if (amp != 0 && n != d->period[PSG_WHITE]) {
            d->period[PSG_WHITE] = n;
            set_hz(d, PSG_WHITE, hz16(clock, n));
        }
        set_amp(d, PSG_PERIODIC, 0);
        set_amp(d, PSG_WHITE, amp);
    } else {
        if (amp != 0 && n != d->period[PSG_PERIODIC]) {
            d->period[PSG_PERIODIC] = n;
            set_hz(d, PSG_PERIODIC, hz16(clock, n) >> 4);
        }
        set_amp(d, PSG_WHITE, 0);
        set_amp(d, PSG_PERIODIC, amp);
    }
    return d->tweaks - t0;
}

void psg_mute(psg_dsp *d)
{
    uint32 v;

    if (!d->ok)
        return;
    for (v = 0; v < PSG_VOICES; v++)
        set_amp(d, v, 0);
}
