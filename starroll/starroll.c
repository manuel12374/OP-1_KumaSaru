/* "star-roll": a random note generator for op1r (synth and drum mode). Eight parameters, each a range shown on an LED
 * gauge (4 on top, the Shift ones below), and in the middle a star field that scrolls from right to left: one star
 * per note played, as a piano roll in space.
 *
 * Parameters (encoder, Shift = the lighter colour):
 *   RATE  blue          time to the next note: 1/64 .. 16 bars (each note draws a value inside the range)
 *   GATE  Shift blue    note length: 1/64 .. 16 bars
 *   NOTE  green         pitch range C1 .. C7, notes taken from the scale
 *   JUMP  Shift green   leap from one note to the next, in scale steps 0 .. 12 (up or down at random, reflected at the
 *                       edges of the NOTE range): small = walking melodies, large = jumpy
 *   VEL   white         velocity 1 .. 127
 *   AIR   Shift white   chance of a rest, 0 .. 100 % (each note draws its chance inside the range)
 *   MEM   red           memory, 0 .. 100 %: the chance that a note repeats the one the phrase had at this place
 *                       (100 % = a loop, 0 % = always new); each note draws its chance inside the range
 *   POLY  Shift red     notes per hit 1 .. 4 (chords: stacked thirds of the scale)
 * Encoders: turn moves the range (its centre, the "target"); press toggles width editing for that parameter, where the
 * turn makes the range longer / shorter around its centre. Shift + turn / press works on the Shift parameter.
 * Keys on the star-roll screen:
 *   note key        root of the scale (the key also sounds; drum mode: the scale picks the drum pads)
 *   drop            next scale (MAJ MIN DOR PHR PEN+ PEN-); Shift + drop: previous
 *   lift            start / stop; Shift + lift: phrase length of the memory 4 8 16 32 notes
 *   scissors        new phrase: forget the memory
 * Any screen while star-roll is the mode's sequencer: Shift + F G A B pattern 1..4 (all parameters + scale), Shift + C#
 * start / stop (with REC held: also starts the tape).
 * Saved (bg_saver, DB table starrollseq, one row per mode): patterns, current pattern. Comes back stopped.
 *
 * Clock: 96 ticks per beat (Q16) at the OP-1 tempo, advanced by the player tick. Random numbers: xorshift32 per mode.
 * Platform independent (host test: test/starroll_test.c); firmware glue at the end.
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "seq/seq_text.h"

#define SR_VERSION "V1.1"
#define SR_STARS   64                                    /* star field: the last notes played */
#define SR_ROWS    8
#define SR_PATS    4
#define SR_PPQN    96
#define SR_MAXEV   32                                    /* phrase memory */
#define SR_MAXON   6                                     /* notes sounding at once (= voices) */
enum { R_RATE, R_GATE, R_NOTE, R_JUMP, R_VEL, R_AIR, R_MEM, R_POLY };
#define SR_NLEN    11
static const uint16_t sr_len[SR_NLEN] = { 6, 12, 24, 48, 96, 192, 384, 768, 1536, 3072, 6144 };   /* ticks: 1/64 .. 16 bars */
static const char *const sr_len_names[SR_NLEN] = { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8", "16" };
typedef struct { uint8_t min, max, step; uint8_t dlo, dhi; } sr_spec_t;
static const sr_spec_t sr_spec[SR_ROWS] = {
    { 0, SR_NLEN - 1, 1,  2,  3 },                    /* RATE 1/16 .. 1/8 */
    { 0, SR_NLEN - 1, 1,  1,  3 },                    /* GATE 1/32 .. 1/8 */
    { 24, 96, 1, 48, 72 },                            /* NOTE C3 .. C5 */
    { 0, 12, 1, 0, 3 },                               /* JUMP 0 .. 3 scale steps */
    { 1, 127, 4, 70, 110 },                           /* VEL */
    { 0, 100, 5, 10, 30 },                            /* AIR 10 .. 30 % */
    { 0, 100, 5, 60, 90 },                            /* MEM 60 .. 90 % */
    { 1, 4, 1, 1, 1 } };                              /* POLY 1 */
#define SR_NMODES  6
static const uint8_t sr_mode_len[SR_NMODES] = { 7, 7, 7, 7, 5, 5 };
static const uint8_t sr_modes[SR_NMODES][7] = { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 },
                                                { 0, 1, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 } };
static const char *const sr_mode_names[SR_NMODES] = { "MAJ", "MIN", "DOR", "PHR", "PEN+", "PEN-" };
static const uint8_t sr_plens[4] = { 4, 8, 16, 32 };

