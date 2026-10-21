/* monolith: a techno / industrial drone engine for long, never-repeating pads.
 *
 *   blue  : tone       filter base cutoff (dark .. bright), with half key tracking
 *   green : motion     how much and how fast the drone evolves (0 = static)
 *   white : rust       inharmonic ring mod, wavefolder and hiss (clean .. industrial)
 *   red   : thickness  detune spread of the 3 saws and sub-octave level
 *
 * Signal per voice: 3 saws (root, +spread, -spread) + square one octave down -> ring mod with a sine at a
 * drifting non-integer ratio -> wavefolder -> + noise -> resonant state-variable low-pass -> soft clip.
 *
 * Why it never repeats: the cutoff, the resonance, the beating speed of the detuned saws, the ring ratio and a
 * tiny pitch drift are driven by three slow sine LFOs with incommensurate periods (about 23.7 s, 37.1 s and
 * 61.3 s at motion 37 %, faster above, slower below) plus three random walks that pick a new target at random intervals and glide to it.
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
    /* read by the screen (GUI thread): */
    int32_t  cut_show;                  /* filter position of the last block, 0..32767 (knob units) */
    volatile uint32_t blocks;           /* render calls: changes while the voice sounds */
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
    const int32_t tone = params[0], mov = params[1], rust = params[2], thick = params[3];
    const uint32_t inc0 = (uint32_t)inc[0];

    /* ---- per block: modulation (about 345 blocks per second) ---- */
    /* LFO rates scale with motion: 0.25x .. 2.25x (Q13 factor 2048 .. 18432) */
    uint32_t rate = 2048u + ((uint32_t)mov >> 1);
    static const uint32_t LFO_BASE[3] = { 525600u, 335800u, 203200u };   /* 23.7 s, 37.1 s, 61.3 s at 1x */
    int32_t l[3];
    for (int i = 0; i < 3; i++) {
        v->lfo[i] += ((LFO_BASE[i] >> 5) * rate) >> 8;
        l[i] = sine_q15(v->lfo[i]);
    }
    /* random walks: new target every 0.4 .. 6 s (sooner with more motion), glide towards it */
    int32_t slew = 6 + (mov >> 8);                    /* Q15 per block: 6 .. 133 (glides of ~16 s .. ~0.7 s) */
    for (int i = 0; i < WALKS; i++) {
        if (--v->wnext[i] <= 0) {
            v->wt[i] = rnd_q15(v);
            int32_t span = 2000 - (mov >> 4);         /* 2000 .. ~ -48 blocks of extra wait */
            v->wnext[i] = 140 + (int32_t)((rnd(v) >> 21) * (uint32_t)(span > 64 ? span : 64) >> 11);
        }
        v->w[i] += ((v->wt[i] - v->w[i]) * slew) >> 15;
    }
    int32_t depth = mov;                              /* modulation depth follows motion */

    /* cutoff: knob + half key tracking + LFO1 / walk 1, in knob units (0..32767 over 25 Hz .. 5 kHz) */
    int32_t oct = inc0 ? 31 - __builtin_clz(inc0) : 22;                 /* C2 (65 Hz) ~ 22 */
    int32_t cc = tone + (oct - 22) * 2143;                             /* 4287 knob units = 1 octave */
    int32_t cmod = (l[0] + v->w[0]) >> 1;                                /* -32768 .. 32767 */
    cc += (((cmod * 12000) >> 15) * depth) >> 15;                        /* up to +- 2.8 octaves at full mov */
    cc = clampi(cc, 0, 32767);
    v->cut_show = cc; v->blocks++;
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

/* ======================= screen (GUI thread) =======================
 * After 2001: A Space Odyssey. A monolith (proportions 4:9) stands on the plain; four hominids, one per knob in
 * its colour, walk towards it as the knob goes up and away as it goes down. At the top of the range a hominid
 * reaches the slab and touches it. Blue and green stand left, white and red right; one in front, one behind.
 *
 * The rest follows the drone's live state (mv[] of the voice that sounds), so what moves is what you hear:
 *   three moons    one per slow LFO (24 / 37 / 61 s), rising and setting across the sky; now and then they line
 *                  up over the monolith
 *   blue scan      a line across the slab at the live filter position (knob + LFO + walk + key tracking)
 *   white rust     scratches on the slab, more with rust
 *   red echo       a ghost outline of the slab drifting off it = the detuned saws (thickness)
 *   the slab       hums brighter while a voice plays
 * Geometry in Q10 px (320 x 160). The GUI thread may use 64-bit math and division (not the audio rules). */
