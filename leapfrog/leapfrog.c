/* "leapfrog": a random note generator for op1r (synth and drum mode). Every parameter is a range drawn as a log on
 * a river; at each note a frog on every log jumps to the value drawn for that note.
 *
 * Rows (top to bottom; encoder, Shift = the lighter colour):
 *   RATE  blue          time to the next note: 1/64 .. 16 bars (each note draws a value inside the range)
 *   GATE  Shift blue    note length: 1/64 .. 16 bars
 *   NOTE  green         pitch range C1 .. C7, notes taken from the scale
 *   JUMP  Shift green   leap between one note and the next, in scale steps 0 .. 12 (up or down at random,
 *                       reflected at the edges of the NOTE range): small = walking melodies, large = jumpy
 *   VEL   white         velocity 1 .. 127
 *   AIR   Shift white   chance of a rest, 0 .. 100 % (a single value: the log grows from the left)
 *   MEM   red           memory, 0 .. 100 %: the chance that a note repeats the one the phrase had at this place
 *                       (100 % = a loop, 0 % = always new); press: phrase length 4 8 16 32 notes
 *   POLY  Shift red     notes per hit 1 .. 4 (chords: stacked thirds of the scale)
 * Encoders: turn moves the range (its centre, the "target"); press toggles width editing for that row, where the turn
 * makes the range longer / shorter around its centre. Shift + turn / press works on the Shift row of that encoder.
 * Keys on the leapfrog screen:
 *   note key        root of the scale (the key also sounds; drum mode: the scale picks the drum pads)
 *   drop            next scale (MAJ MIN DOR PHR PEN+ PEN-); Shift + drop: previous
 *   lift            start / stop
 *   scissors        new phrase: forget the memory
 * Any screen while leapfrog is the mode's sequencer: Shift + F G A B pattern 1..4 (all rows + scale), Shift + C#
 * start / stop (with REC held: also starts the tape).
 * Saved (bg_saver, DB table leapfrogseq, one row per mode): patterns, current pattern. Comes back stopped.
 *
 * Clock: 96 ticks per beat (Q16) at the OP-1 tempo, advanced by the player tick. Random numbers: xorshift32 per mode.
 * Platform independent (host test: test/leapfrog_test.c); firmware glue at the end.
 * Frog: Font Awesome "frog" icon (CC BY 4.0, fontawesome.com), rasterised by tools/gen_assets.py.
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "seq/seq_text.h"
#include "frog_runs.h"

#define LF_VERSION "V1.0"
#define LF_ROWS    8
#define LF_PATS    4
#define LF_PPQN    96
#define LF_MAXEV   32                                    /* phrase memory */
#define LF_MAXON   6                                     /* notes sounding at once (= voices) */
enum { R_RATE, R_GATE, R_NOTE, R_JUMP, R_VEL, R_AIR, R_MEM, R_POLY };
#define LF_NLEN    11
static const uint16_t lf_len[LF_NLEN] = { 6, 12, 24, 48, 96, 192, 384, 768, 1536, 3072, 6144 };   /* ticks: 1/64 .. 16 bars */
static const char *const lf_len_names[LF_NLEN] = { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8", "16" };
typedef struct { uint8_t min, max, step, single; uint8_t dlo, dhi; } lf_spec_t;
static const lf_spec_t lf_spec[LF_ROWS] = {
    { 0, LF_NLEN - 1, 1, 0,  2,  3 },                    /* RATE 1/16 .. 1/8 */
    { 0, LF_NLEN - 1, 1, 0,  1,  3 },                    /* GATE 1/32 .. 1/8 */
    { 24, 96, 1, 0, 48, 72 },                            /* NOTE C3 .. C5 */
    { 0, 12, 1, 0, 0, 3 },                               /* JUMP 0 .. 3 scale steps */
    { 1, 127, 4, 0, 70, 110 },                           /* VEL */
    { 0, 100, 5, 1, 0, 20 },                             /* AIR 20 % */
    { 0, 100, 5, 1, 0, 75 },                             /* MEM 75 % */
    { 1, 4, 1, 0, 1, 1 } };                              /* POLY 1 */
#define LF_NMODES  6
static const uint8_t lf_mode_len[LF_NMODES] = { 7, 7, 7, 7, 5, 5 };
static const uint8_t lf_modes[LF_NMODES][7] = { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 },
                                                { 0, 1, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 } };
