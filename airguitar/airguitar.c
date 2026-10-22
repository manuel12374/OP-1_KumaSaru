/* "airguitar": six plucked strings (Karplus-Strong) strummed by moving the OP-1 like a guitar pick.
 *
 * Hold the notes of a chord on the keys (or one key with the op1r "chord" sequencer), then strum the air: a stroke
 * down plays the strings from the lowest note to the highest, a stroke up the other way round, a few ms apart
 * (faster strokes = tighter and louder). Each held note is a string (6 voices = 6 strings).
 *   blue  (8 steps)  : guitar: CLASSIC (nylon), FLAMENCO, FOLK (steel), JUMBO, JAZZ (archtop), CLEAN (electric),
 *                      CRUNCH (electric, driven), BANJO
 *   green            : drive (soft clip per string)
 *                      page 2 (press): FORCE, how hard a stroke must be: 0 = light (default) .. 100 = hard
 *   white            : tone: from soft (plucked over the neck) to bright (near the bridge)
 *   red              : sustain, from palm-muted to ringing
 *                      page 2 (press): play mode: left half GESTURE (the keys only fret the strings, they sound when
 *                      you strum: the default), right half GESTURE + KEYS (a key also plucks its string at once)
 * Reverb and delay: the op1r FX (instrument FX, master FX), one effect for all six strings as on a guitar rig.
 * Envelope (T2): attack 0, sustain up; lifting the keys stops the strings, like lifting the fretting hand.
 *
 * Strum detector (shared by the voices, run once per new sensor reading, ~69 Hz): per axis the reading minus a slow
 * average (gravity, drift) is integrated into a leaky velocity; its component along gravity is the stroke (down /
 * up whatever way the OP-1 is held). A stroke fires when that crosses the threshold (green page 2) and re-arms when it
 * falls below a third of it, so one stroke fires once (its braking does not fire a second one).
 * Per voice: Karplus-Strong delay line (4096 samples, linear interpolation for the fraction), loop filter
 * (two-point average blended with the plain sample by the tone, then a one-pole low-pass, its delay compensated in
 * the tuning), decay per sample, a body resonance (state-variable band-pass) mixed in, drive. A pluck = a burst of
 * low-passed noise written into the delay line, shaped by a comb for the pluck position.
 * Integer only; divisions once per block; no libc. State in our own arrays (the 32 voice bytes are not enough).
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

#ifndef OP1_HOST
#define AG_ACCEL ((const volatile int16_t *)0xFF801054)    /* ADXL345 copy (aether): X, Y, Z, 1 g = 16384, ~69 Hz */
#else
extern int16_t host_accel[3];
#define AG_ACCEL host_accel
#endif

#define AG_VERSION "V1.0"
#define AG_LEN   4096                                       /* delay line (power of two): down to ~11 Hz */
#define AG_MASK  (AG_LEN - 1)
#define AG_TYPES 8
#define AG_KEYS_MODE(p) ((p)[OP1_ENG_PAGE_WORD(3)] >= 16384)   /* red page 2, right half */