typedef struct { uint8_t lo[SR_ROWS], hi[SR_ROWS]; uint8_t root, mode, plen; } sr_pat_t;   /* saved */
typedef struct {                                          /* one generated note (event) */
    uint8_t rate, gate, vel, n, rest, air_r, mem_r, valid;   /* air_r / mem_r: the chances drawn for this note */
    int16_t deg;                                           /* scale degree (absolute: replays follow the scale) */
    int8_t  jump;                                          /* signed scale steps from the previous note */
} sr_ev_t;
typedef struct {                                          /* one star = one note played (screen) */
    uint32_t t;                                            /* note-on tick (96 per beat, integer) */
    int16_t  y;                                            /* Q10 offset 0..1024 in the field: the pitch inside the NOTE
                                                            * range at that moment (0 = bottom) */
    uint16_t gate;                                         /* length in ticks */
    uint8_t  vel, flags;                                   /* SR_S_MEM / ROOT / HEAD (first note of a hit) / RING */
} sr_star_t;
#define SR_S_MEM  1
#define SR_S_ROOT 2
#define SR_S_HEAD 4
#define SR_S_RING 8
typedef struct {
    sr_pat_t pats[SR_PATS];
    uint8_t  init, pat, running;
    uint8_t  width[SR_ROWS];                               /* width editing on (per row) */
    int32_t  acc[4];
    uint64_t tick_q16, next_q16;
    uint32_t rng;
    sr_ev_t  mem[SR_MAXEV];
    uint8_t  mpos;
    int16_t  last_deg;                                     /* -1: none yet */
    struct { uint8_t id; uint64_t off; } on[SR_MAXON];     /* sounding notes: id + 1 (0 = free), note-off tick */
    sr_ev_t  shown;                                        /* the last note, for the screen */
    uint8_t  shown_mem;                                    /* 1: it came from the memory */
    volatile uint32_t count;                               /* notes generated */
    sr_star_t stars[SR_STARS];
    volatile uint8_t  star_n;                              /* next slot (ring) */
    volatile uint8_t  star_used;                           /* slots filled (up to SR_STARS) */
} sr_bank_t;

static sr_bank_t sr[2];                                   /* 0 synth, 1 drum */

#ifdef OP1_HOST
int host_sr_changed;
void host_sr_note_on(int pos, int note, int key, int vel);
void host_sr_note_off(int pos, int note);
#define SR_CHANGED()             (host_sr_changed++)
#define SR_NOTE_ON(pos, n, k, v) (host_sr_note_on((pos), (n), (k), (v)), (n))
#define SR_NOTE_OFF(pos, n)      host_sr_note_off((pos), (n))
#define SR_DRUM_OCT()            0
#else
static void sr_changed(void);
#define SR_CHANGED()             sr_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);
void eu_fw_note_off(int midi);
#define SR_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define SR_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#define SR_DRUM_OCT()            (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
#endif

static int sr_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static uint32_t sr_rand(sr_bank_t *b) { uint32_t x = b->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return b->rng = x; }
static int sr_pick(sr_bank_t *b, int lo, int hi) { return hi <= lo ? lo : lo + (int)(sr_rand(b) % (uint32_t)(hi - lo + 1)); }

static void sr_pat_default(sr_pat_t *p)
{
    for (int r = 0; r < SR_ROWS; r++) { p->lo[r] = sr_spec[r].dlo; p->hi[r] = sr_spec[r].dhi; }
    p->root = 9; p->mode = 1; p->plen = 2;                 /* A minor, phrase of 16 */
}
static void sr_bank_init(sr_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->pat = 0; b->running = 0; b->last_deg = -1; b->mpos = 0;
    b->rng = 0x2545F491u ^ (uint32_t)(uintptr_t)b;
    if (!b->rng) b->rng = 1;
    for (int p = 0; p < SR_PATS; p++) sr_pat_default(&b->pats[p]);
}
static sr_pat_t *sr_cur(sr_bank_t *b) { return &b->pats[b->pat]; }

/* ---- scale ---- */
static int sr_midi(const sr_pat_t *p, int deg)             /* degree 0 = the root in octave -1 (MIDI root) */
{
    int n = sr_mode_len[p->mode];
    return p->root + 12 * (deg / n) + sr_modes[p->mode][deg % n];
}
static void sr_deg_range(const sr_pat_t *p, int *dlo, int *dhi)    /* degrees inside the NOTE range */
{
    int lo = p->lo[R_NOTE], hi = p->hi[R_NOTE], a = -1, z = -1;
    for (int d = 0; d < 80; d++) {
        int m = sr_midi(p, d);
        if (m >= lo && a < 0) a = d;
        if (m <= hi) z = d;
    }
    if (a < 0 || z < a) { a = z = a < 0 ? 0 : a; }        /* range narrower than a scale step: one note */
    *dlo = a; *dhi = z;
}