static const char *const lf_mode_names[LF_NMODES] = { "MAJ", "MIN", "DOR", "PHR", "PEN+", "PEN-" };
static const uint8_t lf_plens[4] = { 4, 8, 16, 32 };

typedef struct { uint8_t lo[LF_ROWS], hi[LF_ROWS]; uint8_t root, mode, plen; } lf_pat_t;   /* saved */
typedef struct {                                          /* one generated note (event) */
    uint8_t rate, gate, vel, n, rest, air_r, mem_r, valid;
    int16_t deg;                                           /* scale degree (absolute: replays follow the scale) */
    int8_t  jump;                                          /* signed scale steps from the previous note */
} lf_ev_t;
typedef struct {
    lf_pat_t pats[LF_PATS];
    uint8_t  init, pat, running;
    uint8_t  width[LF_ROWS];                               /* width editing on (per row) */
    int32_t  acc[4];
    uint64_t tick_q16, next_q16;
    uint32_t rng;
    lf_ev_t  mem[LF_MAXEV];
    uint8_t  mpos;
    int16_t  last_deg;                                     /* -1: none yet */
    struct { uint8_t id; uint64_t off; } on[LF_MAXON];     /* sounding notes: id + 1 (0 = free), note-off tick */
    lf_ev_t  shown;                                        /* the last note, for the screen */
    uint8_t  shown_mem;                                    /* 1: it came from the memory */
    volatile uint32_t count;                               /* notes generated (screen: a new one -> frogs jump) */
} lf_bank_t;

static lf_bank_t lf[2];                                   /* 0 synth, 1 drum */

#ifdef OP1_HOST
int host_lf_changed;
void host_lf_note_on(int pos, int note, int key, int vel);
void host_lf_note_off(int pos, int note);
#define LF_CHANGED()             (host_lf_changed++)
#define LF_NOTE_ON(pos, n, k, v) (host_lf_note_on((pos), (n), (k), (v)), (n))
#define LF_NOTE_OFF(pos, n)      host_lf_note_off((pos), (n))
#define LF_DRUM_OCT()            0
#else
static void lf_changed(void);
#define LF_CHANGED()             lf_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);
void eu_fw_note_off(int midi);
#define LF_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define LF_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#define LF_DRUM_OCT()            (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
#endif

static int lf_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static uint32_t lf_rand(lf_bank_t *b) { uint32_t x = b->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return b->rng = x; }
static int lf_pick(lf_bank_t *b, int lo, int hi) { return hi <= lo ? lo : lo + (int)(lf_rand(b) % (uint32_t)(hi - lo + 1)); }

static void lf_pat_default(lf_pat_t *p)
{
    for (int r = 0; r < LF_ROWS; r++) { p->lo[r] = lf_spec[r].dlo; p->hi[r] = lf_spec[r].dhi; }
    p->root = 9; p->mode = 1; p->plen = 2;                 /* A minor, phrase of 16 */
}
static void lf_bank_init(lf_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->pat = 0; b->running = 0; b->last_deg = -1; b->mpos = 0;
    b->rng = 0x2545F491u ^ (uint32_t)(uintptr_t)b;
    if (!b->rng) b->rng = 1;
    for (int p = 0; p < LF_PATS; p++) lf_pat_default(&b->pats[p]);
}
static lf_pat_t *lf_cur(lf_bank_t *b) { return &b->pats[b->pat]; }

/* ---- scale ---- */
static int lf_midi(const lf_pat_t *p, int deg)             /* degree 0 = the root in octave -1 (MIDI root) */
{
    int n = lf_mode_len[p->mode];
    return p->root + 12 * (deg / n) + lf_modes[p->mode][deg % n];
}
static void lf_deg_range(const lf_pat_t *p, int *dlo, int *dhi)    /* degrees inside the NOTE range */
{
    int lo = p->lo[R_NOTE], hi = p->hi[R_NOTE], a = -1, z = -1;
    for (int d = 0; d < 80; d++) {
        int m = lf_midi(p, d);
        if (m >= lo && a < 0) a = d;
        if (m <= hi) z = d;
    }
    if (a < 0 || z < a) { a = z = a < 0 ? 0 : a; }        /* range narrower than a scale step: one note */
    *dlo = a; *dhi = z;
}