typedef struct {
    const char *name;
    int16_t bright;        /* loop: 0 = two-point average (dark) .. 32767 = plain sample (bright), Q15 */
    int16_t lpa;           /* loop one-pole low-pass coefficient Q15 (32767 = off) */
    int16_t decay;         /* loss per sample of string period, Q20 (T60 ~ 6.9 / (decay / 2^20 * 44100)) */
    int16_t pick;          /* pick hardness: noise low-pass coefficient Q15 */
    int16_t pos;           /* pluck position, Q15 of the string length (comb) */
    int16_t body_f;        /* body resonance: SVF coefficient 2 sin(pi f / fs), Q15 */
    int16_t body_amt;      /* body mix, Q15 */
    int16_t drive0;        /* drive added to the green knob, Q15 */
    int16_t pick_x;        /* screen: where the pick crosses the strings (px) */
} ag_type_t;
static const ag_type_t ag_types[AG_TYPES] = {
    { "CLASSIC",   6000, 26000, 19,  9000, 14700,  467, 14000,     0, 262 },   /* nylon, body ~100 Hz */
    { "FLAMENCO", 13000, 30000, 32, 17000,  9800,  840, 11000,     0, 262 },   /* nylon, brighter, shorter */
    { "FOLK",     20000, 32000, 14, 22000,  8200,  560, 12000,     0, 262 },   /* steel dreadnought */
    { "JUMBO",    18000, 31000, 12, 20000,  9000,  420, 18000,     0, 262 },   /* steel, big low body */
    { "JAZZ",      4000, 20000, 27, 12000, 11000, 1027, 10000,  2000, 238 },   /* archtop, warm and short */
    { "CLEAN",    16000, 30000, 10, 18000,  6500,  700,  3000,  1500, 233 },   /* electric, long sustain */
    { "CRUNCH",   16000, 30000, 10, 20000,  6500,  700,  3000, 20000, 233 },   /* electric, driven */
    { "BANJO",    26000, 32767, 48, 28000,  3900, 1866, 20000,     0, 250 },   /* bright, short, twangy */
};

typedef struct {
    int16_t  buf[AG_LEN];
    int32_t  w;                       /* write index */
    uint32_t inc;                     /* pitch (phase increment) of the last block */
    int32_t  per;                     /* period, samples Q8 */
    int32_t  yprev, lp, bl, bb, dc;   /* loop filter, body SVF, DC blocker */
    int32_t  pending;                 /* samples until the pluck (-1: none) */
    int32_t  pamp;                    /* pluck amplitude, Q15 (-1: the key's velocity) */
    uint32_t seen;                    /* strum id handled */
    uint32_t rng;
    volatile int32_t  level;          /* output level for the screen, Q15 */
    volatile uint32_t plucks;         /* plucks so far */
    volatile uint8_t  held;           /* key down (note_on .. note_off) */
} ag_voice_t;
static ag_voice_t gv[OP1_VOICES];

/* the strum detector (one for all voices) */
#define AG_AVG_SH  4                                        /* gravity estimate: ~16 readings (~0.23 s) */
#define AG_LEAK_SH 5                                        /* velocity leak: ~32 readings (~0.46 s) */
static struct {
    int16_t  raw[3];                  /* last sensor reading seen */
    int32_t  avg[3];                  /* gravity estimate, raw << 8 */
    int32_t  vel;                     /* leaky velocity along gravity, raw units, + = up */
    int32_t  armed, since;            /* re-armed; readings since the last stroke */
    volatile int32_t  proj;           /* = vel (screen meter) */
    volatile uint32_t id;             /* strokes so far */
    volatile int8_t   dir;            /* +1 down (low -> high strings), -1 up */
    int32_t  strength;                /* Q15 */
    uint8_t  init;
} gs;

static inline int32_t ag_abs(int32_t a) { return a < 0 ? -a : a; }
static int ag_type(const int16_t *p) { int t = p[0] * AG_TYPES >> 15; return t < 0 ? 0 : (t >= AG_TYPES ? AG_TYPES - 1 : t); }
/* stroke threshold (velocity along gravity, raw units: 16384 = 1 g for one reading = 0.14 m/s) from FORCE (green
 * page 2): light 20000 (~0.2 m/s) .. hard 60000 (~0.6 m/s) */
static int32_t ag_threshold(const int16_t *p)
{
    int32_t f = p[OP1_ENG_PAGE_WORD(1)];
    if (f < 0) f = 0;
    return 20000 + (int32_t)(((int64_t)f * 40000) >> 15);
}