/* ---- notes ---- */
static void sr_off_all(int pos)
{
    sr_bank_t *b = &sr[pos & 1];
    for (int i = 0; i < SR_MAXON; i++) if (b->on[i].id) { SR_NOTE_OFF(pos & 1, b->on[i].id - 1); b->on[i].id = 0; }
}
static void sr_note(int pos, sr_bank_t *b, int midi, int vel, uint64_t off)
{
    int key = 0, note = midi;
    if (pos) {                                             /* drum mode: the scale note picks a pad (key 0..23) */
        key = ((midi - 0x35) % 24 + 24) % 24;
        note = 0x35 + key + SR_DRUM_OCT() * 12;
    }
    if (note < 0 || note > 127) return;
    for (int i = 0; i < SR_MAXON; i++)                     /* the same note again: restart it */
        if (b->on[i].id && b->on[i].id - 1 == note) { SR_NOTE_OFF(pos, note); b->on[i].id = 0; }
    int slot = -1;
    for (int i = 0; i < SR_MAXON; i++) if (!b->on[i].id) { slot = i; break; }
    if (slot < 0) {                                        /* all busy: the one ending first goes */
        slot = 0;
        for (int i = 1; i < SR_MAXON; i++) if (b->on[i].off < b->on[slot].off) slot = i;
        SR_NOTE_OFF(pos, b->on[slot].id - 1); b->on[slot].id = 0;
    }
    int id = SR_NOTE_ON(pos, note, key, vel);
    if (id >= 0) { b->on[slot].id = (uint8_t)(id + 1); b->on[slot].off = off; }
}

/* draw a new note from the ranges */
static sr_ev_t sr_new(sr_bank_t *b)
{
    const sr_pat_t *p = sr_cur(b);
    sr_ev_t e = { 0 };
    e.valid = 1;
    e.rate = (uint8_t)sr_pick(b, p->lo[R_RATE], p->hi[R_RATE]);
    e.gate = (uint8_t)sr_pick(b, p->lo[R_GATE], p->hi[R_GATE]);
    e.vel  = (uint8_t)sr_pick(b, p->lo[R_VEL], p->hi[R_VEL]);
    e.n    = (uint8_t)sr_pick(b, p->lo[R_POLY], p->hi[R_POLY]);
    e.air_r = (uint8_t)sr_pick(b, p->lo[R_AIR], p->hi[R_AIR]);          /* this note's chance of a rest */
    e.rest = (int)(sr_rand(b) % 100) < e.air_r;
    int dlo, dhi; sr_deg_range(p, &dlo, &dhi);
    int d;
    if (b->last_deg < 0) { d = sr_pick(b, dlo, dhi); e.jump = 0; }
    else {
        int j = sr_pick(b, p->lo[R_JUMP], p->hi[R_JUMP]);
        int s = j && (sr_rand(b) & 1) ? -1 : 1;
        d = b->last_deg + s * j;
        for (int k = 0; k < 8 && (d < dlo || d > dhi); k++) {      /* reflect at the edges */
            if (d > dhi) d = 2 * dhi - d;
            if (d < dlo) d = 2 * dlo - d;
        }
        d = sr_clamp(d, dlo, dhi);
        e.jump = (int8_t)sr_clamp(d - b->last_deg, -127, 127);
    }
    e.deg = (int16_t)d;
    return e;
}

/* one note event at tick t: from the memory or new; plays it; schedules the next */
static void sr_event(int pos, sr_bank_t *b, uint64_t t)
{
    const sr_pat_t *p = sr_cur(b);
    int len = sr_plens[p->plen];
    if (b->mpos >= len) b->mpos = 0;
    sr_ev_t *slot = &b->mem[b->mpos];
    uint8_t mem_r = (uint8_t)sr_pick(b, p->lo[R_MEM], p->hi[R_MEM]);    /* this note's chance of the memory */
    sr_ev_t e;
    int from_mem = slot->valid && (int)(sr_rand(b) % 100) < mem_r;
    if (from_mem) e = *slot;
    else { e = sr_new(b); *slot = e; }
    e.mem_r = mem_r;
    b->mpos = (uint8_t)((b->mpos + 1) % len);
    b->last_deg = e.deg;
    if (!e.rest) {
        uint64_t off = t + ((uint64_t)sr_len[e.gate] << 16);
        int lo = p->lo[R_NOTE], hi = p->hi[R_NOTE];
        for (int i = 0; i < e.n; i++) {                     /* chord: stacked thirds of the scale */
            int m = sr_midi(p, e.deg + 2 * i);
            if (m > 127) break;
            sr_note(pos, b, m, e.vel, off);
            sr_star_t *st = &b->stars[b->star_n];           /* the star: position fixed now, in the range of now */
            int y = hi > lo ? (m - lo) * 1024 / (hi - lo) : 512;
            st->t = (uint32_t)(t >> 16); st->y = (int16_t)sr_clamp(y, -256, 1280);
            st->gate = sr_len[e.gate]; st->vel = e.vel;
            st->flags = (uint8_t)((from_mem ? SR_S_MEM : 0) | ((m - p->root) % 12 == 0 ? SR_S_ROOT : 0) | (i == 0 ? SR_S_HEAD : 0)
                      | (sr_len[e.gate] >= 384 && e.vel >= 96 ? SR_S_RING : 0));
            b->star_n = (uint8_t)((b->star_n + 1) % SR_STARS);
            if (b->star_used < SR_STARS) b->star_used++;
        }
    }
    b->shown = e; b->shown_mem = (uint8_t)from_mem; b->count++;
    b->next_q16 = t + ((uint64_t)sr_len[e.rate] << 16);
}