/* ---- notes ---- */
static void lf_off_all(int pos)
{
    lf_bank_t *b = &lf[pos & 1];
    for (int i = 0; i < LF_MAXON; i++) if (b->on[i].id) { LF_NOTE_OFF(pos & 1, b->on[i].id - 1); b->on[i].id = 0; }
}
static void lf_note(int pos, lf_bank_t *b, int midi, int vel, uint64_t off)
{
    int key = 0, note = midi;
    if (pos) {                                             /* drum mode: the scale note picks a pad (key 0..23) */
        key = ((midi - 0x35) % 24 + 24) % 24;
        note = 0x35 + key + LF_DRUM_OCT() * 12;
    }
    if (note < 0 || note > 127) return;
    for (int i = 0; i < LF_MAXON; i++)                     /* the same note again: restart it */
        if (b->on[i].id && b->on[i].id - 1 == note) { LF_NOTE_OFF(pos, note); b->on[i].id = 0; }
    int slot = -1;
    for (int i = 0; i < LF_MAXON; i++) if (!b->on[i].id) { slot = i; break; }
    if (slot < 0) {                                        /* all busy: the one ending first goes */
        slot = 0;
        for (int i = 1; i < LF_MAXON; i++) if (b->on[i].off < b->on[slot].off) slot = i;
        LF_NOTE_OFF(pos, b->on[slot].id - 1); b->on[slot].id = 0;
    }
    int id = LF_NOTE_ON(pos, note, key, vel);
    if (id >= 0) { b->on[slot].id = (uint8_t)(id + 1); b->on[slot].off = off; }
}

/* draw a new note from the ranges */
static lf_ev_t lf_new(lf_bank_t *b)
{
    const lf_pat_t *p = lf_cur(b);
    lf_ev_t e = { 0 };
    e.valid = 1;
    e.rate = (uint8_t)lf_pick(b, p->lo[R_RATE], p->hi[R_RATE]);
    e.gate = (uint8_t)lf_pick(b, p->lo[R_GATE], p->hi[R_GATE]);
    e.vel  = (uint8_t)lf_pick(b, p->lo[R_VEL], p->hi[R_VEL]);
    e.n    = (uint8_t)lf_pick(b, p->lo[R_POLY], p->hi[R_POLY]);
    e.air_r = (uint8_t)(lf_rand(b) % 100);
    e.rest = e.air_r < p->hi[R_AIR];
    int dlo, dhi; lf_deg_range(p, &dlo, &dhi);
    int d;
    if (b->last_deg < 0) { d = lf_pick(b, dlo, dhi); e.jump = 0; }
    else {
        int j = lf_pick(b, p->lo[R_JUMP], p->hi[R_JUMP]);
        int s = j && (lf_rand(b) & 1) ? -1 : 1;
        d = b->last_deg + s * j;
        for (int k = 0; k < 8 && (d < dlo || d > dhi); k++) {      /* reflect at the edges */
            if (d > dhi) d = 2 * dhi - d;
            if (d < dlo) d = 2 * dlo - d;
        }
        d = lf_clamp(d, dlo, dhi);
        e.jump = (int8_t)lf_clamp(d - b->last_deg, -127, 127);
    }
    e.deg = (int16_t)d;
    return e;
}

/* one note event at tick t: from the memory or new; plays it; schedules the next */
static void lf_event(int pos, lf_bank_t *b, uint64_t t)
{
    const lf_pat_t *p = lf_cur(b);
    int len = lf_plens[p->plen];
    if (b->mpos >= len) b->mpos = 0;
    lf_ev_t *slot = &b->mem[b->mpos];
    uint8_t mem_r = (uint8_t)(lf_rand(b) % 100);
    lf_ev_t e;
    int from_mem = slot->valid && mem_r < p->hi[R_MEM];
    if (from_mem) e = *slot;
    else { e = lf_new(b); *slot = e; }
    e.mem_r = mem_r;
    b->mpos = (uint8_t)((b->mpos + 1) % len);
    b->last_deg = e.deg;
    if (!e.rest) {
        uint64_t off = t + ((uint64_t)lf_len[e.gate] << 16);
        for (int i = 0; i < e.n; i++) {                     /* chord: stacked thirds of the scale */
            int m = lf_midi(p, e.deg + 2 * i);
            if (m > 127) break;
            lf_note(pos, b, m, e.vel, off);
        }
    }
    b->shown = e; b->shown_mem = (uint8_t)from_mem; b->count++;
    b->next_q16 = t + ((uint64_t)lf_len[e.rate] << 16);
}