/* one step per new sensor reading: the reading minus the gravity estimate, projected on gravity (|g| ~ 16384 ->
 * scaled back with >> 14), integrated with a leak: the velocity along gravity, + = up. A stroke fires when |vel|
 * crosses the threshold while rising (a fading rest never fires); it re-arms below a third of the threshold or when
 * the velocity turns round. A stroke the other way needs 10 readings (~145 ms) since the last one: the braking of a
 * stroke, with the sensor clipped at 2 g, looks like a stroke back. Strength from the rise per reading.
 * (Simulated: alternating strokes at 70..160 BPM in eighths, 0.3..2 g, all caught; turning the OP-1 never fires.) */
static void ag_detect(const int16_t *params)
{
    int16_t r[3] = { AG_ACCEL[0], AG_ACCEL[1], AG_ACCEL[2] };
    if (!gs.init) {
        for (int i = 0; i < 3; i++) { gs.raw[i] = r[i]; gs.avg[i] = r[i] * 256; }
        gs.vel = 0; gs.armed = 1; gs.since = 99; gs.init = 1;
        return;
    }
    if (r[0] == gs.raw[0] && r[1] == gs.raw[1] && r[2] == gs.raw[2]) return;   /* no new reading */
    int32_t dot = 0;
    for (int i = 0; i < 3; i++) {
        gs.raw[i] = r[i];
        int32_t g = gs.avg[i] >> 8;
        dot += (r[i] - g) * (g >> 2);                          /* |hp| <= 2^16, |g >> 2| <= 2^13 */
        gs.avg[i] += ((r[i] * 256) - gs.avg[i]) >> AG_AVG_SH;
    }
    int32_t prev = gs.vel;
    gs.vel += (dot >> 12) - (gs.vel >> AG_LEAK_SH);
    if (gs.vel > 400000) gs.vel = 400000;
    if (gs.vel < -400000) gs.vel = -400000;
    int32_t mag = ag_abs(gs.vel);
    gs.proj = gs.vel;
    int32_t thr = ag_threshold(params);
    if (gs.since < 99) gs.since++;
    int8_t dir = (int8_t)(gs.vel < 0 ? 1 : -1);               /* the reading points up: moving against it = down */
    if (gs.armed && mag > thr && mag > ag_abs(prev) && (dir == gs.dir || gs.since > 9)) {
        int32_t rise = ag_abs(gs.vel - prev);
        int32_t s = 12000 + (int32_t)((int64_t)rise * 10000 / thr);
        gs.strength = s > 32767 ? 32767 : s;
        gs.dir = dir; gs.armed = 0; gs.since = 0;
        gs.id++;
    } else if (!gs.armed && (mag < thr / 3 || dir != gs.dir)) gs.armed = 1;
}

/* write a pluck into the delay line of v: low-passed noise over one period, comb for the pluck position */
static void ag_pluck(ag_voice_t *v, const ag_type_t *t, int32_t tone, int32_t amp)
{
    int32_t n = v->per >> 8;
    if (n < 4) n = 4;
    int32_t pick = t->pick + ((tone - 16384) >> 1);
    if (pick < 2000) pick = 2000;
    if (pick > 32767) pick = 32767;
    int32_t x = 0;
    for (int32_t i = 0; i < n; i++) {
        v->rng = v->rng * 1664525u + 1013904223u;
        int32_t nz = (int32_t)(v->rng >> 16) - 32768;
        x += ((nz - x) * pick) >> 15;
        int32_t idx = (v->w - n + i) & AG_MASK;
        v->buf[idx] = (int16_t)((v->buf[idx] * 6554 >> 15) + ((x * amp) >> 16));   /* 20 % of what rang on */
    }
    int32_t pos = t->pos - ((tone - 16384) >> 3);          /* brighter tone: nearer the bridge */
    if (pos < 2000) pos = 2000;
    if (pos > 16384) pos = 16384;
    int32_t p = (n * pos) >> 15;
    if (p < 1) p = 1;
    for (int32_t i = n - 1; i >= p; i--) {                 /* comb: x[i] - x[i - p] / 2, backwards (in place) */
        int32_t a = (v->w - n + i) & AG_MASK, b = (v->w - n + i - p) & AG_MASK;
        v->buf[a] = op1_sat16(v->buf[a] - (v->buf[b] >> 1));
    }
    v->plucks++;
}