static void sr_advance(int pos, uint64_t inc)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    uint64_t t = b->tick_q16;
    b->tick_q16 = t + inc;
    for (int i = 0; i < SR_MAXON; i++)
        if (b->on[i].id && t >= b->on[i].off) { SR_NOTE_OFF(pos & 1, b->on[i].id - 1); b->on[i].id = 0; }
    for (int k = 0; b->running && t >= b->next_q16 && k < 8; k++) sr_event(pos & 1, b, b->next_q16);
}
/* host: advance by n samples at bpm_x100 (tempo * 100) */
void sr_tick(int pos, int n, uint32_t bpm_x100)
{
    sr_advance(pos, ((uint64_t)bpm_x100 * SR_PPQN << 16) * (uint64_t)n / (6000ull * 44100ull));
}

/* ---- run ---- */
static void sr_start(sr_bank_t *b) { if (!b->running) { b->running = 1; b->next_q16 = b->tick_q16; } }
static void sr_stop(int pos, sr_bank_t *b) { if (b->running) { b->running = 0; sr_off_all(pos); } }
int sr_toggle(int pos)                                     /* returns 1 if it started */
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    if (b->running) { sr_stop(pos & 1, b); return 0; }
    sr_start(b); return 1;
}
void sr_all_off(int pos) { sr_off_all(pos & 1); }
void sr_phrase_len(int pos)                                /* Shift + lift: memory phrase 4 8 16 32 notes */
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    sr_pat_t *p = sr_cur(b);
    p->plen = (uint8_t)((p->plen + 1) % 4);
    SR_CHANGED();
}
void sr_forget(int pos)                                    /* scissors: a new phrase */
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    for (int i = 0; i < SR_MAXEV; i++) b->mem[i].valid = 0;
    b->mpos = 0;
}

/* ---- patterns ---- */
void sr_recall(int pos, int p)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    if (p < 0 || p >= SR_PATS || p == b->pat) return;
    b->pat = (uint8_t)p;
    SR_CHANGED();
}
/* Shift + key: F G A B (0 2 4 6) -> pattern 1..4, C# (20) -> start / stop. 0 = not handled, 2 = started, else 1 */
int sr_shift_key(int pos, int key)
{
    if (key == 0 || key == 2 || key == 4 || key == 6) { sr_recall(pos, key / 2); return 1; }
    if (key == 20) return sr_toggle(pos) ? 2 : 1;
    return 0;
}

/* ---- keys on the screen ---- */
void sr_root(int pos, int note)                            /* note key: root of the scale */
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    sr_cur(b)->root = (uint8_t)(((note % 12) + 12) % 12);
    SR_CHANGED();
}
void sr_scale_step(int pos, int dir)                       /* drop / Shift + drop */
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    sr_pat_t *p = sr_cur(b);
    p->mode = (uint8_t)((p->mode + dir + SR_NMODES) % SR_NMODES);
    SR_CHANGED();
}

/* ---- encoders: knob k, Shift -> row 2k + shift ---- */
void sr_press(int pos, int knob, int shift)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int r = 2 * knob + (shift != 0);
    b->width[r] ^= 1;
}
void sr_turn(int pos, int knob, int delta, int shift)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    if (knob < 0 || knob > 3) return;
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    if (!st) return;
    b->acc[knob] -= st * 1024;
    int r = 2 * knob + (shift != 0);
    const sr_spec_t *s = &sr_spec[r];
    sr_pat_t *p = sr_cur(b);
    int lo = p->lo[r], hi = p->hi[r], d = st * s->step;
    if (b->width[r]) {                                /* width around the centre */
        int c2 = lo + hi, w = sr_clamp(hi - lo + d, 0, s->max - s->min);
        lo = (c2 - w + (d > 0 ? 0 : 1)) / 2; hi = lo + w;
        if (lo < s->min) { hi += s->min - lo; lo = s->min; }
        if (hi > s->max) { lo -= hi - s->max; hi = s->max; }
    } else {                                               /* move the whole range */
        if (lo + d < s->min) d = s->min - lo;
        if (hi + d > s->max) d = s->max - hi;
        lo += d; hi += d;
    }
    p->lo[r] = (uint8_t)lo; p->hi[r] = (uint8_t)hi;
    SR_CHANGED();
}