static void lf_advance(int pos, uint64_t inc)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    uint64_t t = b->tick_q16;
    b->tick_q16 = t + inc;
    for (int i = 0; i < LF_MAXON; i++)
        if (b->on[i].id && t >= b->on[i].off) { LF_NOTE_OFF(pos & 1, b->on[i].id - 1); b->on[i].id = 0; }
    for (int k = 0; b->running && t >= b->next_q16 && k < 8; k++) lf_event(pos & 1, b, b->next_q16);
}
/* host: advance by n samples at bpm_x100 (tempo * 100) */
void lf_tick(int pos, int n, uint32_t bpm_x100)
{
    lf_advance(pos, ((uint64_t)bpm_x100 * LF_PPQN << 16) * (uint64_t)n / (6000ull * 44100ull));
}

/* ---- run ---- */
static void lf_start(lf_bank_t *b) { if (!b->running) { b->running = 1; b->next_q16 = b->tick_q16; } }
static void lf_stop(int pos, lf_bank_t *b) { if (b->running) { b->running = 0; lf_off_all(pos); } }
int lf_toggle(int pos)                                     /* returns 1 if it started */
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    if (b->running) { lf_stop(pos & 1, b); return 0; }
    lf_start(b); return 1;
}
void lf_all_off(int pos) { lf_off_all(pos & 1); }
void lf_forget(int pos)                                    /* scissors: a new phrase */
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    for (int i = 0; i < LF_MAXEV; i++) b->mem[i].valid = 0;
    b->mpos = 0;
}

/* ---- patterns ---- */
void lf_recall(int pos, int p)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    if (p < 0 || p >= LF_PATS || p == b->pat) return;
    b->pat = (uint8_t)p;
    LF_CHANGED();
}
/* Shift + key: F G A B (0 2 4 6) -> pattern 1..4, C# (20) -> start / stop. 0 = not handled, 2 = started, else 1 */
int lf_shift_key(int pos, int key)
{
    if (key == 0 || key == 2 || key == 4 || key == 6) { lf_recall(pos, key / 2); return 1; }
    if (key == 20) return lf_toggle(pos) ? 2 : 1;
    return 0;
}

/* ---- keys on the screen ---- */
void lf_root(int pos, int note)                            /* note key: root of the scale */
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    lf_cur(b)->root = (uint8_t)(((note % 12) + 12) % 12);
    LF_CHANGED();
}
void lf_scale_step(int pos, int dir)                       /* drop / Shift + drop */
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    lf_pat_t *p = lf_cur(b);
    p->mode = (uint8_t)((p->mode + dir + LF_NMODES) % LF_NMODES);
    LF_CHANGED();
}

/* ---- encoders: knob k, Shift -> row 2k + shift ---- */
void lf_press(int pos, int knob, int shift)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int r = 2 * knob + (shift != 0);
    if (r == R_MEM) { lf_pat_t *p = lf_cur(b); p->plen = (uint8_t)((p->plen + 1) % 4); LF_CHANGED(); return; }
    if (!lf_spec[r].single) b->width[r] ^= 1;
}
void lf_turn(int pos, int knob, int delta, int shift)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    if (knob < 0 || knob > 3) return;
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    if (!st) return;
    b->acc[knob] -= st * 1024;
    int r = 2 * knob + (shift != 0);
    const lf_spec_t *s = &lf_spec[r];
    lf_pat_t *p = lf_cur(b);
    int lo = p->lo[r], hi = p->hi[r], d = st * s->step;
    if (s->single) hi = lf_clamp(hi + d, s->min, s->max);
    else if (b->width[r]) {                                /* width around the centre */
        int c2 = lo + hi, w = lf_clamp(hi - lo + d, 0, s->max - s->min);
        lo = (c2 - w + (d > 0 ? 0 : 1)) / 2; hi = lo + w;
        if (lo < s->min) { hi += s->min - lo; lo = s->min; }
        if (hi > s->max) { lo -= hi - s->max; hi = s->max; }
    } else {                                               /* move the whole range */
        if (lo + d < s->min) d = s->min - lo;
        if (hi + d > s->max) d = s->max - hi;
        lo += d; hi += d;
    }
    p->lo[r] = (uint8_t)lo; p->hi[r] = (uint8_t)hi;
    LF_CHANGED();
}

/* MIDI START: the next note on the next clock tick (the master's downbeat) */
void lf_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        lf_bank_t *b = &lf[pos];
        b->tick_q16 = 0;
        if (b->running) { lf_off_all(pos); b->next_q16 = 0; }
    }
}