static void ag_note_on(void *self, const int16_t *params)
{
    ag_voice_t *v = &gv[op1_voice_index(self) % OP1_VOICES];
    v->held = 1;
    v->seen = gs.id;                                       /* strokes before this note do not pluck it */
    v->rng ^= (uint32_t)(uintptr_t)self * 2654435761u + 977u;
    if (AG_KEYS_MODE(params)) { v->pending = 0; v->pamp = -1; }   /* the key plucks at once (velocity in render) */
    else {                                                 /* GESTURE: fretted, silent until a stroke */
        v->pending = -1;
        for (int i = 0; i < AG_LEN; i++) v->buf[i] = 0;
        v->lp = v->yprev = v->bl = v->bb = v->dc = 0;
    }
}
static void ag_note_off(void *self) { gv[op1_voice_index(self) % OP1_VOICES].held = 0; }

static void ag_render(void *self, int16_t *out, const int16_t *params)
{
    int vi = op1_voice_index(self) % OP1_VOICES;
    ag_voice_t *v = &gv[vi];
    const ag_type_t *t = &ag_types[ag_type(params)];
    ag_detect(params);

    /* loop filter of this block */
    int32_t tone = params[2], sus = params[3];
    int32_t bright = t->bright + ((tone - 16384) >> 1);
    if (bright < 0) bright = 0;
    if (bright > 32767) bright = 32767;
    int32_t lpa = t->lpa;
    if (sus < 8000) lpa = (lpa * (12000 + sus * 2)) >> 15;   /* palm mute: darker */
    if (lpa < 1000) lpa = 1000;
    /* pitch -> delay length: period = 2^32 / inc samples, minus the loop filter's delay (average: half a sample
     * when dark, one-pole: (1 - a) / a) so the string stays in tune */
    uint32_t inc = (uint32_t)OP1_PITCH_INC[0];
    if (!inc) inc = 1;
    v->inc = inc;
    int64_t per = ((int64_t)1 << 40) / inc;               /* Q8 samples */
    per -= (128 * (32767 - bright)) >> 15;
    per -= ((int64_t)(32767 - lpa) << 8) / lpa;
    if (per > (int64_t)(AG_LEN - 4) << 8) per = (int64_t)(AG_LEN - 4) << 8;
    if (per < 4 << 8) per = 4 << 8;
    v->per = (int32_t)per;

    /* a new stroke: this string's turn in it, by pitch among the held strings */
    if (gs.id != v->seen) {
        v->seen = gs.id;
        if (v->held) {
            int rank = 0, count = 0;
            for (int k = 0; k < OP1_VOICES; k++) if (gv[k].held) {
                count++;
                if (k != vi && (gv[k].inc < v->inc || (gv[k].inc == v->inc && k < vi))) rank++;
            }
            int order = gs.dir > 0 ? rank : count - 1 - rank;
            int32_t spread = 800 - ((gs.strength * 540) >> 15);   /* ~18 .. 6 ms per string */
            v->pending = order * spread;
            v->pamp = gs.strength;
        }
    }
    if (v->pending >= 0) {
        if (v->pending < OP1_BLOCK) {
            ag_pluck(v, t, tone, v->pamp < 0 ? op1_voice_vel_gain(self) : v->pamp);
            v->pending = -1;
        } else v->pending -= OP1_BLOCK;
    }

    int32_t ni = v->per >> 8, fr = v->per & 255;
    /* loss per pass round the string, Q16: the rate per sample times the period (same time for every note) */
    int32_t rate = (t->decay * (36000 - sus)) >> 13;      /* sustain: ~4.4x (muted) .. 0.4x (ringing) */
    if (sus < 8000) rate += (8000 - sus) >> 3;             /* palm mute: T60 ~0.15 s */
    int32_t dec = (rate * ni) >> 4;                        /* Q16 */
    if (dec > 19000) dec = 19000;
    if (dec < 1) dec = 1;
    int32_t drive = t->drive0 + params[1];
    if (drive > 32767) drive = 32767;
    int32_t gain = 4096 + ((drive >> 1) * 7 >> 2);         /* Q12: 1x .. ~8x */
    int32_t makeup = 32768 - (drive >> 1) - (drive >> 2);  /* Q15: driven = quieter after the clip */
    const int32_t bf = t->body_f, ba = t->body_amt;
    int32_t w = v->w, yp = v->yprev, lp = v->lp, bl = v->bl, bb = v->bb, dc = v->dc, peak = 0;
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t r0 = v->buf[(w - ni) & AG_MASK], r1 = v->buf[(w - ni - 1) & AG_MASK];
        int32_t y = r0 + (((r1 - r0) * fr) >> 8);
        int32_t avg = (y + yp) >> 1;
        yp = y;
        int32_t f = avg + (((y - avg) * bright) >> 15);
        lp += ((f - lp) * lpa) >> 15;
        v->buf[w] = op1_sat16(lp - ((lp * dec) >> 16));
        w = (w + 1) & AG_MASK;
        /* body: SVF band-pass at the type's resonance */
        bl += (bf * bb) >> 15;
        int32_t hi = y - bl - ((bb * 2600) >> 12);
        if (hi > 65535) hi = 65535;
        if (hi < -65535) hi = -65535;
        bb += (bf * hi) >> 15;
        if (bb > 80000) bb = 80000;
        if (bb < -80000) bb = -80000;
        if (bl > 80000) bl = 80000;
        if (bl < -80000) bl = -80000;
        int32_t o = y + ((bb * ba) >> 15);
        dc += (o * 256 - dc) >> 9;                         /* DC blocker (~14 Hz) */
        o -= dc >> 8;
        if (o > 65000) o = 65000;
        if (o < -65000) o = -65000;
        o = (o * gain) >> 12;                              /* drive + soft clip */
        if (o > 32767) o = 32767;
        if (o < -32767) o = -32767;
        o -= ((((o * o) >> 15) * o) >> 15) * 10923 >> 15;
        o = (o * makeup) >> 15;
        out[n] = op1_sat16(o);
        int32_t a = ag_abs(o);
        if (a > peak) peak = a;
    }
    v->w = w; v->yprev = yp; v->lp = lp; v->bl = bl; v->bb = bb; v->dc = dc;
    v->level = peak > v->level ? peak : v->level - (v->level >> 4);   /* screen: fast up, slow down */
}