#define SC_HORIZON 110
#define SC_BASE    134                 /* the slab stands in front of the horizon */
#define SC_H       88
#define SC_W       40                  /* 4 : 9 */
#define SC_CX      160

static const uint8_t SC_SLAB[4]  = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t SC_KNOB[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                       { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t SC_MOON[4]  = { 0xAE, 0xB1, 0xDC, 0xFF };
static const uint8_t SC_GREY[4]  = { 0x6F, 0x71, 0x73, 0xFF };

/* hominids: side (-1 left, +1 right), feet line, scale (Q10) */
static const int8_t  HM_SIDE[4]  = { -1, -1, 1, 1 };
static const int16_t HM_BASE[4]  = { 154, 128, 128, 154 };
static const int16_t HM_SCALE[4] = { 1300, 1000, 1000, 1300 };
static int32_t hm_x[4], hm_ph[4], hm_mv[4];               /* position (Q10 px), stride phase, moving 0..1024 */
static int     hm_init;

static uint32_t sc_frame, sc_last[OP1_VOICES];
static int      sc_disp;
static int32_t  sc_act;                                    /* 0..1024: something sounds (fast up, slow down) */

static inline int32_t ssin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t scos(int32_t a) { return ssin(a + 2048); }
static inline uint32_t shash(uint32_t a, uint32_t b) { uint32_t h = a * 2654435761u ^ (b + 0x9E37u) * 40503u; h ^= h >> 15; return h * 2246822519u; }

static void sc_dim(uint8_t *o, const uint8_t *c, int32_t l)       /* l = 0..256 */
{
    if (l > 256) l = 256;
    if (l < 0) l = 0;
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
static void sc_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void sc_circle(void *r, int32_t cx, int32_t cy, int32_t rad, int n, int32_t w, const uint8_t *c)
{
    int step = 8192 / n;
    op1_pt p = { cx + rad, cy };
    for (int a = step; a <= 8192; a += step) {
        op1_pt q = { cx + (int32_t)(((int64_t)rad * scos(a)) >> 15), cy + (int32_t)(((int64_t)rad * ssin(a)) >> 15) };
        op1_raster_line(r, &p, &q, w, c);
        p = q;
    }
}
static void sc_rect(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    sc_line(r, x0, y0, x1, y0, w, c); sc_line(r, x1, y0, x1, y1, w, c);
    sc_line(r, x1, y1, x0, y1, w, c); sc_line(r, x0, y1, x0, y0, w, c);
}

/* one hominid, facing the slab. Local units: px, origin between the feet, y up = negative. */
typedef struct { void *r; int32_t x, y, s, face; int32_t w; const uint8_t *c; } hm_ctx;
static inline int32_t hx(const hm_ctx *h, int32_t u) { return h->x + h->face * (int32_t)(((int64_t)u * h->s) >> 10); }
static inline int32_t hy(const hm_ctx *h, int32_t v) { return h->y + (int32_t)(((int64_t)v * h->s) >> 10); }
static void hseg(const hm_ctx *h, int32_t u0, int32_t v0, int32_t u1, int32_t v1)
{ sc_line(h->r, hx(h, u0), hy(h, v0), hx(h, u1), hy(h, v1), h->w, h->c); }

static void hominid(void *r, int i, int32_t touch, const uint8_t *c)
{
    hm_ctx h = { r, hm_x[i], OP1_Q10(HM_BASE[i]), HM_SCALE[i], -HM_SIDE[i], HM_SCALE[i] > 1024 ? 0x600 : 0x480, c };
    int32_t sw = (int32_t)(((int64_t)ssin(hm_ph[i] >> 3) * hm_mv[i]) >> 10);          /* stride swing, Q15 */
    int32_t sx = (int32_t)(((int64_t)OP1_Q10(2.5) * sw) >> 15);                        /* +-2.5 px */
    int32_t bob = hm_mv[i] > 64 ? -(int32_t)(((int64_t)OP1_Q10(0.8) * (sw < 0 ? -sw : sw)) >> 15)
                                : (int32_t)(((int64_t)OP1_Q10(0.4) * ssin((int32_t)sc_frame * 40 + i * 2000)) >> 15);
    int32_t t = touch;                                     /* 0..1024: crouched .. reaching up */
#define TB(a, b) ((a) + (int32_t)(((int64_t)((b) - (a)) * t) >> 10))
    int32_t hipu = TB(OP1_Q10(-6), OP1_Q10(-4)),  hipv = TB(OP1_Q10(-12), OP1_Q10(-13)) + bob;   /* hip */
    int32_t hmu = TB(OP1_Q10(-1), OP1_Q10(-2)),   hmv = TB(OP1_Q10(-19), OP1_Q10(-22)) + bob;    /* hump */
    int32_t shu = TB(OP1_Q10(5), OP1_Q10(3)),     shv = TB(OP1_Q10(-18), OP1_Q10(-25)) + bob;    /* shoulder */
    int32_t hdu = TB(OP1_Q10(10), OP1_Q10(7)),    hdv = TB(OP1_Q10(-16), OP1_Q10(-28)) + bob;    /* head, low and forward */
    /* legs: short and bent */
    hseg(&h, hipu, hipv, OP1_Q10(-4) + sx, OP1_Q10(-6));
    hseg(&h, OP1_Q10(-4) + sx, OP1_Q10(-6), OP1_Q10(-4) + sx, 0);
    hseg(&h, OP1_Q10(-4) + sx, 0, OP1_Q10(-1) + sx, 0);
    hseg(&h, hipu, hipv, OP1_Q10(-9) - sx, OP1_Q10(-6));
    hseg(&h, OP1_Q10(-9) - sx, OP1_Q10(-6), OP1_Q10(-8) - sx, 0);
    hseg(&h, OP1_Q10(-8) - sx, 0, OP1_Q10(-5) - sx, 0);
    /* curved back (hip -> hump -> shoulder) and belly */
    hseg(&h, hipu, hipv, hmu, hmv);
    hseg(&h, hmu, hmv, shu, shv);
    hseg(&h, hipu, hipv, TB(OP1_Q10(2), OP1_Q10(1)), TB(OP1_Q10(-11), OP1_Q10(-15)) + bob);
    hseg(&h, TB(OP1_Q10(2), OP1_Q10(1)), TB(OP1_Q10(-11), OP1_Q10(-15)) + bob, shu, shv + OP1_Q10(2));
    /* head: skull, brow ridge, muzzle */
    sc_circle(r, hx(&h, hdu), hy(&h, hdv), (OP1_Q10(2.6) * HM_SCALE[i]) >> 10, 10, 0x400, c);
    hseg(&h, hdu - OP1_Q10(1.5), hdv - OP1_Q10(2.8), hdu + OP1_Q10(3.2), hdv - OP1_Q10(2));
    hseg(&h, hdu + OP1_Q10(3), hdv - OP1_Q10(0.5), hdu + OP1_Q10(3.5), hdv + OP1_Q10(2));
    hseg(&h, hdu + OP1_Q10(3.5), hdv + OP1_Q10(2), hdu + OP1_Q10(0.5), hdv + OP1_Q10(3));
    /* back arm: long, knuckles on the ground, swinging against the legs */
    hseg(&h, shu - OP1_Q10(1), shv + OP1_Q10(1), OP1_Q10(3) - sx / 2, OP1_Q10(-8));
    hseg(&h, OP1_Q10(3) - sx / 2, OP1_Q10(-8), OP1_Q10(4) - sx, 0);
    /* front arm: knuckle-walking, or raised to the slab */
    int32_t eu = TB(OP1_Q10(8) + sx / 2, OP1_Q10(9)),  ev = TB(OP1_Q10(-9), OP1_Q10(-30)) + bob;
    int32_t pu = TB(OP1_Q10(10) + sx, OP1_Q10(12)),    pv = TB(0, OP1_Q10(-36));
    hseg(&h, shu, shv, eu, ev);
    hseg(&h, eu, ev, pu, pv);
#undef TB
    if (touch > 900) {                                     /* contact: a flicker on the slab under the palm */
        int32_t x = hx(&h, pu + OP1_Q10(1.2)), y = hy(&h, pv);
        uint8_t g[4]; sc_dim(g, c, 120 + (int32_t)(shash((uint32_t)i, sc_frame >> 1) & 127u));
        sc_line(r, x, y - OP1_Q10(4), x, y + OP1_Q10(4), 0x400, g);
    }
}

static void mono_screen_init(void *scr) { (void)scr; }

static void mono_screen_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    sc_frame++;

    /* which voice to show: the one that rendered most recently */
    int any = 0;
    for (int i = 0; i < OP1_VOICES; i++) {
        uint32_t b = mv[i].blocks;
        if (b != sc_last[i]) { sc_last[i] = b; if (!any) sc_disp = i; any = 1; }
    }
    sc_act = any ? sc_act + ((1024 - sc_act) >> 2) : sc_act - (sc_act >> 5);
    const mono_voice_t *v = &mv[sc_disp];
    int live = v->init == 0x6D6F6E6Fu;                     /* never played: a calm default sky */
    uint32_t lf[3] = { live ? v->lfo[0] : 0x30000000u, live ? v->lfo[1] : 0x48000000u, live ? v->lfo[2] : 0x58000000u };
    int32_t  cut = live ? v->cut_show : k[0];

    op1_screen_draw(scr, dt);                              /* the horizon and the rocks (SVG) */
    void *r = op1_raster();
    uint8_t c[4];
    const int32_t mov = k[1], rust = k[2], thick = k[3];
    int32_t xl = OP1_Q10(SC_CX - SC_W / 2), xr = OP1_Q10(SC_CX + SC_W / 2);
    int32_t yt = OP1_Q10(SC_BASE - SC_H), yb = OP1_Q10(SC_BASE);

    /* the sky: three moons, one per LFO, on arcs from horizon to horizon */
    int32_t mxs[3];
    for (int i = 0; i < 3; i++) {
        int32_t a = (int32_t)(lf[i] >> 19);                /* 0..8191 per LFO cycle */
        int32_t x = OP1_Q10(SC_CX) + (int32_t)(((int64_t)OP1_Q10(140) * scos(a)) >> 15);
        int32_t s = ssin(a); if (s < 0) s = -s;            /* up and over, twice per cycle */
        int32_t y = OP1_Q10(SC_HORIZON - 4) - (int32_t)(((int64_t)OP1_Q10(78 - 10 * i) * s) >> 15);
        mxs[i] = x;
        if (x > xl - OP1_Q10(6) && x < xr + OP1_Q10(6) && y > yt - OP1_Q10(6)) continue;   /* behind the slab */
        int32_t rad = OP1_Q10(i == 2 ? 6 : 3 - i);
        sc_dim(c, SC_MOON, 110 + (mov * 100 >> 15));
        sc_circle(r, x, y, rad, i == 2 ? 14 : 8, 0x500, c);
    }
    {   /* alignment: all three within a few px of the slab's axis */
        int al = 1;
        for (int i = 0; i < 3; i++) { int32_t d = mxs[i] - OP1_Q10(SC_CX); if (d < -OP1_Q10(8) || d > OP1_Q10(8)) al = 0; }
        if (al) { sc_dim(c, SC_SLAB, 120); sc_line(r, OP1_Q10(SC_CX), OP1_Q10(4), OP1_Q10(SC_CX), yt - OP1_Q10(3), 0x300, c); }
    }

    /* red echo of the slab: the detuned saws drifting apart (its depth, d = 0 below 512) */
    int32_t d = 0;
    if (thick > 512) {
        d = OP1_Q10(1) + (int32_t)(((int64_t)OP1_Q10(4) * thick) >> 15)
                  + (int32_t)(((((int64_t)OP1_Q10(2) * ssin((int32_t)(lf[1] >> 19))) >> 15) * mov) >> 15);
        if (d < OP1_Q10(1)) d = OP1_Q10(1);
        sc_dim(c, SC_KNOB[3], 60 + (thick * 110 >> 15));
        sc_line(r, xl + d, yt - d, xr + d, yt - d, 0x400, c);
        sc_line(r, xr + d, yt - d, xr + d, yb - d, 0x400, c);
    }

    /* the horizon, interrupted by the slab and its depth: the monolith is solid */
    sc_line(r, OP1_Q10(8), OP1_Q10(SC_HORIZON) + 512, xl - 512, OP1_Q10(SC_HORIZON) + 512, 0x400, SC_GREY);
    sc_line(r, xr + d + 512, OP1_Q10(SC_HORIZON) + 512, OP1_Q10(312), OP1_Q10(SC_HORIZON) + 512, 0x400, SC_GREY);

    /* the slab */
    sc_dim(c, SC_SLAB, 150 + (sc_act * 106 >> 10));
    sc_rect(r, xl, yt, xr, yb, 0x700, c);

    /* white rust: scratches on the face, flickering */
    int nsc = rust * 26 >> 15;
    for (int s = 0; s < nsc; s++) {
        uint32_t h = shash((uint32_t)s, 99);
        int32_t x = xl + OP1_Q10(4) + (int32_t)(h % (uint32_t)(xr - xl - OP1_Q10(10)));
        int32_t y = yt + OP1_Q10(5) + (int32_t)((h >> 8) % (uint32_t)(yb - yt - OP1_Q10(10)));
        sc_dim(c, SC_KNOB[2], 70 + (int32_t)(shash((uint32_t)s, sc_frame >> 3) & 127u) + (rust >> 9));
        int32_t len = OP1_Q10(1.5) + (int32_t)((h >> 20) & 0x7FF);
        sc_line(r, x, y, x + len, y + ((h & 1) ? len : -len) / 2, 0x300, c);
    }

    /* blue scan line at the live filter position */
    {
        int32_t y = yb - OP1_Q10(4) - (int32_t)(((int64_t)(yb - yt - OP1_Q10(8)) * cut) >> 15);
        sc_dim(c, SC_KNOB[0], 80);
        sc_line(r, xl + OP1_Q10(2), y - OP1_Q10(2), xr - OP1_Q10(2), y - OP1_Q10(2), 0x400, c);
        sc_line(r, xl + OP1_Q10(2), y + OP1_Q10(2), xr - OP1_Q10(2), y + OP1_Q10(2), 0x400, c);
        sc_line(r, xl + OP1_Q10(2), y, xr - OP1_Q10(2), y, 0x700, SC_KNOB[0]);
    }

    /* hominids: knob 0 far .. 32767 at the slab; they walk, they do not jump */
    for (int i = 0; i < 4; i++) {
        int32_t reach = (OP1_Q10(13.5) * HM_SCALE[i]) >> 10;              /* hand on the slab at the top */
        int32_t edge = HM_SIDE[i] < 0 ? xl - reach : xr + reach;
        int32_t far = HM_BASE[i] < 140 ? (HM_SIDE[i] < 0 ? OP1_Q10(62) : OP1_Q10(258))
                                      : (HM_SIDE[i] < 0 ? OP1_Q10(24) : OP1_Q10(296));
        int32_t tgt = far + (int32_t)(((int64_t)(edge - far) * k[i]) >> 15);
        if (!hm_init) hm_x[i] = tgt;
        int32_t st = tgt - hm_x[i], step = OP1_Q10(1.4);
        if (st > step) st = step;
        if (st < -step) st = -step;
        hm_x[i] += st;
        hm_ph[i] += (st < 0 ? -st : st) * 8;               /* one stride per ~8 px (phase >> 3 indexes the table) */
        hm_mv[i] = st != 0 ? hm_mv[i] + ((1024 - hm_mv[i]) >> 1) : hm_mv[i] - (hm_mv[i] >> 2);
    }
    hm_init = 1;
    /* back row first, then the front row */
    static const int ORDER[4] = { 1, 2, 0, 3 };
    for (int n = 0; n < 4; n++) {
        int i = ORDER[n];
        int32_t reach = (OP1_Q10(13.5) * HM_SCALE[i]) >> 10;
        int32_t edge = HM_SIDE[i] < 0 ? xl - reach : xr + reach;
        int32_t gap = HM_SIDE[i] < 0 ? edge - hm_x[i] : hm_x[i] - edge;
        int32_t touch = (k[i] >= 31000 && gap < OP1_Q10(6)) ? (int32_t)((int64_t)(OP1_Q10(6) - (gap > 0 ? gap : 0)) * 1024 / OP1_Q10(6)) : 0;
        uint8_t hc[4]; sc_dim(hc, SC_KNOB[i], HM_BASE[i] < 140 ? 190 : 256);
        hominid(r, i, touch, hc);
    }
}

const op1_screen_t SCREEN_monolith = { mono_screen_init, mono_screen_update };