/* ---- persistence: "pat, (lo x8, hi x8, root, mode, plen) x 4" ---- */
#define LF_NUMS (1 + LF_PATS * (2 * LF_ROWS + 3))
int lf_serialize(int pos, char *out, int n)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    int v[LF_NUMS], k = 0;
    v[k++] = b->pat;
    for (int p = 0; p < LF_PATS; p++) {
        const lf_pat_t *q = &b->pats[p];
        for (int r = 0; r < LF_ROWS; r++) v[k++] = q->lo[r];
        for (int r = 0; r < LF_ROWS; r++) v[k++] = q->hi[r];
        v[k++] = q->root; v[k++] = q->mode; v[k++] = q->plen;
    }
    return sq_join(out, n, v, k);
}
int lf_parse(int pos, const char *s, int len)
{
    int v[LF_NUMS + 1];
    if (sq_split(s, len, v, LF_NUMS + 1) != LF_NUMS) return 0;
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    b->pat = (uint8_t)lf_clamp(v[0], 0, LF_PATS - 1);
    int k = 1;
    for (int p = 0; p < LF_PATS; p++) {
        lf_pat_t *q = &b->pats[p];
        for (int r = 0; r < LF_ROWS; r++) q->lo[r] = (uint8_t)lf_clamp(v[k + r], lf_spec[r].min, lf_spec[r].max);
        for (int r = 0; r < LF_ROWS; r++) q->hi[r] = (uint8_t)lf_clamp(v[k + LF_ROWS + r], q->lo[r], lf_spec[r].max);
        k += 2 * LF_ROWS;
        q->root = (uint8_t)(v[k] % 12); q->mode = (uint8_t)(v[k + 1] % LF_NMODES); q->plen = (uint8_t)(v[k + 2] % 4);
        k += 3;
    }
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Left: 8 rivers (water: leapfrog.svg, groups w0..w7, trembling a little with the beat while running), a log per row
 * spanning its range (single-value rows: from the left edge to the value), in the row's colour (Shift rows lighter),
 * with arrows at the ends while its width is being edited; a frog on every log jumps (an arc over a few frames, facing
 * the way it goes) to the value of the latest note. AIR / MEM: the frog stands on the log when the rest / the memory
 * happened, else it swims (head only) where it landed. Right: scale, pattern, run, version; per row name + values.
 * Everything stays above y = 140 (the OP-1 hides the bottom rows). */
#define LF_ROW0  13
#define LF_ROWH  15
#define LF_X0    10
#define LF_X1    184
#define LF_TX    200
static const uint8_t lf_col[LF_ROWS][4] = {
    { 0x4D, 0x9E, 0xFF, 0xFF }, { 0xA8, 0xCE, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF }, { 0x8C, 0xF6, 0xCA, 0xFF },
    { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xB8, 0xB8, 0xC4, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF }, { 0xFF, 0x9D, 0xAE, 0xFF } };
static const char *const lf_row_names[LF_ROWS] = { "RATE", "GATE", "NOTE", "JUMP", "VEL", "AIR", "MEM", "POLY" };
static const uint8_t lf_frog_col[4] = { 0xFF, 0xD2, 0x3F, 0xFF };
static const uint8_t lf_black[4] = { 0, 0, 0, 0xFF };
static const uint8_t lf_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t lf_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

static void *lf_water[LF_ROWS];
static int32_t lf_fx[LF_ROWS], lf_from[LF_ROWS], lf_to[LF_ROWS];   /* frog x now / hop from / to (Q10) */
static int8_t  lf_face[LF_ROWS];                           /* 1 right, -1 left */
static uint8_t lf_hop[LF_ROWS], lf_frog_ok;                /* hop frame (0 = resting) */
static uint32_t lf_seen;
#define LF_HOP_FRAMES 8

static inline int32_t lf_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static int32_t lf_x(int r, int v)                          /* value of row r -> x (Q10) */
{
    const lf_spec_t *s = &lf_spec[r];
    return OP1_Q10(LF_X0) + (int32_t)((int64_t)OP1_Q10(LF_X1 - LF_X0) * (v - s->min) / (s->max - s->min));
}
static void lf_dim(uint8_t *o, const uint8_t *c, int l)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
/* the frog: bottom centre (x, y) Q10, facing f; head_only: just the rows above the waterline (swimming) */
static void lf_draw_frog(void *r, int32_t x, int32_t y, int f, int head_only, const uint8_t *c)
{
    for (int i = 0; i < LF_FROG_RUNS; i++) {
        int32_t yy = y + lf_frog[i][2];
        if (head_only && lf_frog[i][2] > -OP1_Q10(5)) continue;
        int32_t a = lf_frog[i][0], b = lf_frog[i][1];
        if (f < 0) { int32_t t = -b; b = -a; a = t; }
        sq_line(r, x + a, yy, x + b, yy, 0x400, c);
    }
}
/* a log from xa to xb (Q10) centred on y: black inside (hides the water), outline, cut ring, bark */
static void lf_log(void *r, int32_t xa, int32_t xb, int32_t y, const uint8_t *c, int width_edit)
{
    int32_t h = OP1_Q10(2.5);
    sq_line(r, xa + OP1_Q10(1), y, xb - OP1_Q10(1), y, 2 * h, lf_black);   /* one wide black stroke */
    sq_line(r, xa + OP1_Q10(2), y - h, xb - OP1_Q10(2), y - h, 0x480, c);
    sq_line(r, xa + OP1_Q10(2), y + h, xb - OP1_Q10(2), y + h, 0x480, c);
    sq_line(r, xa + OP1_Q10(2), y - h, xa, y, 0x480, c); sq_line(r, xa, y, xa + OP1_Q10(2), y + h, 0x480, c);
    sq_line(r, xb - OP1_Q10(2), y - h, xb, y, 0x480, c); sq_line(r, xb, y, xb - OP1_Q10(2), y + h, 0x480, c);
    sq_dot(r, xb - OP1_Q10(2.2), y, 0x500, c);                          /* the cut end */
    uint8_t d[4]; lf_dim(d, c, 150);
    int32_t gap = (xb - xa) / 7 > OP1_Q10(9) ? (xb - xa) / 7 : OP1_Q10(9);                /* bark: at most 6 marks */
    for (int32_t x = xa + OP1_Q10(5); x + OP1_Q10(6) < xb - OP1_Q10(3); x += gap)
        sq_line(r, x, y, x + OP1_Q10(3), y, 0x300, d);
    if (width_edit) {                                      /* arrows: the ends move */
        sq_line(r, xa - OP1_Q10(5), y, xa - OP1_Q10(2), y - OP1_Q10(2.5), 0x400, c);   /* <  > pointing out */
        sq_line(r, xa - OP1_Q10(5), y, xa - OP1_Q10(2), y + OP1_Q10(2.5), 0x400, c);
        sq_line(r, xb + OP1_Q10(5), y, xb + OP1_Q10(2), y - OP1_Q10(2.5), 0x400, c);
        sq_line(r, xb + OP1_Q10(5), y, xb + OP1_Q10(2), y + OP1_Q10(2.5), 0x400, c);
    }
}
static char *lf_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    p = sq_cat(p, nm[n % 12]);
    return sq_num(p, (uint32_t)(n / 12 - 1 < 0 ? 0 : n / 12 - 1), 0);
}
static void lf_value_text(char *t, const lf_bank_t *b, int r)
{
    const lf_pat_t *p = &b->pats[b->pat];
    int lo = p->lo[r], hi = p->hi[r];
    char *o = t; *o = 0;
    switch (r) {
    case R_RATE: case R_GATE:
        o = sq_cat(o, lf_len_names[lo]); if (hi != lo) { o = sq_cat(o, "-"); o = sq_cat(o, lf_len_names[hi]); } break;
    case R_NOTE: o = lf_note_name(o, lo); if (hi != lo) { o = sq_cat(o, "-"); o = lf_note_name(o, hi); } break;
    case R_AIR:  o = sq_cat(sq_num(o, (uint32_t)hi, 0), "%"); break;
    case R_MEM:  o = sq_cat(sq_num(o, (uint32_t)hi, 0), "% L"); o = sq_num(o, lf_plens[p->plen], 0); break;
    default:     o = sq_num(o, (uint32_t)lo, 0); if (hi != lo) { o = sq_cat(o, "-"); o = sq_num(o, (uint32_t)hi, 0); }
    }
}