const op1_engine_t ENGINE_airguitar = { ag_note_on, ag_render, ag_note_off,
    { OP1_PRESS_NONE, OP1_PRESS_PAGE, OP1_PRESS_NONE, OP1_PRESS_PAGE } };

/* ======================= screen (GUI thread) =======================
 * airguitar.svg: the neck as the player sees it (nut, 12 frets, inlays). Drawn here every frame:
 *   - the body of the chosen guitar (soundhole and rosette, archtop f-holes, pickups, banjo head) and its bridge;
 *   - six strings, the 6th (lowest) on top and the 1st at the bottom: a held note takes a string by its pitch among
 *     the held notes, the lowest on the lowest string; a string vibrates (standing wave) and lights up with its level;
 *   - the note name of each held string at the nut;
 *   - the pick sweeping down or up across the strings on each stroke;
 *   - a meter at the right edge: the stroke in progress against the FORCE threshold (green tick);
 *   - top: version and guitar (blue); bottom: drive / tone / sustain, or FORCE and the play mode on page 2. */
#include "seq/seq_text.h"
#pragma GCC push_options
#pragma GCC optimize("Os")

#define AG_X0 22                                           /* nut */
#define AG_X1 292                                          /* saddle */
static const int16_t AG_ROW[6] = { 42, 57, 72, 87, 102, 117 };          /* 6th .. 1st string */
static const int16_t AG_W[6] = { 0xA00, 0x880, 0x700, 0x600, 0x500, 0x400 };
static const uint8_t C_BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     C_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF }, C_DGREY[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
/* phase increments at the upper quarter-tone edge of C4 .. B4 (note names at the nut) */
static const uint32_t AG_EDGE[12] = { 26226743, 27786267, 29438524, 31189030, 33043626, 35008502, 37090216,
                                      39295715, 41632360, 44107949, 46730744, 49509499 };
static const char AG_NOTE[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static uint32_t sc_id, sc_pick_t = 99;
static int8_t   sc_pick_dir = 1;
static uint32_t sc_ph[6];

static inline int32_t ssin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
#define L sq_line
#define Q OP1_Q10
static void sc_mix(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t t)   /* t 0..256 */
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((b[i] - a[i]) * t) >> 8));
    o[3] = 0xFF;
}
static void sc_circle(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t w, const uint8_t *c)   /* Q10 */
{
    op1_pt p = { cx + rad, cy };
    for (int a = 256; a <= 8192; a += 256) {               /* 32 segments */
        op1_pt q = { cx + (rad * ssin(a + 2048) >> 15), cy + (rad * ssin(a) >> 15) };
        op1_raster_line(r, &p, &q, w, c);
        p = q;
    }
}
static void sc_rect(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    L(r, x0, y0, x1, y0, w, c); L(r, x1, y0, x1, y1, w, c); L(r, x1, y1, x0, y1, w, c); L(r, x0, y1, x0, y0, w, c);
}