/* MIDI START: the next note on the next clock tick (the master's downbeat) */
void sr_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        sr_bank_t *b = &sr[pos];
        b->tick_q16 = 0;
        b->star_used = 0; b->star_n = 0;                   /* the clock restarts: the old stars would come back */
        if (b->running) { sr_off_all(pos); b->next_q16 = 0; }
    }
}

/* ---- persistence: "pat, (lo x8, hi x8, root, mode, plen) x 4" ---- */
#define SR_NUMS (1 + SR_PATS * (2 * SR_ROWS + 3))
int sr_serialize(int pos, char *out, int n)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    int v[SR_NUMS], k = 0;
    v[k++] = b->pat;
    for (int p = 0; p < SR_PATS; p++) {
        const sr_pat_t *q = &b->pats[p];
        for (int r = 0; r < SR_ROWS; r++) v[k++] = q->lo[r];
        for (int r = 0; r < SR_ROWS; r++) v[k++] = q->hi[r];
        v[k++] = q->root; v[k++] = q->mode; v[k++] = q->plen;
    }
    return sq_join(out, n, v, k);
}
int sr_parse(int pos, const char *s, int len)
{
    int v[SR_NUMS + 1];
    if (sq_split(s, len, v, SR_NUMS + 1) != SR_NUMS) return 0;
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    b->pat = (uint8_t)sr_clamp(v[0], 0, SR_PATS - 1);
    int k = 1;
    for (int p = 0; p < SR_PATS; p++) {
        sr_pat_t *q = &b->pats[p];
        for (int r = 0; r < SR_ROWS; r++) q->lo[r] = (uint8_t)sr_clamp(v[k + r], sr_spec[r].min, sr_spec[r].max);
        for (int r = 0; r < SR_ROWS; r++) q->hi[r] = (uint8_t)sr_clamp(v[k + SR_ROWS + r], q->lo[r], sr_spec[r].max);
        k += 2 * SR_ROWS;
        q->root = (uint8_t)(v[k] % 12); q->mode = (uint8_t)(v[k + 1] % SR_NMODES); q->plen = (uint8_t)(v[k + 2] % 4);
        k += 3;
    }
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Top: LED gauges RATE NOTE VEL MEM; bottom: GATE JUMP AIR POLY (the Shift parameters, lighter colours). A gauge is an
 * arc of 11 segments over the parameter's whole scale: lit in the parameter's colour = the range, white = the value
 * drawn for the latest note (always inside the range shown; AIR / MEM: the chance drawn); name inside, values
 * below; width editing: the baseline in colour with arrows at its ends.
 * Middle: the star field, 2 bars wide, scrolling right to left at the tempo: a star per note played, born at the right
 * edge when it sounds. Height = pitch inside the NOTE range of that moment (it keeps it when the range changes later);
 * size = velocity; trail = the note's length (it grows out of the right edge while the note sounds); white = new,
 * red = from the memory, yellow = the root; faint lines join the notes in order (the leaps); long loud notes are planets
 * with a ring; rests leave the sky empty. Slow dust drifts behind. Corners: scale, pattern + beat, version, NOTE range.
 * Everything stays above y = 140 (the OP-1 hides the bottom rows). */
#define SR_FY0    44                                       /* star field top / bottom (px) */
#define SR_FY1    102
#define SR_WIN    768                                      /* ticks across the screen: 2 bars */
#define SR_GR     22                                       /* gauge radius */
#define SR_SEGS   11
static const uint8_t sr_col[SR_ROWS][4] = {
    { 0x4D, 0x9E, 0xFF, 0xFF }, { 0xA8, 0xCE, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF }, { 0x8C, 0xF6, 0xCA, 0xFF },
    { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xB8, 0xB8, 0xC4, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF }, { 0xFF, 0x9D, 0xAE, 0xFF } };
static const char *const sr_row_names[SR_ROWS] = { "RATE", "GATE", "NOTE", "JUMP", "VEL", "AIR", "MEM", "POLY" };
static const uint8_t sr_dark[4]  = { 0x3A, 0x3C, 0x3E, 0xFF };
static const uint8_t sr_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t sr_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t sr_red[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t sr_yel[4]   = { 0xFF, 0xD2, 0x3F, 0xFF };
static const uint8_t sr_line[4]  = { 0x40, 0x44, 0x4C, 0xFF };
static const uint8_t sr_dust[4]  = { 0x30, 0x32, 0x36, 0xFF };
static const uint8_t sr_dgreen[4] = { 0x00, 0x77, 0x4A, 0xFF };

static inline int32_t sr_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t sr_cos(int32_t a) { return OP1_SINE_TABLE[(a + 2048) & 8191]; }
static void sr_dim(uint8_t *o, const uint8_t *c, int l) { for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8); o[3] = 0xFF; }
static void sr_ctext(void *r, int32_t cx, int32_t y, int32_t sc, const char *t, int32_t w, const uint8_t *c)
{
    sq_text(r, cx - (sq_width(sc, t) - sc * 2) / 2, y, sc, t, w, c);    /* centred (the last advance has 2 units of gap) */
}
static char *sr_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    p = sq_cat(p, nm[n % 12]);
    return sq_num(p, (uint32_t)(n / 12 - 1 < 0 ? 0 : n / 12 - 1), 0);
}
static void sr_value_text(char *t, const sr_bank_t *b, int r)
{
    const sr_pat_t *p = &b->pats[b->pat];
    int lo = p->lo[r], hi = p->hi[r];
    char *o = t; *o = 0;
    switch (r) {
    case R_RATE: case R_GATE:
        o = sq_cat(o, sr_len_names[lo]); if (hi != lo) { o = sq_cat(o, "-"); o = sq_cat(o, sr_len_names[hi]); } break;
    case R_NOTE: o = sr_note_name(o, lo); if (hi != lo) { o = sq_cat(o, "-"); o = sr_note_name(o, hi); } break;
    case R_AIR:  o = sq_num(o, (uint32_t)lo, 0); if (hi != lo) { o = sq_cat(o, "-"); o = sq_num(o, (uint32_t)hi, 0); } sq_cat(o, "%"); break;
    case R_MEM:  o = sq_num(o, (uint32_t)lo, 0); if (hi != lo) { o = sq_cat(o, "-"); o = sq_num(o, (uint32_t)hi, 0); }
                 o = sq_cat(o, "% L"); o = sq_num(o, sr_plens[p->plen], 0); break;
    default:     o = sq_num(o, (uint32_t)lo, 0); if (hi != lo) { o = sq_cat(o, "-"); o = sq_num(o, (uint32_t)hi, 0); }
    }
}