static void lf_screen_init(void *scr)
{
    static const char *ids[LF_ROWS] = { "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7" };
    for (int i = 0; i < LF_ROWS; i++) lf_water[i] = op1_find_node(scr, ids[i]);
}

static void lf_screen_update(void *scr, int dt, int pos)
{
    lf_bank_t *b = &lf[pos & 1];
    lf_bank_init(b);
    const lf_pat_t *p = &b->pats[b->pat];
    /* water: a small tremble with the beat while running */
    int32_t beat = b->running ? (int32_t)((b->tick_q16 >> 16) % LF_PPQN) * 8192 / LF_PPQN : 0;
    for (int i = 0; i < LF_ROWS; i++) if (lf_water[i]) {
        int32_t dx = b->running ? (int32_t)(((int64_t)OP1_Q10(0.8) * lf_sin(beat + i * 900)) >> 15) : 0;
        int32_t dy = b->running ? (int32_t)(((int64_t)OP1_Q10(0.4) * lf_sin(beat * 2 + i * 1500 + 2048)) >> 15) : 0;
        int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, dx, dy, 1024 };
        op1_node_set_transform(lf_water[i], m);
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    /* frogs: targets from the latest note (or the log centres before any) */
    const lf_ev_t *e = &b->shown;
    int have = e->valid;
    int32_t tgt[LF_ROWS];
    int on_log[LF_ROWS];
    {
        int dlo, dhi; lf_deg_range(p, &dlo, &dhi);
        int vals[LF_ROWS] = { e->rate, e->gate, have ? lf_midi(p, e->deg) : 0, e->jump < 0 ? -e->jump : e->jump,
                              e->vel, e->air_r, e->mem_r, e->n };
        for (int i = 0; i < LF_ROWS; i++) {
            int v = have ? lf_clamp(vals[i], lf_spec[i].min, lf_spec[i].max) : (p->lo[i] + p->hi[i]) / 2;
            if (!have && lf_spec[i].single) v = p->hi[i] / 2;
            tgt[i] = lf_x(i, v);
            on_log[i] = !lf_spec[i].single || v < p->hi[i] || (!have && p->hi[i]);
            if (i == R_MEM && have) on_log[i] = b->shown_mem;    /* on the log = the note came from the memory */
        }
        (void)dlo; (void)dhi;
    }
    if (!lf_frog_ok || b->count != lf_seen) {             /* a new note: every frog jumps */
        for (int i = 0; i < LF_ROWS; i++) {
            if (!lf_frog_ok) { lf_fx[i] = tgt[i]; lf_face[i] = 1; lf_hop[i] = 0; continue; }
            lf_from[i] = lf_fx[i]; lf_to[i] = tgt[i]; lf_hop[i] = 1;
            if (lf_to[i] != lf_from[i]) lf_face[i] = (int8_t)(lf_to[i] > lf_from[i] ? 1 : -1);
            if (i == R_NOTE && e->jump) lf_face[i] = (int8_t)(e->jump > 0 ? 1 : -1);
        }
        lf_frog_ok = 1; lf_seen = b->count;
    }

    for (int i = 0; i < LF_ROWS; i++) {
        int32_t y = OP1_Q10(LF_ROW0 + i * LF_ROWH + 10);
        int32_t xa, xb;
        if (lf_spec[i].single) { xa = OP1_Q10(LF_X0) - OP1_Q10(3); xb = lf_x(i, p->hi[i]) + OP1_Q10(3); }
        else { xa = lf_x(i, p->lo[i]) - OP1_Q10(4); xb = lf_x(i, p->hi[i]) + OP1_Q10(4); }
        if (!lf_spec[i].single || p->hi[i]) lf_log(r, xa, xb, y, lf_col[i], b->width[i]);
        /* the frog: hop arc, then rest */
        int32_t fy = y - OP1_Q10(2.5);
        if (lf_hop[i]) {
            int k = lf_hop[i];
            lf_fx[i] = lf_from[i] + (int32_t)((int64_t)(lf_to[i] - lf_from[i]) * k / LF_HOP_FRAMES);
            fy -= OP1_Q10(5) * 4 * k * (LF_HOP_FRAMES - k) / (LF_HOP_FRAMES * LF_HOP_FRAMES);
            if (++lf_hop[i] > LF_HOP_FRAMES) { lf_hop[i] = 0; lf_fx[i] = lf_to[i]; }
        } else lf_fx[i] = tgt[i];
        int swim = !lf_hop[i] && !on_log[i];
        if (swim) fy = y + OP1_Q10(3.5);                   /* in the water: head only */
        lf_draw_frog(r, lf_fx[i], fy, lf_face[i], swim, lf_frog_col);
        /* right: name + values */
        int32_t ty = y - OP1_Q10(3);
        sq_text(r, OP1_Q10(LF_TX), ty, OP1_Q10(1), lf_row_names[i], 0x300, lf_col[i]);
        char t[24]; lf_value_text(t, b, i);
        sq_text(r, OP1_Q10(LF_TX + 27), ty, OP1_Q10(1), t, 0x300, lf_col[i]);
        if (b->width[i]) sq_line(r, OP1_Q10(LF_TX), ty + OP1_Q10(8), OP1_Q10(LF_TX + 23), ty + OP1_Q10(8), 0x300, lf_col[i]);
    }
    /* top right: scale, pattern, run, version */
    {
        char t[16], *o = t;
        static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        o = sq_cat(o, nm[p->root]); o = sq_cat(o, " "); sq_cat(o, lf_mode_names[p->mode]);
        sq_text(r, OP1_Q10(LF_TX), OP1_Q10(3), OP1_Q10(1), t, 0x300, lf_white);
        t[0] = 'P'; t[1] = (char)('1' + b->pat); t[2] = 0;
        sq_text(r, OP1_Q10(LF_TX + 50), OP1_Q10(3), OP1_Q10(1), t, 0x300, lf_white);
        if (b->running) lf_draw_frog(r, OP1_Q10(LF_TX + 76), OP1_Q10(10), 1, 0, lf_frog_col);   /* running: a frog */
        sq_text(r, OP1_Q10(LF_TX + 92), OP1_Q10(3), OP1_Q10(1), LF_VERSION, 0x280, lf_grey);
    }
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "leapfrog", type 3 + lf_active[mode], as euclid) ======
 * Player vtable as euclid's (ok_player layout): +0x14 key_down(key 0..23) -> 1 = consumed, +0x24 reset,
 * +0x2c tick (96 per beat), +0x3c stop. Screen in the finger screen slot 0x2D. */
