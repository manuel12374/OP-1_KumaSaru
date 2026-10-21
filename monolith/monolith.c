/* monolith: a techno / industrial drone engine for long, never-repeating pads.
 *
 *   blue  : colore     filter base cutoff (dark .. bright), with half key tracking
 *   green : movimento  how much and how fast the drone evolves (0 = static)
 *   white : ruggine    rust: inharmonic ring mod, wavefolder and hiss (clean .. industrial)
 *   red   : spessore   thickness: detune spread of the 3 saws and sub-octave level
 *
 * Signal per voice: 3 saws (root, +spread, -spread) + square one octave down -> ring mod with a sine at a
 * drifting non-integer ratio -> wavefolder -> + noise -> resonant state-variable low-pass -> soft clip.
 *
 * Why it never repeats: the cutoff, the resonance, the beating speed of the detuned saws, the ring ratio and a
 * tiny pitch drift are driven by three slow sine LFOs with incommensurate periods (about 23.7 s, 37.1 s and
 * 61.3 s at movimento 37 %, faster above, slower below) plus three random walks that pick a new target at random intervals and glide to it.
 * The modulation state lives in a static array per voice, so it keeps running across notes: a drone played
 * again does not restart from the same point.
 *
 * Integer only (Q15), no per-sample division, no 64-bit math, no libc. Every multiply is bounded so that it
 * cannot overflow int32 (checked with the harness under -fsanitize=undefined).
 *
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

/* cutoff control 0..32 -> Chamberlin SVF coefficient f = 2 sin(pi fc / fs) in Q15, fc = 25 Hz * 200^(k/32)
 * (25 Hz .. 5 kHz, exponential: one step of the knob is the same musical interval everywhere) */
static const int16_t F_TABLE[33] = {
    117, 138, 163, 192, 226, 267, 315, 372, 439, 518, 611, 721, 851, 1004, 1185, 1399, 1650,
    1948, 2298, 2712, 3200, 3775, 4454, 5254, 6198, 7310, 8619, 10159, 11970, 14094, 16581, 19482, 22853 };

#define ST_MAX   80000          /* SVF state clamp: f (<= 22853) * 80000 stays below 2^31 */
#define HI_MAX   65535          /* high-pass clamp before its multiply */
#define WALKS    3

typedef struct {
    uint32_t p1, p2, p3, psub, pring;   /* oscillator phases (2^32 = one cycle) */
    int32_t  low, band;                 /* state-variable filter */
    uint32_t rng;                       /* per-voice LCG */
    uint32_t lfo[3];                    /* slow LFO phases */
    int32_t  w[WALKS], wt[WALKS];       /* random walks: value and target, Q15 signed */
    int32_t  wnext[WALKS];              /* blocks until each walk picks a new target */
    uint32_t init;                      /* set once the voice has been seeded */
} mono_voice_t;

/* the 32 bytes of the firmware voice object are shared with other engines: keep our state here instead */
static mono_voice_t mv[OP1_VOICES];

static inline uint32_t rnd(mono_voice_t *v) { v->rng = v->rng * 1664525u + 1013904223u; return v->rng; }
static inline int32_t rnd_q15(mono_voice_t *v) { return (int32_t)(rnd(v) >> 16) - 32768; }   /* -32768 .. 32767 */
static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int32_t sine_q15(uint32_t ph) { return OP1_SINE_TABLE[ph >> 19]; }

static void mono_note_on(void *self, const int16_t *params)
{
    int vi = op1_voice_index(self);
    mono_voice_t *v = &mv[vi];
    (void)params;
    if (v->init != 0x6D6F6E6Fu) {                     /* first note on this voice: seed everything once */
        v->rng = 0x9E3779B9u * (uint32_t)(vi + 1) + 0x7F4A7C15u;
        v->p1 = rnd(v); v->p2 = rnd(v); v->p3 = rnd(v); v->psub = rnd(v); v->pring = rnd(v);
        for (int i = 0; i < 3; i++) v->lfo[i] = rnd(v);
        for (int i = 0; i < WALKS; i++) { v->w[i] = rnd_q15(v) >> 1; v->wt[i] = rnd_q15(v); v->wnext[i] = 0; }
        v->low = v->band = 0;
        v->init = 0x6D6F6E6Fu;
    }
    /* later notes keep phases, LFOs and walks running (continuity, no repetition); only the filter restarts
     * cleanly, under the stock envelope's attack */
    v->low = v->band = 0;
}