/* one gauge: centre (cx, cy) px; lo / hi / last in 0..1024 of the parameter's scale (last < 0: none) */
static void sr_gauge(void *r, int cx, int cy, int row, int32_t lo, int32_t hi, int32_t last, int width_edit, const char *val)
{
    const uint8_t *c = sr_col[row];
    int32_t X = OP1_Q10(cx), Y = OP1_Q10(cy), rm = OP1_Q10(SR_GR) * 78 / 100, w = OP1_Q10(SR_GR) * 34 / 100;
    int li = last < 0 ? -1 : (last * SR_SEGS) >> 10;
    if (li >= SR_SEGS) li = SR_SEGS - 1;
    for (int k = 0; k < SR_SEGS; k++) {
        int32_t a0 = 6144 + k * 4096 / SR_SEGS + 60, a1 = 6144 + (k + 1) * 4096 / SR_SEGS - 60;   /* left .. right over the top */
        int32_t v = (2 * k + 1) * 512 / SR_SEGS;                                                     /* segment centre, 0..1024 */
        int on = v >= lo - 512 / SR_SEGS && v <= hi + 512 / SR_SEGS;
        const uint8_t *sc = k == li ? sr_white : on ? c : sr_dark;
        sq_line(r, X + (int32_t)(((int64_t)rm * sr_sin(a0)) >> 15), Y - (int32_t)(((int64_t)rm * sr_cos(a0)) >> 15),
                   X + (int32_t)(((int64_t)rm * sr_sin(a1)) >> 15), Y - (int32_t)(((int64_t)rm * sr_cos(a1)) >> 15), w, sc);
    }
    int32_t bl = X - OP1_Q10(SR_GR), br = X + OP1_Q10(SR_GR), by = Y + OP1_Q10(1);
    sq_line(r, bl, by, br, by, 0x300, width_edit ? c : sr_grey);
    if (width_edit) {                                       /* < > : the range grows / shrinks */
        sq_line(r, bl - OP1_Q10(4), by, bl - OP1_Q10(1), by - OP1_Q10(2.5), 0x300, c); sq_line(r, bl - OP1_Q10(4), by, bl - OP1_Q10(1), by + OP1_Q10(2.5), 0x300, c);
        sq_line(r, br + OP1_Q10(4), by, br + OP1_Q10(1), by - OP1_Q10(2.5), 0x300, c); sq_line(r, br + OP1_Q10(4), by, br + OP1_Q10(1), by + OP1_Q10(2.5), 0x300, c);
    }
    sr_ctext(r, X, Y - OP1_Q10(6.5), OP1_Q10(0.8), sr_row_names[row], 0x2C0, c);   /* inside the arc */
    sr_ctext(r, X, Y + OP1_Q10(3.5), OP1_Q10(0.9), val, 0x2C0, c);
}
static int32_t sr_norm(int r, int v) { return (int32_t)((v - sr_spec[r].min) * 1024 / (sr_spec[r].max - sr_spec[r].min)); }