#define LF_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define LF_SHIFT()     (*(volatile uint8_t *)0x01DD7F78)
#define LF_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
extern volatile uint8_t eu_rec_play;                    /* core: Shift + C# with REC held -> PLAY */
static int lf_nop(void *self) { (void)self; return 0; }
static int lf_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = LF_MODE();
    if (LF_SHIFT()) {
        int h = lf_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)            /* leapfrog screen shown: the key sets the root */
        lf_root(m, key + 0x35 + (m ? LF_DRUM_OCT() : LF_SYNTH_OCT()) * 12);
    return 0;                                           /* never consumed: the key sounds as usual */
}
static void lf_reset(void *self) { (void)self; lf_all_off(LF_MODE()); }
static void lf_ptick(void *self, int counter) { (void)self; (void)counter; lf_advance(LF_MODE(), 1u << 16); }   /* 1/96 beat */
static void lf_pstop(void *self) { (void)self; lf_all_off(0); lf_all_off(1); }
static const uint32_t lf_vt[16] = {
    0, 0,  0, (uint32_t)lf_nop,  0, (uint32_t)lf_key_down,  0, (uint32_t)lf_nop,
    0, (uint32_t)lf_reset,  0, (uint32_t)lf_ptick,  0, (uint32_t)lf_nop,  0, (uint32_t)lf_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } lf_player = { lf_vt, { 0 } };