/* note name of a phase increment ("E2"), into s */
static void sc_note(char *s, uint32_t inc)
{
    int oct = 4;
    while (inc >= AG_EDGE[11] && oct < 10) { inc >>= 1; oct++; }
    while (inc < 24754749u && oct > -1) { inc <<= 1; oct--; }
    int m = 0;
    while (m < 11 && inc >= AG_EDGE[m]) m++;
    char *p = sq_cat(s, AG_NOTE[m]);
    if (oct >= 0) sq_num(p, (uint32_t)oct, 1);
}

static void sc_body(void *r, int type)
{
    switch (type) {
    case 4: {                                              /* archtop: f-holes above and below the strings */
        for (int k = 0; k < 2; k++) {
            int32_t y = k ? Q(123) : Q(31), d = k ? Q(-1) : Q(1);
            L(r, Q(194), y + 3 * d, Q(202), y - 3 * d, 0x500, C_GREY);
            L(r, Q(202), y - 3 * d, Q(220), y - 3 * d, 0x500, C_GREY);
            L(r, Q(220), y - 3 * d, Q(228), y + 3 * d, 0x500, C_GREY);
            L(r, Q(198), y - d, Q(198), y + d, 0x500, C_GREY);
            L(r, Q(224), y - d, Q(224), y + d, 0x500, C_GREY);
        }
        sc_rect(r, Q(256), Q(34), Q(262), Q(125), 0x500, C_GREY);          /* floating bridge */
        L(r, Q(298), Q(50), Q(298), Q(109), 0x600, C_GREY);                 /* tailpiece */
        return;
    }
    case 5: case 6:                                        /* electric: two pickups with pole pieces */
        for (int k = 0; k < 2; k++) {
            int32_t x = k ? Q(250) : Q(200);
            sc_rect(r, x, Q(34), x + Q(16), Q(125), 0x600, type == 6 ? C_RED : C_GREY);
            for (int s = 0; s < 6; s++) L(r, x + Q(6), Q(AG_ROW[s]), x + Q(10), Q(AG_ROW[s]), 0xA00, C_DGREY);
        }
        sc_rect(r, Q(282), Q(34), Q(296), Q(125), 0x500, C_GREY);          /* bridge */
        return;
    case 7:                                                /* banjo: the head */
        sc_circle(r, Q(220), Q(80), Q(48), 0x600, C_GREY);
        sc_circle(r, Q(220), Q(80), Q(44), 0x300, C_DGREY);
        sc_rect(r, Q(262), Q(36), Q(266), Q(123), 0x500, C_GREY);          /* bridge */
        return;
    default: {                                             /* flat tops: soundhole and rosette */
        int32_t rad = type == 3 ? Q(33) : Q(29);
        sc_circle(r, Q(214), Q(80), rad, 0x600, C_GREY);
        sc_circle(r, Q(214), Q(80), rad + Q(4), 0x300, C_DGREY);
        sc_circle(r, Q(214), Q(80), rad + Q(7), 0x300, type <= 1 ? C_GREY : C_DGREY);
        sc_rect(r, Q(278), Q(32), Q(298), Q(127), 0x500, C_GREY);          /* bridge */
        return;
    }
    }
}