static void sr_screen_init(void *scr) { (void)scr; }

static void sr_screen_update(void *scr, int dt, int pos)
{
    sr_bank_t *b = &sr[pos & 1];
    sr_bank_init(b);
    const sr_pat_t *p = &b->pats[b->pat];
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    char t[24];

    /* ---- gauges: top RATE NOTE VEL MEM, bottom GATE JUMP AIR POLY ---- */
    const sr_ev_t *e = &b->shown;
    int lastv[SR_ROWS] = { e->rate, e->gate, e->valid ? sr_midi(p, e->deg) : 0, e->jump < 0 ? -e->jump : e->jump,
                           e->vel, e->air_r, e->mem_r, e->n };
    for (int k = 0; k < SR_ROWS; k++) {
        int row = (k % 4) * 2 + k / 4;                     /* k 0..3 top (normal), 4..7 bottom (Shift) */
        const sr_spec_t *s = &sr_spec[row];
        int lo = p->lo[row], hi = p->hi[row];
        int32_t last = -1;
        if (e->valid) last = sr_norm(row, sr_clamp(lastv[row], lo, hi));      /* always inside the range shown */
        sr_value_text(t, b, row);
        sr_gauge(r, 40 + (k % 4) * 80, k < 4 ? 27 : 125, row, sr_norm(row, lo), sr_norm(row, hi), last,
                 b->width[row], t);
    }

    /* ---- star field ---- */
    int64_t now = (int64_t)b->tick_q16;                    /* Q16 ticks */
    int32_t fy0 = OP1_Q10(SR_FY0 + 4), fh = OP1_Q10(SR_FY1 - SR_FY0 - 8);
#define SR_X(tq) (OP1_Q10(320) - (int32_t)((now - (int64_t)(tq)) * 320 / SR_WIN >> 6))   /* Q16 tick -> Q10 px */
    /* dust: drifts at half speed */
    int32_t drift = (int32_t)((now * 160 / SR_WIN) >> 6);                                    /* Q10 px */
    for (int i = 0; i < 28; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        int32_t x = (int32_t)((h >> 8) % 320u) * 1024 - drift % OP1_Q10(320);
        if (x < 0) x += OP1_Q10(320);
        int32_t y = OP1_Q10(SR_FY0 + 2) + (int32_t)((h >> 20) % (uint32_t)(SR_FY1 - SR_FY0 - 4)) * 1024;
        sq_line(r, x, y, x + OP1_Q10(1), y, 0x300, sr_dust);
    }
    /* stars, oldest first: constellation lines between the first notes of the hits, then trail, star, ring */
    int n = b->star_used, first = (b->star_n - n + SR_STARS) % SR_STARS;
    int32_t px = 0, py = 0, have = 0;
    for (int j = 0; j < n; j++) {
        const sr_star_t *st = &b->stars[(first + j) % SR_STARS];
        int32_t x = SR_X((int64_t)st->t << 16);
        if (x < -OP1_Q10(40)) continue;                    /* gone off the left edge (its trail too) */
        int32_t y = fy0 + fh - (int32_t)((int64_t)fh * st->y >> 10);
        if (y < OP1_Q10(SR_FY0 + 1)) y = OP1_Q10(SR_FY0 + 1);
        if (y > OP1_Q10(SR_FY1 - 1)) y = OP1_Q10(SR_FY1 - 1);
        if (st->flags & SR_S_HEAD) {
            if (have && x >= 0) sq_line(r, px, py, x, y, 0x200, sr_line);
            px = x; py = y; have = 1;
        }
        const uint8_t *c = st->flags & SR_S_MEM ? sr_red : st->flags & SR_S_ROOT ? sr_yel : sr_white;
        int32_t size = OP1_Q10(1.2) + (int32_t)st->vel * OP1_Q10(3.6) / 127;
        int32_t xe = SR_X(((int64_t)st->t + st->gate) << 16);
        if (xe > OP1_Q10(320)) xe = OP1_Q10(320);
        uint8_t d[4]; sr_dim(d, c, 110);
        if (xe > x + OP1_Q10(1) && xe > 0) sq_line(r, x < 0 ? 0 : x, y, xe, y, size / 2, d);
        if (x < 0) continue;
        sq_dot(r, x, y, size, c);
        if (st->flags & SR_S_RING) {                       /* planet: an elliptic ring */
            int32_t rx = size * 17 / 10, ry = size * 6 / 10, ox = x + rx, oy = y;
            for (int a = 1024; a <= 8192; a += 1024) {
                int32_t qx = x + (int32_t)(((int64_t)rx * sr_cos(a)) >> 15), qy = y + (int32_t)(((int64_t)ry * sr_sin(a)) >> 15);
                sq_line(r, ox, oy, qx, qy, 0x200, c); ox = qx; oy = qy;
            }
        }
    }
#undef SR_X
    /* corners: scale, pattern + beat, version, NOTE range */
    {
        static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        char *o = sq_cat(t, nm[p->root]); o = sq_cat(o, " "); sq_cat(o, sr_mode_names[p->mode]);
        sq_text(r, OP1_Q10(3), OP1_Q10(SR_FY0 + 3), OP1_Q10(0.9), t, 0x2C0, sr_white);
        t[0] = 'P'; t[1] = (char)('1' + b->pat); t[2] = 0;
        sq_text(r, OP1_Q10(3), OP1_Q10(SR_FY1 - 8), OP1_Q10(0.9), t, 0x2C0, sr_white);
        if (b->running && (b->tick_q16 >> 16) % SR_PPQN < SR_PPQN / 4)                       /* beat */
            sq_dot(r, OP1_Q10(19), OP1_Q10(SR_FY1 - 5), 0x900, sr_red);
        sq_text(r, OP1_Q10(301), OP1_Q10(SR_FY0 + 3), OP1_Q10(0.7), SR_VERSION, 0x240, sr_grey);
        sr_note_name(t, p->hi[R_NOTE]);
        sq_text(r, OP1_Q10(301), OP1_Q10(SR_FY0 + 11), OP1_Q10(0.7), t, 0x240, sr_dgreen);
        sr_note_name(t, p->lo[R_NOTE]);
        sq_text(r, OP1_Q10(301), OP1_Q10(SR_FY1 - 7), OP1_Q10(0.7), t, 0x240, sr_dgreen);
    }
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "star-roll", type 3 + sr_active[mode], as euclid) ======
 * Player vtable as euclid's (ok_player layout): +0x14 key_down(key 0..23) -> 1 = consumed, +0x24 reset,
 * +0x2c tick (96 per beat), +0x3c stop. Screen in the finger screen slot 0x2D. */