static void mono_render(void *self, int16_t *out, const int16_t *params)
{
    mono_voice_t *v = &mv[op1_voice_index(self)];
    const int32_t *inc = OP1_PITCH_INC;
    const int32_t colore = params[0], mov = params[1], rust = params[2], thick = params[3];
    const uint32_t inc0 = (uint32_t)inc[0];

    /* ---- per block: modulation (about 345 blocks per second) ---- */
    /* LFO rates scale with movimento: 0.25x .. 2.25x (Q13 factor 2048 .. 18432) */
    uint32_t rate = 2048u + ((uint32_t)mov >> 1);
    static const uint32_t LFO_BASE[3] = { 525600u, 335800u, 203200u };   /* 23.7 s, 37.1 s, 61.3 s at 1x */
    int32_t l[3];
    for (int i = 0; i < 3; i++) {
        v->lfo[i] += ((LFO_BASE[i] >> 5) * rate) >> 8;
        l[i] = sine_q15(v->lfo[i]);
    }
    /* random walks: new target every 0.4 .. 6 s (sooner with more movimento), glide towards it */
    int32_t slew = 6 + (mov >> 8);                    /* Q15 per block: 6 .. 133 (glides of ~16 s .. ~0.7 s) */
    for (int i = 0; i < WALKS; i++) {
        if (--v->wnext[i] <= 0) {
            v->wt[i] = rnd_q15(v);
            int32_t span = 2000 - (mov >> 4);         /* 2000 .. ~ -48 blocks of extra wait */
            v->wnext[i] = 140 + (int32_t)((rnd(v) >> 21) * (uint32_t)(span > 64 ? span : 64) >> 11);
        }
        v->w[i] += ((v->wt[i] - v->w[i]) * slew) >> 15;
    }
    int32_t depth = mov;                              /* modulation depth follows movimento */

    /* cutoff: knob + half key tracking + LFO1 / walk 1, in knob units (0..32767 over 25 Hz .. 5 kHz) */
    int32_t oct = inc0 ? 31 - __builtin_clz(inc0) : 22;                 /* C2 (65 Hz) ~ 22 */
    int32_t cc = colore + (oct - 22) * 2143;                             /* 4287 knob units = 1 octave */
    int32_t cmod = (l[0] + v->w[0]) >> 1;                                /* -32768 .. 32767 */
    cc += (((cmod * 12000) >> 15) * depth) >> 15;                        /* up to +- 2.8 octaves at full mov */
    cc = clampi(cc, 0, 32767);
    int32_t ki = cc >> 10, kf = cc & 1023;
    int32_t f = F_TABLE[ki] + (((F_TABLE[ki < 32 ? ki + 1 : 32] - F_TABLE[ki]) * kf) >> 10);

    /* resonance: damping q in Q12 (lower = more resonant), breathing with LFO3 */
    int32_t q = 2600 - ((((l[2] * 900) >> 15) * depth) >> 15);           /* 1700 .. 3500 */
    q = clampi(q, 1500, 4096);

    /* detune spread 0 .. ~3 %, beating speed swayed by LFO2 */
    int32_t dt = thick >> 5;                                             /* 0 .. 1023 (Q15 of the increment) */
    dt += (((dt * l[1]) >> 16) * depth) >> 15;                           /* +- 50 % at full mov */
    uint32_t dinc = (inc0 >> 15) * (uint32_t)(dt > 0 ? dt : 0);
    int32_t sublv = thick;                                               /* sub-octave square level, Q15 */

    /* tiny pitch drift (+- ~3 cents) from walk 3 */
    int32_t dr = ((v->w[2] >> 9) * depth) >> 15;                         /* -64 .. 63 */
    uint32_t drift = (inc0 >> 15) * (uint32_t)(dr < 0 ? -dr : dr);

    /* rust: ring ratio drifts around 1.41 (non-integer: inharmonic, metallic), ring amount, fold, hiss */
    int32_t ratio = 5776 + ((v->w[1] * 2048) >> 15) + ((l[2] * 600) >> 15);   /* Q12, ~0.75 .. ~1.97 x 1.41 */
    ratio = clampi(ratio, 3500, 9500);
    uint32_t ringinc = (inc0 >> 12) * (uint32_t)ratio;
    int32_t ringamt = (int32_t)(((uint32_t)rust * (uint32_t)rust) >> 15);   /* square curve: gentle start */
    if (ringamt > 32000) ringamt = 32000;
    int32_t drive = 4096 + ((rust * 3) >> 3);                            /* Q12: 1x .. ~4x */
    int32_t hiss = rust >> 3;                                            /* noise level, Q15 */

    int32_t gain = op1_voice_vel_gain(self);
    uint32_t p1 = v->p1, p2 = v->p2, p3 = v->p3, ps = v->psub, pr = v->pring, rng = v->rng;
    int32_t low = v->low, band = v->band;

    /* ---- per sample ---- */
    for (int n = 0; n < OP1_BLOCK; n++) {
        uint32_t i = (uint32_t)inc[n];
        i = dr < 0 ? i - drift : i + drift;
        p1 += i; p2 += i + dinc; p3 += i - dinc; ps += i >> 1; pr += ringinc;

        int32_t x = ((int32_t)p1 >> 18) + ((int32_t)p2 >> 18) + ((int32_t)p3 >> 18);   /* 3 saws, +- 24576 */
        x += (((int32_t)ps < 0 ? -8192 : 8192) * sublv) >> 15;                         /* sub, +- 8192 */

        int32_t r = OP1_SINE_TABLE[pr >> 19];
        int32_t xr = (x * r) >> 15;                                                    /* ring modulated */
        x += ((xr - x) * ringamt) >> 15;

        int32_t y = (x * drive) >> 12;                                                 /* wavefolder */
        for (int k = 0; k < 3; k++) {
            if (y > 32767) y = 65534 - y;
            else if (y < -32767) y = -65534 - y;
            else break;
        }

        rng = rng * 1664525u + 1013904223u;
        int32_t in = (y >> 1) + ((((int32_t)rng >> 18) * hiss) >> 15);                 /* +- ~16900 */

        /* Chamberlin state-variable low-pass */
        low += (f * band) >> 15;
        low = clampi(low, -ST_MAX, ST_MAX);
        int32_t high = in - low - ((q * band) >> 12);
        high = clampi(high, -HI_MAX, HI_MAX);
        band += (f * high) >> 15;
        band = clampi(band, -ST_MAX, ST_MAX);

        int32_t s = low + (band >> 3);                                                 /* a little band for bite */
        s = clampi((s * 3) >> 2, -32767, 32767);
        s -= (((((s * s) >> 15) * s) >> 15) * 10923) >> 15;                            /* soft clip x - x^3/3 */
        out[n] = op1_sat16((s * gain) >> 15);
    }
    v->p1 = p1; v->p2 = p2; v->p3 = p3; v->psub = ps; v->pring = pr; v->rng = rng;
    v->low = low; v->band = band;
}

const op1_engine_t ENGINE_monolith = { mono_note_on, mono_render, 0,
    { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