static void ag_screen_init(void *scr) { (void)scr; }

static void ag_screen_update(void *scr, int dt)
{
    int16_t k[4];
    int slot = OP1_SCREEN_SLOT & 7;
    op1_synth_knobs_live(slot, k);
    const int16_t *wd = op1_synth_knobs(slot);
    int type = ag_type(k);
    const ag_type_t *t = &ag_types[type];
    op1_screen_draw(scr, dt);                              /* the neck (SVG) */
    void *r = op1_raster();
    sc_body(r, type);

    /* strings: held voices by pitch, the lowest on the lowest string used */
    int voice_of[6], n = 0, order[OP1_VOICES];
    for (int i = 0; i < 6; i++) voice_of[i] = -1;
    for (int i = 0; i < OP1_VOICES; i++) if (gv[i].held) order[n++] = i;
    for (int a = 1; a < n; a++)                            /* sort by pitch (insertion) */
        for (int b = a; b > 0 && gv[order[b]].inc < gv[order[b - 1]].inc; b--) { int x = order[b]; order[b] = order[b - 1]; order[b - 1] = x; }
    for (int i = 0; i < n; i++) ((volatile int *)voice_of)[6 - n + i] = order[i];   /* the bottom n strings, lowest
                                                           note on top (volatile: gcc would make it a memcpy) */
    for (int s = 0; s < 6; s++) {
        int vi = voice_of[s];
        int32_t lvl = vi >= 0 ? gv[vi].level : 0, y = Q(AG_ROW[s]);
        int32_t amp = (lvl * 3) >> 2;                      /* Q10 px: 0.75 px per 1000, up to 6 px */
        if (amp > Q(6)) amp = Q(6);
        uint8_t c[4];
        sc_mix(c, vi >= 0 ? C_GREY : C_DGREY, C_WHITE, vi >= 0 ? 40 + (lvl >> 7) : 0);
        sc_ph[s] += 1900 + (uint32_t)s * 411;
        if (amp < 0x80) L(r, Q(AG_X0), y, Q(AG_X1), y, AG_W[s], c);
        else {
            int32_t c1 = ssin((int32_t)sc_ph[s] + 2048), c2 = ssin((int32_t)(sc_ph[s] * 2) + 3000);
            int32_t px = Q(AG_X0), py = y;
            for (int j = 1; j <= 24; j++) {                /* standing wave: modes 1 and 2 */
                int32_t u = j * 4096 / 24;                 /* 0..pi over the string */
                int32_t d = (ssin(u) * c1 >> 15) + ((ssin(u * 2) * c2 >> 15) * 11 >> 5);
                int32_t x = Q(AG_X0) + (Q(AG_X1 - AG_X0) * j) / 24, yy = y + ((amp * d) >> 15);
                L(r, px, py, x, yy, AG_W[s], c);
                px = x; py = yy;
            }
        }
        if (vi >= 0) {                                     /* note name at the nut */
            char nm[6];
            sc_note(nm, gv[vi].inc);
            sq_text(r, Q(1), y - Q(3), Q(1), nm, 0x300, lvl > 1500 ? C_WHITE : C_GREY);
        }
    }

    /* the pick: a sweep across the strings on each stroke */
    if (gs.id != sc_id) { sc_id = gs.id; sc_pick_t = 0; sc_pick_dir = gs.dir; }
    {
        int32_t y;
        if (sc_pick_t < 8) {
            int32_t f = (int32_t)sc_pick_t * Q(AG_ROW[5] - AG_ROW[0] + 12) / 7;
            y = sc_pick_dir > 0 ? Q(AG_ROW[0] - 6) + f : Q(AG_ROW[5] + 6) - f;
            sc_pick_t++;
        } else y = sc_pick_dir > 0 ? Q(AG_ROW[5] + 9) : Q(AG_ROW[0] - 9);   /* rests past the strings */
        int32_t x = Q(t->pick_x), up = sc_pick_dir > 0 ? Q(-1) : Q(1);   /* the tip points along the stroke */
        const uint8_t *c = sc_pick_t < 8 ? C_WHITE : C_GREY;
        L(r, x - Q(5), y + 4 * up, x + Q(5), y + 4 * up, 0x600, c);
        L(r, x + Q(5), y + 4 * up, x, y - 5 * up, 0x600, c);
        L(r, x, y - 5 * up, x - Q(5), y + 4 * up, 0x600, c);
    }

    /* stroke meter at the right edge: 0 at the bottom, the threshold at the middle (green tick) */
    {
        int32_t thr = ag_threshold(wd), m = ag_abs(gs.proj);
        int32_t h = (int32_t)(((int64_t)m * Q(37)) / thr);
        if (h > Q(75)) h = Q(75);
        L(r, Q(312), Q(117), Q(312), Q(42), 0x200, C_DGREY);
        if (h > 0x100) L(r, Q(312), Q(117), Q(312), Q(117) - h, 0x500, h >= Q(37) ? C_WHITE : C_GREY);
        L(r, Q(308), Q(80), Q(316), Q(80), 0x400, C_GREEN);
    }

    /* text: version and guitar on top, the knobs at the bottom (y <= 140) */
    sq_text(r, Q(4), Q(7), Q(1), AG_VERSION, 0x300, C_GREY);
    sq_text(r, Q(36), Q(5), Q(1.5), t->name, 0x500, C_BLUE);
    {
        char s[20], *p;
        if (op1_eng_page[1]) { p = sq_cat(s, "FORCE "); sq_num(p, (uint32_t)(wd[OP1_ENG_PAGE_WORD(1)] * 100 >> 15), 1); }
        else { p = sq_cat(s, "DRIVE "); sq_num(p, (uint32_t)(k[1] * 100 >> 15), 1); }
        sq_text(r, Q(22), Q(131), Q(1), s, 0x400, C_GREEN);
        p = sq_cat(s, "TONE "); sq_num(p, (uint32_t)(k[2] * 100 >> 15), 1);
        sq_text(r, Q(118), Q(131), Q(1), s, 0x400, C_WHITE);
        if (op1_eng_page[3]) sq_text(r, Q(204), Q(131), Q(1), AG_KEYS_MODE(wd) ? "GESTURE+KEYS" : "GESTURE", 0x400, C_RED);
        else {
            p = sq_cat(s, k[3] < 8000 ? "MUTE " : "SUSTAIN "); sq_num(p, (uint32_t)(k[3] * 100 >> 15), 1);
            sq_text(r, Q(204), Q(131), Q(1), s, 0x400, C_RED);
            if (AG_KEYS_MODE(wd)) sq_text(r, Q(276), Q(7), Q(1), "+KEYS", 0x300, C_RED);
        }
    }
}
#undef L
#undef Q
#pragma GCC pop_options

const op1_screen_t SCREEN_airguitar = { ag_screen_init, ag_screen_update };