#define SR_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define SR_SHIFT()     (*(volatile uint8_t *)0x01DD7F78)
#define SR_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
extern volatile uint8_t eu_rec_play;                    /* core: Shift + C# with REC held -> PLAY */
static int sr_nop(void *self) { (void)self; return 0; }
static int sr_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = SR_MODE();
    if (SR_SHIFT()) {
        int h = sr_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)            /* star-roll screen shown: the key sets the root */
        sr_root(m, key + 0x35 + (m ? SR_DRUM_OCT() : SR_SYNTH_OCT()) * 12);
    return 0;                                           /* never consumed: the key sounds as usual */
}
static void sr_reset(void *self) { (void)self; sr_all_off(SR_MODE()); }
static void sr_ptick(void *self, int counter) { (void)self; (void)counter; sr_advance(SR_MODE(), 1u << 16); }   /* 1/96 beat */
static void sr_pstop(void *self) { (void)self; sr_all_off(0); sr_all_off(1); }
static const uint32_t sr_vt[16] = {
    0, 0,  0, (uint32_t)sr_nop,  0, (uint32_t)sr_key_down,  0, (uint32_t)sr_nop,
    0, (uint32_t)sr_reset,  0, (uint32_t)sr_ptick,  0, (uint32_t)sr_nop,  0, (uint32_t)sr_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } sr_player = { sr_vt, { 0 } };
volatile uint8_t sr_active[2];

static char sr_sql[600];                                  /* 77 numbers <= 4 chars + commas ~ 320 */
static sq_store_t sr_store = { .name = "starrollseq", .ser = sr_serialize, .parse = sr_parse,
                               .sql = sr_sql, .cap = sizeof sr_sql, .maxlen = 500 };
static void sr_changed(void) { sq_store_dirty(&sr_store); }
void *sr_player_get(void)
{
    sq_store_load(&sr_store);
    return &sr_player;
}
static void sr_scr_update(void *scr, int dt) { sr_screen_update(scr, dt, SR_MODE()); }
const op1_screen_t SCREEN_starroll = { sr_screen_init, sr_scr_update };

/* plugin interface (docs/44): lift start / stop, drop scale (Shift: back), scissors new phrase, presses, turns */
static int sr_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code == 0x0A) { if (pressed) { if (shift) sr_phrase_len(m); else sr_toggle(m); } return 1; }
    if (code == 0x0B) { if (pressed) sr_scale_step(m, shift ? -1 : 1); return 1; }
    if (code == 0x0C) { if (pressed) sr_forget(m); return 1; }
    if (knob >= 0) { if (pressed) sr_press(m, knob, shift); return 1; }
    return 0;
}
static void sr_pturn(int m, int knob, int delta, int shift) { sr_turn(m, knob, delta, shift); }
const op1_seq_t SEQ_starroll = { sr_active, sr_player_get, sr_pkey, sr_pturn, sr_midi_start, 0 };
#endif