volatile uint8_t lf_active[2];

static char lf_sql[600];                                  /* 77 numbers <= 4 chars + commas ~ 320 */
static sq_store_t lf_store = { .name = "leapfrogseq", .ser = lf_serialize, .parse = lf_parse,
                               .sql = lf_sql, .cap = sizeof lf_sql, .maxlen = 500 };
static void lf_changed(void) { sq_store_dirty(&lf_store); }
void *lf_player_get(void)
{
    sq_store_load(&lf_store);
    return &lf_player;
}
static void lf_scr_update(void *scr, int dt) { lf_screen_update(scr, dt, LF_MODE()); }
const op1_screen_t SCREEN_leapfrog = { lf_screen_init, lf_scr_update };

/* plugin interface (docs/44): lift start / stop, drop scale (Shift: back), scissors new phrase, presses, turns */
static int lf_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code == 0x0A) { if (pressed) lf_toggle(m); return 1; }
    if (code == 0x0B) { if (pressed) lf_scale_step(m, shift ? -1 : 1); return 1; }
    if (code == 0x0C) { if (pressed) lf_forget(m); return 1; }
    if (knob >= 0) { if (pressed) lf_press(m, knob, shift); return 1; }
    return 0;
}
static void lf_pturn(int m, int knob, int delta, int shift) { lf_turn(m, knob, delta, shift); }
const op1_seq_t SEQ_leapfrog = { lf_active, lf_player_get, lf_pkey, lf_pturn, lf_midi_start, 0 };
#endif
