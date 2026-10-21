/* "track racer": a circular sequencer for op1r (synth and drum mode), after Google's Racer experiment with Giorgio
 * Moroder. Six concentric lanes, a little car on each; a lane plays when its car drives over a stop sign.
 *
 * Lanes: 6 per mode (= the OP-1's 6 voices), lane 1 the outer one. Each lane has its own lap length (1 2 3 4 5 6 7 8
 * 12 16 beats), so the cars drive at different speeds and the lanes phase against each other (polyrhythms). A lap has
 * 64 positions; a stop sign sits on one of them and holds its own note (synth: a pitch; drum: a drum sound + pitch),
 * so a lane is usually one sound but can also play a line. One note at a time per lane (a new sign cuts the last).
 *
 * Controls (track racer screen shown; Shift = the shift key held):
 *   blue turn       select lane 1..6 (wraps)
 *   green turn      cursor along the selected lane, 1 position (Shift: 4 = 1/16 of the lap), wraps
 *   white turn      lap length of the selected lane; press: page 2 = gate (1/32 1/16 1/8 1/4 1/2 1/1 TIE)
 *   red turn        right = start, left = stop the selected lane's car; Shift: all cars; press: page 2 = level
 *   REC             record on / off
 *   note key        record on, lane stopped: puts a sign with that note at the cursor (replaces one there)
 *                   record on, lane running: live, a sign at the car's position (nearest of the 64) - the key
 *                   sounds as usual and the new sign does not play again in the same pass
 *                   record off: just plays (audition)
 *   scissors        delete the sign under the cursor; Shift + scissors: clear the selected lane
 *   lift / drop     copy / paste the selected lane (signs, lap, gate, level); Shift: the whole pattern
 * Any screen while track racer is the mode's sequencer (as euclid / dots):
 *   Shift + F G A B (keys 0 2 4 6)            pattern 1..4 (cars keep running, in phase)
 *   Shift + G A B C D E (14 16 18 19 21 23)   mute lane 1..6 (the car keeps driving), Shift + D# (22) all
 *   Shift + C# (20)                           stop all if any car runs, else start all at once; with REC held
 *                                             (tape armed) the start also presses PLAY
 * Saved (bg_saver, DB table trackracerseq; rows of the old name circuitseq are taken over once): rows 0..7 = mode * 4 + pattern (lanes), rows 8 / 9 = selected lane,
 * pattern, cursor per mode. Cars come back stopped.
 *
 * Clock as euclid: 24 ticks per beat (Q16) at the OP-1 tempo, advanced by the player tick (96 per beat). A single
 * car starts on the next beat; Shift + C# starts all at once. Changing a running lane's lap keeps the car where it
 * is on the circle (the phase is kept).
 *
 * Platform independent (host test: test/trackracer_test.c); firmware glue at the end.
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "seq/seq_text.h"

#define TR_VERSION "V1.2"                              /* shown top left on the screen */
#define TR_LANES   6
#define TR_POS     64                                   /* positions per lap */
#define TR_PATS    4
#define TR_PPQN    24
#define TR_EMPTY   0xFF                                 /* key of an empty position */
#define TR_TIE     6                                    /* gate index: hold until the next sign */
#define TR_NLAPS   10
#define TR_NGATES  7
static const uint8_t tr_laps[TR_NLAPS] = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 16 };    /* beats per lap */
static const uint8_t tr_gates[TR_NGATES] = { 3, 6, 12, 24, 48, 96, 0 };          /* ticks (24 per beat); TIE */
static const char *const tr_gate_names[TR_NGATES] = { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1", "TIE" };

typedef struct {                                        /* a lane's settings (saved, per pattern) */
    uint8_t note[TR_POS], key[TR_POS];                  /* sign: key 0..23, note = absolute pitch; key TR_EMPTY: none */
    uint8_t lap, gate, vel, muted;                      /* lap / gate: index into tr_laps / tr_gates */
} tr_lane_t;

typedef struct {                                        /* a car (live, not saved; global per lane like euclid) */
    uint8_t  running;
    int16_t  last;                                      /* last position driven over (-1: none yet) */
    int16_t  skip;                                      /* live record: do not play this position once (-1 none) */
    uint8_t  sounding;                                  /* note id on (+1), 0 none */
    int64_t  start;                                     /* tick (Q16) of position 0 of lap 0 (may lie before 0 after a
                                                         * re-phase: only (now - start) mod lap matters) */
    uint64_t off_at;                                    /* tick (Q16) of the note-off (TIE: never) */
    int16_t  hit;                                       /* last position played (screen flash) */
    uint32_t hit_tick;                                  /* tick (24 ppqn) of that hit */
} tr_car_t;

typedef struct {
    tr_lane_t pats[TR_PATS][TR_LANES];
    tr_car_t  car[TR_LANES];
    uint8_t   init, sel, pat, cur, rec;                 /* selected lane, pattern, cursor (0..63), record on */
    uint8_t   page[4];                                  /* encoder page per knob (white: gate, red: level) */
    int32_t   acc[4];                                   /* encoder accumulators (one step per 1024) */
    uint64_t  tick_q16;                                 /* clock, ticks (24 per beat) Q16 */
} tr_bank_t;

static tr_bank_t tr[2];                                 /* 0 synth, 1 drum */
static tr_lane_t tr_clip_lane[2], tr_clip_pat[2][TR_LANES];
static uint8_t   tr_clip_full[2][2];

#ifdef OP1_HOST
int host_tr_changed;
void host_tr_note_on(int pos, int note, int key, int vel);
void host_tr_note_off(int pos, int note);
#define TR_CHANGED()             (host_tr_changed++)
#define TR_NOTE_ON(pos, n, k, v) (host_tr_note_on((pos), (n), (k), (v)), (n))
#define TR_NOTE_OFF(pos, n)      host_tr_note_off((pos), (n))
#else
static void tr_changed(void);
#define TR_CHANGED()             tr_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);    /* core (seq/seq_core.c), shared by the sequencers */
void eu_fw_note_off(int midi);
#define TR_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define TR_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#endif

static int tr_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* struct copies: no libc in a plugin (GCC turns a struct assignment or a plain copy loop into a memcpy call) */
static void tr_copy_lane(tr_lane_t *d, const tr_lane_t *s)
{
    volatile uint8_t *o = (volatile uint8_t *)d;
    const uint8_t *i = (const uint8_t *)s;
    for (unsigned n = 0; n < sizeof *d; n++) o[n] = i[n];
}

static void tr_lane_default(tr_lane_t *l)
{
    for (int i = 0; i < TR_POS; i++) { l->key[i] = TR_EMPTY; l->note[i] = 0; }
    l->lap = 3; l->gate = 1; l->vel = 100; l->muted = 0;                 /* 1 bar, 1/16, level 100 */
}
static void tr_bank_init(tr_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->sel = 0; b->pat = 0; b->cur = 0; b->rec = 0;
    for (int p = 0; p < TR_PATS; p++) for (int k = 0; k < TR_LANES; k++) tr_lane_default(&b->pats[p][k]);
    for (int k = 0; k < TR_LANES; k++) { b->car[k].last = -1; b->car[k].skip = -1; b->car[k].hit = -1; }
}
static tr_lane_t *tr_lane(tr_bank_t *b, int k) { return &b->pats[b->pat][k]; }
static uint64_t tr_lap_q16(const tr_lane_t *l) { return (uint64_t)tr_laps[l->lap] * TR_PPQN << 16; }
static int tr_started(const tr_bank_t *b, const tr_car_t *c) { return c->running && (int64_t)b->tick_q16 >= c->start; }
static uint64_t tr_elapsed(const tr_bank_t *b, const tr_car_t *c) { return (uint64_t)((int64_t)b->tick_q16 - c->start); }
/* keep a running car where it is on its circle when its lap changes from lo to ln (Q16 ticks) */
static void tr_rephase(tr_bank_t *b, tr_car_t *c, uint64_t lo, uint64_t ln)
{
    if (!tr_started(b, c)) return;
    uint64_t f = (tr_elapsed(b, c) % lo) * 65536 / lo;                   /* phase 0..65535 */
    c->start = (int64_t)b->tick_q16 - (int64_t)(f * ln / 65536);
    c->last = (int16_t)(tr_elapsed(b, c) % ln * TR_POS / ln);
}

static void tr_silence(int pos, tr_car_t *c)
{
    if (c->sounding) { TR_NOTE_OFF(pos, c->sounding - 1); c->sounding = 0; }
}

/* ---- the clock: advance by inc (Q16 ticks, 24 per beat); cars play the signs they drive over ---- */
static void tr_play(int pos, tr_bank_t *b, int k, int i)
{
    tr_car_t *c = &b->car[k];
    const tr_lane_t *l = tr_lane(b, k);
    if (c->skip == i) { c->skip = -1; return; }          /* just recorded live (it already sounded) */
    if (l->key[i] == TR_EMPTY || l->muted) return;
    tr_silence(pos, c);
    int id = TR_NOTE_ON(pos, l->note[i], l->key[i], l->vel);
    if (id >= 0) c->sounding = (uint8_t)(id + 1);
    c->off_at = l->gate == TR_TIE ? ~0ull : b->tick_q16 + ((uint64_t)tr_gates[l->gate] << 16);
    c->hit = (int16_t)i; c->hit_tick = (uint32_t)(b->tick_q16 >> 16);
}
static void tr_advance(int pos, uint64_t inc)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    uint64_t t = b->tick_q16;                            /* this tick's own position (as euclid) */
    b->tick_q16 = t + inc;
    for (int k = 0; k < TR_LANES; k++) {
        tr_car_t *c = &b->car[k];
        if (c->sounding && t >= c->off_at) tr_silence(pos & 1, c);
        if (!c->running || (int64_t)t < c->start) continue;
        uint64_t lap = tr_lap_q16(tr_lane(b, k));
        int i = (int)(((uint64_t)((int64_t)t - c->start) % lap) * TR_POS / lap);
        if (c->last < 0) { tr_play(pos & 1, b, k, i); c->last = (int16_t)i; continue; }
        for (int n = 0; c->last != i && n < TR_POS; n++) {        /* every position passed since the last tick */
            c->last = (int16_t)((c->last + 1) % TR_POS);
            tr_play(pos & 1, b, k, c->last);
        }
    }
}

/* host: advance by n samples at bpm_x100 (tempo * 100) */
void tr_tick(int pos, int n, uint32_t bpm_x100)
{
    tr_advance(pos, ((uint64_t)bpm_x100 * TR_PPQN << 16) * (uint64_t)n / (6000ull * 44100ull));
}

/* ---- starting / stopping cars ---- */
static void tr_start(tr_bank_t *b, int k, int now)
{
    tr_car_t *c = &b->car[k];
    if (c->running) return;
    uint64_t t = b->tick_q16, beat = (uint64_t)TR_PPQN << 16;
    c->start = (int64_t)(now ? t : (t / beat + 1) * beat);   /* now, or the next beat */
    c->last = -1; c->skip = -1; c->running = 1;
}
static void tr_stop(int pos, tr_bank_t *b, int k)
{
    tr_car_t *c = &b->car[k];
    if (!c->running) return;
    c->running = 0; c->last = -1; tr_silence(pos, c);
}
static int tr_any_running(const tr_bank_t *b)
{
    for (int k = 0; k < TR_LANES; k++) if (b->car[k].running) return 1;
    return 0;
}
/* Shift + C#: stop all if any car runs, else start all at once. Returns 1 if it started. */
int tr_toggle_all(int pos)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    int any = tr_any_running(b);
    for (int k = 0; k < TR_LANES; k++) { if (any) tr_stop(pos & 1, b, k); else tr_start(b, k, 1); }
    return !any;
}
void tr_all_off(int pos)
{
    for (int k = 0; k < TR_LANES; k++) tr_silence(pos & 1, &tr[pos & 1].car[k]);
}

/* ---- patterns, mutes ---- */
void tr_recall(int pos, int p)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    if (p < 0 || p >= TR_PATS || p == b->pat) return;
    for (int k = 0; k < TR_LANES; k++) {                 /* keep each car where it is on its circle */
        tr_car_t *c = &b->car[k];
        tr_silence(pos & 1, c);
        tr_rephase(b, c, tr_lap_q16(&b->pats[b->pat][k]), tr_lap_q16(&b->pats[p][k]));
    }
    b->pat = (uint8_t)p;
    TR_CHANGED();
}
void tr_mute(int pos, int k)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    if (k < 0 || k >= TR_LANES) return;
    tr_lane_t *l = tr_lane(b, k);
    l->muted ^= 1;
    if (l->muted) tr_silence(pos & 1, &b->car[k]);
    TR_CHANGED();
}
void tr_mute_all(int pos)                               /* all muted -> unmute all, else mute all */
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    int all = 1;
    for (int k = 0; k < TR_LANES; k++) if (!tr_lane(b, k)->muted) all = 0;
    for (int k = 0; k < TR_LANES; k++) { tr_lane(b, k)->muted = (uint8_t)!all; if (!all) tr_silence(pos & 1, &b->car[k]); }
    TR_CHANGED();
}
/* Shift + key: F G A B -> pattern 1..4, G A B C D E -> mute lane 1..6, D# -> mute all, C# -> start / stop all.
 * Returns 0 if not handled, 2 if C# started the cars, else 1. */
int tr_shift_key(int pos, int key)
{
    static const int8_t act[24] = { 10, -1, 11, -1, 12, -1, 13, -1, -1, -1, -1, -1, -1, -1,
                                    0, -1, 1, -1, 2, 3, 8, 4, 9, 5 };
    if (key < 0 || key > 23 || act[key] < 0) return 0;
    if (act[key] >= 10) { tr_recall(pos, act[key] - 10); return 1; }
    if (act[key] == 8) return tr_toggle_all(pos) ? 2 : 1;
    if (act[key] == 9) tr_mute_all(pos); else tr_mute(pos, act[key]);
    return 1;
}

/* ---- signs ---- */
static void tr_put(tr_bank_t *b, int k, int i, int note, int key)
{
    tr_lane_t *l = tr_lane(b, k);
    l->note[i] = (uint8_t)tr_clamp(note, 0, 127); l->key[i] = (uint8_t)tr_clamp(key, 0, 23);
}
/* note key on the track racer screen with record on: a sign at the cursor (car stopped) or under the car (running).
 * Returns the position written, -1 if nothing was recorded. The key is never consumed: it sounds as usual. */
int tr_key(int pos, int note, int key)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    if (!b->rec) return -1;
    int k = b->sel, i = b->cur;
    tr_car_t *c = &b->car[k];
    if (tr_started(b, c)) {                              /* live: the nearest position to the car */
        uint64_t lap = tr_lap_q16(tr_lane(b, k)), t = tr_elapsed(b, c) % lap;
        i = (int)((t * TR_POS + lap / 2) / lap) % TR_POS;
        if (i != c->last) c->skip = (int16_t)i;           /* still ahead: do not play it again this pass */
        b->cur = (uint8_t)i;                              /* the cursor follows what was recorded */
    }
    tr_put(b, k, i, note, key);
    TR_CHANGED();
    return i;
}
void tr_rec_key(int pos) { tr_bank_t *b = &tr[pos & 1]; tr_bank_init(b); b->rec ^= 1; }

/* scissors: delete the sign at the cursor; Shift: clear the lane */
void tr_delete(int pos, int all)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    tr_lane_t *l = tr_lane(b, b->sel);
    if (all) { for (int i = 0; i < TR_POS; i++) l->key[i] = TR_EMPTY; tr_silence(pos & 1, &b->car[b->sel]); }
    else l->key[b->cur] = TR_EMPTY;
    TR_CHANGED();
}
/* lift / drop: copy / paste the selected lane (all = 1: the whole pattern). One clipboard of each per mode. */
void tr_clip(int pos, int paste, int all)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    int m = pos & 1, a = all != 0;
    if (!paste) {
        if (a) for (int k = 0; k < TR_LANES; k++) tr_copy_lane(&tr_clip_pat[m][k], tr_lane(b, k));
        else tr_copy_lane(&tr_clip_lane[m], tr_lane(b, b->sel));
        tr_clip_full[m][a] = 1;
        return;
    }
    if (!tr_clip_full[m][a]) return;
    for (int k = a ? 0 : b->sel; k < (a ? TR_LANES : b->sel + 1); k++) {
        tr_silence(m, &b->car[k]);
        uint64_t lo = tr_lap_q16(tr_lane(b, k));
        tr_copy_lane(tr_lane(b, k), a ? &tr_clip_pat[m][k] : &tr_clip_lane[m]);
        tr_rephase(b, &b->car[k], lo, tr_lap_q16(tr_lane(b, k)));       /* keep the car's place on the circle */
    }
    TR_CHANGED();
}

/* ---- encoders ---- */
static int tr_steps(tr_bank_t *b, int knob, int delta)
{
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;   /* direction change */
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    b->acc[knob] -= st * 1024;
    return st;
}
void tr_press(int pos, int knob)
{
    tr_bank_t *b = &tr[pos & 1];
    if (knob == 2 || knob == 3) b->page[knob] ^= 1;     /* white: lap / gate, red: run / level */
}
void tr_turn(int pos, int knob, int delta, int shift)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int st = tr_steps(b, knob, delta);
    if (!st) return;
    tr_lane_t *l = tr_lane(b, b->sel);
    tr_car_t *c = &b->car[b->sel];
    switch (knob) {
    case 0:                                              /* blue: lane (not saved alone: written with the next change) */
        b->sel = (uint8_t)(((b->sel + st) % TR_LANES + TR_LANES) % TR_LANES);
        return;
    case 1:                                              /* green: cursor */
        b->cur = (uint8_t)(((b->cur + st * (shift ? 4 : 1)) % TR_POS + TR_POS) % TR_POS);
        return;
    case 2:
        if (b->page[2]) { l->gate = (uint8_t)tr_clamp(l->gate + st, 0, TR_NGATES - 1); break; }
        {                                                /* lap: keep the car's place on the circle */
            uint64_t lo = tr_lap_q16(l);
            int nl = tr_clamp(l->lap + st, 0, TR_NLAPS - 1);
            if (nl == l->lap) return;
            l->lap = (uint8_t)nl;
            tr_rephase(b, c, lo, tr_lap_q16(l));
        }
        break;
    case 3:
        if (b->page[3]) { l->vel = (uint8_t)tr_clamp(l->vel + st * 4, 1, 127); break; }
        for (int k = shift ? 0 : b->sel; k < (shift ? TR_LANES : b->sel + 1); k++) {   /* run (not saved) */
            if (st > 0) tr_start(b, k, 0); else tr_stop(pos & 1, b, k);
        }
        return;
    }
    TR_CHANGED();
}

/* MIDI START: running cars restart from the start line on the next clock tick (the master's downbeat) */
void tr_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        tr_bank_t *b = &tr[pos];
        b->tick_q16 = 0;
        for (int k = 0; k < TR_LANES; k++) {
            tr_car_t *c = &b->car[k];
            if (!c->running) continue;
            tr_silence(pos, c);
            c->start = 0; c->last = -1; c->skip = -1;
        }
    }
}

/* ---- persistence text (hex, no libc) ----
 * lanes row (mode, pattern): "c1" then per lane: lap gate vel muted count (2 hex each), then count signs as
 * position note key (2 hex each). Longest: 2 + 6 * (10 + 64 * 6) = 2366 chars.
 * state row (mode): "s1" sel pat cur (2 hex each). */
#define TR_TEXT_LEN (2 + TR_LANES * (10 + TR_POS * 6))
static const char tr_hexd[] = "0123456789ABCDEF";
static char *tr_hex(char *p, int v) { *p++ = tr_hexd[(v >> 4) & 15]; *p++ = tr_hexd[v & 15]; return p; }
static int tr_unhex(const char *s)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}
int tr_serialize(int pos, int p, char *out)              /* out: TR_TEXT_LEN + 1 */
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    char *o = out;
    *o++ = 'c'; *o++ = '1';
    for (int k = 0; k < TR_LANES; k++) {
        const tr_lane_t *l = &b->pats[p][k];
        int n = 0;
        for (int i = 0; i < TR_POS; i++) n += l->key[i] != TR_EMPTY;
        o = tr_hex(tr_hex(tr_hex(tr_hex(tr_hex(o, l->lap), l->gate), l->vel), l->muted), n);
        for (int i = 0; i < TR_POS; i++) if (l->key[i] != TR_EMPTY) o = tr_hex(tr_hex(tr_hex(o, i), l->note[i]), l->key[i]);
    }
    *o = 0;
    return (int)(o - out);
}
int tr_parse(int pos, int p, const char *s, int len)
{
    if (p < 0 || p >= TR_PATS || len < 2 || s[0] != 'c' || s[1] != '1') return 0;
    tr_lane_t tmp[TR_LANES];
    int o = 2;
    for (int k = 0; k < TR_LANES; k++) {
        tr_lane_t *l = &tmp[k];
        tr_lane_default(l);
        if (o + 10 > len) return 0;
        int lap = tr_unhex(s + o), gate = tr_unhex(s + o + 2), vel = tr_unhex(s + o + 4), mu = tr_unhex(s + o + 6), n = tr_unhex(s + o + 8);
        if (lap < 0 || gate < 0 || vel < 0 || mu < 0 || n < 0 || n > TR_POS) return 0;
        o += 10;
        l->lap = (uint8_t)tr_clamp(lap, 0, TR_NLAPS - 1); l->gate = (uint8_t)tr_clamp(gate, 0, TR_NGATES - 1);
        l->vel = (uint8_t)tr_clamp(vel, 1, 127); l->muted = (uint8_t)(mu & 1);
        for (int j = 0; j < n; j++) {
            if (o + 6 > len) return 0;
            int i = tr_unhex(s + o), note = tr_unhex(s + o + 2), key = tr_unhex(s + o + 4);
            if (i < 0 || i >= TR_POS || note < 0 || key < 0) return 0;
            l->note[i] = (uint8_t)tr_clamp(note, 0, 127); l->key[i] = (uint8_t)tr_clamp(key, 0, 23);
            o += 6;
        }
    }
    if (o != len) return 0;
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    for (int k = 0; k < TR_LANES; k++) tr_copy_lane(&b->pats[p][k], &tmp[k]);
    return 1;
}
int tr_serialize_state(int pos, char *out)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    char *o = out;
    *o++ = 's'; *o++ = '1';
    o = tr_hex(tr_hex(tr_hex(o, b->sel), b->pat), b->cur);
    *o = 0;
    return (int)(o - out);
}
int tr_parse_state(int pos, const char *s, int len)
{
    if (len != 8 || s[0] != 's' || s[1] != '1') return 0;
    int sel = tr_unhex(s + 2), pat = tr_unhex(s + 4), cur = tr_unhex(s + 6);
    if (sel < 0 || pat < 0 || cur < 0) return 0;
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    b->sel = (uint8_t)tr_clamp(sel, 0, TR_LANES - 1); b->pat = (uint8_t)tr_clamp(pat, 0, TR_PATS - 1);
    b->cur = (uint8_t)(cur % TR_POS);
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Left: the track, 6 concentric lanes around (80, 70), lane 1 the outer one; the start / finish line at the top
 * (chequered); each lane's car where it is on its lap (stopped: parked on the start line, dim); stop signs on their
 * positions (selected lane: red octagons with the white band; other lanes: red dots), white for a moment when
 * played; the cursor (white, red while recording) across the selected lane.
 * Right: the selected lane and what the encoders do, in their colours (page 2 marked with a bar):
 *   LANE n / POS p + the sign's note / LAP n (page 2: GATE x) / DRIVE or PARK (page 2: LVL n); bottom: P1..P4, REC.
 * The SVG (trackracer.svg) holds a divider only; everything else is raster lines (stroke text: seq_text.h). */
#define TR_CX   80
#define TR_CY   70                                     /* the OP-1 hides the bottom rows: keep y <= ~138 */
static const uint8_t tr_r[TR_LANES] = { 64, 54, 44, 34, 24, 15 };
static const uint8_t tr_col[TR_LANES][4] = {            /* car colours: the encoder colours, then orange and violet */
    { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF },
    { 0xFF, 0x3A, 0x5D, 0xFF }, { 0xFF, 0x9A, 0x3C, 0xFF }, { 0xB9, 0x8C, 0xFF, 0xFF } };
static const uint8_t tr_knob[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                       { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t tr_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t tr_lite[4]  = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t tr_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t tr_dark[4]  = { 0x3A, 0x3C, 0x3E, 0xFF };
static const uint8_t tr_red[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };

static inline int32_t tr_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tr_cos(int32_t a) { return OP1_SINE_TABLE[(a + 2048) & 8191]; }
/* point at angle a (8192 = full turn, 0 = top, clockwise) and radius rad (Q10) */
static void tr_pt(int32_t a, int32_t rad, int32_t *x, int32_t *y)
{
    *x = OP1_Q10(TR_CX) + (int32_t)(((int64_t)tr_sin(a) * rad) >> 15);
    *y = OP1_Q10(TR_CY) - (int32_t)(((int64_t)tr_cos(a) * rad) >> 15);
}
static void tr_dim(uint8_t *o, const uint8_t *c, int l)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
static void tr_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (const char *s = nm[n % 12]; *s; ) *p++ = *s++;
    int o = n / 12 - 1;
    if (o < 0) { *p++ = '-'; o = -o; }
    *p++ = (char)('0' + o % 10); *p = 0;
}

/* a stop sign: a filled red octagon (flat top) of radius rs (Q10) with the white band; small = a red dot.
 * Filled with 5 horizontal strokes: an octagon's half width is 0.924 rs up to |dy| = 0.383 rs, then shrinks to
 * 0.383 rs at the top and bottom edges. */
static void tr_sign(void *r, int32_t x, int32_t y, int32_t rs, int full, const uint8_t *c)
{
    if (!full) { sq_dot(r, x, y, rs * 2, c); return; }
    static const int16_t dy[5] = { -760, -380, 0, 380, 760 };   /* * rs / 1024 */
    int32_t w = (rs * 460) >> 10;                        /* stroke width ~ 0.45 rs */
    for (int j = 0; j < 5; j++) {
        int32_t d = dy[j] < 0 ? -dy[j] : dy[j];
        int32_t hw = d <= 392 ? 946 : 946 - (d - 392);  /* half width, * rs / 1024 */
        int32_t yy = y + ((rs * dy[j]) >> 10), h = (rs * hw) >> 10;
        sq_line(r, x - h, yy, x + h, yy, w, c);
    }
    if (c[1] < 0x80) sq_line(r, x - ((rs * 640) >> 10), y, x + ((rs * 640) >> 10), y, (rs * 300) >> 10, tr_white);   /* band */
}

/* a little car at angle a on radius rad, driving clockwise: body, cabin, wheels */
static void tr_car_draw(void *r, int32_t a, int32_t rad, const uint8_t *c)
{
    int32_t x, y; tr_pt(a, rad, &x, &y);
    int32_t tx = tr_cos(a) >> 5, ty = tr_sin(a) >> 5;    /* direction of travel (Q10 unit vector) */
    int32_t nx = tr_sin(a) >> 5, ny = -tr_cos(a) >> 5;   /* outward */
#define CP(u, v, X, Y) do { X = x + (int32_t)(((int64_t)(u) * tx + (int64_t)(v) * nx) >> 10); \
                            Y = y + (int32_t)(((int64_t)(u) * ty + (int64_t)(v) * ny) >> 10); } while (0)
    int32_t ax, ay, bx, by, cx, cy, dx, dy;
    CP(OP1_Q10(4), OP1_Q10(2), ax, ay); CP(OP1_Q10(4), -OP1_Q10(2), bx, by);
    CP(-OP1_Q10(4), -OP1_Q10(2), cx, cy); CP(-OP1_Q10(4), OP1_Q10(2), dx, dy);
    sq_line(r, ax, ay, bx, by, 0x500, c); sq_line(r, bx, by, cx, cy, 0x500, c);
    sq_line(r, cx, cy, dx, dy, 0x500, c); sq_line(r, dx, dy, ax, ay, 0x500, c);
    CP(OP1_Q10(1.5), OP1_Q10(2), ax, ay); CP(OP1_Q10(1.5), -OP1_Q10(2), bx, by);   /* windscreen */
    sq_line(r, ax, ay, bx, by, 0x400, c);
    CP(-OP1_Q10(1.5), OP1_Q10(2), cx, cy); CP(-OP1_Q10(1.5), -OP1_Q10(2), dx, dy); /* rear window */
    sq_line(r, cx, cy, dx, dy, 0x300, c);
#undef CP
}

static void tr_screen_draw(void *r, int pos)
{
    tr_bank_t *b = &tr[pos & 1];
    tr_bank_init(b);
    uint64_t now = b->tick_q16;
    uint8_t c[4];

    sq_text(r, OP1_Q10(2), OP1_Q10(2), OP1_Q10(1), TR_VERSION, 0x280, tr_grey);   /* version, top left */

    /* start / finish line: chequered dashes at the top, across all lanes */
    for (int y = TR_CY - tr_r[0] - 5, n = 0; y < TR_CY - tr_r[TR_LANES - 1] + 5; y += 3, n++)
        sq_line(r, OP1_Q10(TR_CX + (n & 1 ? 1 : -1)), OP1_Q10(y), OP1_Q10(TR_CX + (n & 1 ? 1 : -1)), OP1_Q10(y + 3), 0x200, n & 1 ? tr_white : tr_grey);

    for (int k = 0; k < TR_LANES; k++) {
        const tr_lane_t *l = tr_lane(b, k);
        const tr_car_t *car = &b->car[k];
        int sel = k == b->sel;
        int32_t rad = OP1_Q10(tr_r[k]);
        /* the lane: a polygon, finer for the outer lanes; muted lanes dashed */
        int nseg = tr_r[k] / 2 + 8;
        const uint8_t *lc = sel ? tr_lite : car->running ? tr_grey : tr_dark;
        int32_t px, py; tr_pt(0, rad, &px, &py);
        for (int j = 1; j <= nseg; j++) {
            int32_t qx, qy; tr_pt(j * 8192 / nseg, rad, &qx, &qy);
            if (!l->muted || (j & 1)) sq_line(r, px, py, qx, qy, sel ? 0x500 : 0x400, lc);
            px = qx; py = qy;
        }
        /* stop signs; white for 1/16 beat after they play */
        int32_t rs = OP1_Q10(sel ? 2.6 : 2.1) + (int32_t)((int64_t)OP1_Q10(1.1) * (tr_r[k] - 15) / 49);
        for (int i = 0; i < TR_POS; i++) if (l->key[i] != TR_EMPTY) {
            int32_t x, y; tr_pt(i * 8192 / TR_POS, rad, &x, &y);
            int lit = car->hit == i && (uint32_t)(now >> 16) - car->hit_tick < TR_PPQN / 4;
            const uint8_t *sc = lit ? tr_white : tr_red;
            if (l->muted && !lit) { tr_dim(c, tr_red, 110); sc = c; }
            tr_sign(r, x, y, rs, sel, sc);
        }
        /* cursor on the selected lane */
        if (sel) {
            int32_t a = b->cur * 8192 / TR_POS, x0, y0, x1, y1;
            tr_pt(a, rad - OP1_Q10(5), &x0, &y0); tr_pt(a, rad + OP1_Q10(5), &x1, &y1);
            sq_line(r, x0, y0, x1, y1, 0x300, b->rec ? tr_red : tr_white);
        }
        /* the car: where it is on its lap; stopped = parked on the start line, dim */
        int32_t a = 0;
        if (tr_started(b, car)) {
            uint64_t lap = tr_lap_q16(l);
            a = (int32_t)((tr_elapsed(b, car) % lap) * 8192 / lap);
        }
        if (car->running) tr_car_draw(r, a, rad, tr_col[k]);
        else { tr_dim(c, tr_col[k], 90); tr_car_draw(r, a, rad, c); }
    }

    /* panel */
    const tr_lane_t *l = tr_lane(b, b->sel);
    const tr_car_t *car = &b->car[b->sel];
    char t[24], *p;
    int32_t X = OP1_Q10(186), S = OP1_Q10(1.7), W = 0x400;
    for (int k = 0; k < 4; k++) {
        int32_t y = OP1_Q10(8 + k * 24);
        sq_dot(r, OP1_Q10(178), y + OP1_Q10(5), 0x1600, tr_knob[k]);
        if (b->page[k]) sq_line(r, OP1_Q10(172), y, OP1_Q10(172), y + OP1_Q10(10), 0x300, tr_knob[k]);   /* page 2 */
        p = t;
        if (k == 0) { p = sq_cat(t, "LANE "); *p++ = (char)('1' + b->sel); *p = 0; }
        else if (k == 1) {
            p = sq_cat(t, "POS "); p = sq_num(p, (uint32_t)b->cur + 1, 0);
            sq_text(r, X, y, S, t, W, tr_knob[k]);
            if (l->key[b->cur] != TR_EMPTY) tr_note_name(t, l->note[b->cur]); else sq_cat(t, "--");
            sq_text(r, OP1_Q10(270), y, S, t, W, tr_white);
            continue;
        }
        else if (k == 2) {
            if (b->page[2]) sq_cat(sq_cat(t, "GATE "), tr_gate_names[l->gate]);
            else { p = sq_num(sq_cat(t, "LAP "), tr_laps[l->lap], 0); }
        }
        else {
            if (b->page[3]) sq_num(sq_cat(t, "LVL "), l->vel, 0);
            else sq_cat(t, l->muted ? "MUTE" : car->running ? "DRIVE" : "PARK");
        }
        sq_text(r, X, y, S, t, W, tr_knob[k]);
    }
    /* signs on this lane, pattern, record */
    {
        int n = 0;
        for (int i = 0; i < TR_POS; i++) n += l->key[i] != TR_EMPTY;
        p = sq_num(t, (uint32_t)n, 0); sq_cat(p, n == 1 ? " STOP" : " STOPS");
        sq_text(r, OP1_Q10(178), OP1_Q10(104), OP1_Q10(1.4), t, 0x380, tr_grey);
    }
    t[0] = 'P'; t[1] = (char)('1' + b->pat); t[2] = 0;
    sq_text(r, OP1_Q10(178), OP1_Q10(122), OP1_Q10(1.7), t, W, tr_white);
    if (b->rec) sq_text(r, OP1_Q10(236), OP1_Q10(122), OP1_Q10(1.7), car->running ? "LIVE" : "REC", W, tr_red);
}

static void tr_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    tr_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "track racer", type 3 + tr_active[mode], as euclid) =======
 * The plugin loader adds the seq_types row; dispatcher.c selects it (tr_active), creates the player through
 * tr_player_get and shows SCREEN_trackracer in the finger screen slot 0x2D. Player vtable as euclid's (ok_player
 * layout): +0x14 key_down(key 0..23) -> 1 = consumed, +0x24 reset, +0x2c tick (96 per beat), +0x3c stop. */
#define TR_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define TR_SHIFT()     (*(volatile uint8_t *)0x01DD7F78)
#define TR_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define TR_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
extern volatile uint8_t eu_rec_play;                    /* core: Shift + C# with REC held -> PLAY */
static int tr_nop(void *self) { (void)self; return 0; }
static int tr_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = TR_MODE();
    if (TR_SHIFT()) {                                   /* Shift + key shortcuts (any screen) */
        int h = tr_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)            /* track racer screen shown: record (if on) */
        tr_key(m, key + 0x35 + (m ? TR_DRUM_OCT() : TR_SYNTH_OCT()) * 12, key);
    return 0;                                           /* never consumed: the key sounds as usual */
}
static void tr_reset(void *self) { (void)self; tr_all_off(TR_MODE()); }
static void tr_ptick(void *self, int counter) { (void)self; (void)counter; tr_advance(TR_MODE(), 1u << 14); }   /* 1/96 beat */
static void tr_pstop(void *self) { (void)self; tr_all_off(0); tr_all_off(1); }
static const uint32_t tr_vt[16] = {
    0, 0,  0, (uint32_t)tr_nop,  0, (uint32_t)tr_key_down,  0, (uint32_t)tr_nop,
    0, (uint32_t)tr_reset,  0, (uint32_t)tr_ptick,  0, (uint32_t)tr_nop,  0, (uint32_t)tr_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } tr_player = { tr_vt, { 0 } };
volatile uint8_t tr_active[2];                          /* track racer selected (synth, drum) */

/* persistence: bg_saver + DB as dots, table trackracerseq: ids 0..7 lanes (mode * 4 + pattern), 8 / 9 state */
#define TR_BG_REGISTER ((void (*)(void *))0x0193F010)
#define TR_BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define TR_DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define TR_DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define TR_DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } tr_dstr;
static const char TR_CREATE[] = "CREATE TABLE IF NOT EXISTS trackracerseq (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static char tr_sql[64 + TR_TEXT_LEN];                   /* < 8 KB: the firmware's SQL format buffer */
typedef char tr_sql_fits[sizeof tr_sql < 0x2000 - 64 ? 1 : -1];
static void tr_bg_nop(void *self) { (void)self; }
static void tr_bg_save(void *self)                      /* saver thread */
{
    (void)self;
    void *db = TR_DB_HANDLE();
    TR_DB_EXEC(db, TR_CREATE);
    for (int id = 0; id < 2 * TR_PATS + 2; id++) {
        char *o = sq_cat(tr_sql, "INSERT OR REPLACE INTO trackracerseq VALUES (");
        *o++ = (char)('0' + id); *o++ = ','; *o++ = '\'';
        o += id < 2 * TR_PATS ? tr_serialize(id / TR_PATS, id % TR_PATS, o) : tr_serialize_state(id - 2 * TR_PATS, o);
        *o++ = '\''; *o++ = ')'; *o = 0;
        op1_db_write(db, tr_sql);                       /* (no assert: long statement, docs/17) */
    }
}
static const uint32_t tr_bg_vt[6] = { 0, 0, 0, (uint32_t)tr_bg_nop, 0, (uint32_t)tr_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } tr_bg;
static uint8_t tr_bg_ready;
static void tr_changed(void) { if (tr_bg_ready) TR_BG_DIRTY(&tr_bg); }
/* rows written before the rename (table circuitseq) are read once when the new table has none, then saved anew */
static const char TR_CREATE_OLD[] = "CREATE TABLE IF NOT EXISTS circuitseq (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static int tr_load_row(void *db, const char *table_q, int id)
{
    static char q[48];
    char *o = sq_cat(q, table_q);
    *o++ = (char)('0' + id); *o = 0;
    tr_dstr s = { 0 };
    TR_DB_GET(&s, db, q);
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    if (!s.size || s.size > TR_TEXT_LEN) return 0;      /* (heap copy, once per boot, not freed) */
    return id < 2 * TR_PATS ? tr_parse(id / TR_PATS, id % TR_PATS, p, (int)s.size)
                            : tr_parse_state(id - 2 * TR_PATS, p, (int)s.size);
}
static void tr_load(void)
{
    void *db = TR_DB_HANDLE();
    TR_DB_EXEC(db, TR_CREATE);
    TR_DB_EXEC(db, TR_CREATE_OLD);
    int migrated = 0;
    for (int id = 0; id < 2 * TR_PATS + 2; id++)
        if (!tr_load_row(db, "SELECT data FROM trackracerseq WHERE id=", id))
            migrated |= tr_load_row(db, "SELECT data FROM circuitseq WHERE id=", id);
    TR_BG_REGISTER(&tr_bg);
    tr_bg.vt = tr_bg_vt;
    tr_bg_ready = 1;
    if (migrated) TR_BG_DIRTY(&tr_bg);                  /* write them to the new table */
}
void *tr_player_get(void)
{
    if (!tr_bg_ready) tr_load();
    return &tr_player;
}

static void tr_scr_init(void *scr) { (void)scr; }
static void tr_scr_update(void *scr, int dt) { tr_screen_update(scr, dt, TR_MODE()); }
const op1_screen_t SCREEN_trackracer = { tr_scr_init, tr_scr_update };

/* plugin interface (docs/44): lift / drop / scissors, encoder presses, REC; turns (Shift passed on) */
static int tr_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code == 0x0A || code == 0x0B) { if (pressed) tr_clip(m, code == 0x0B, shift); return 1; }
    if (code == 0x0C) { if (pressed) tr_delete(m, shift); return 1; }
    if (knob >= 0) { if (pressed) tr_press(m, knob); return 1; }
    if (code == 0x21) { if (pressed) tr_rec_key(m); return 1; }
    return 0;
}
static void tr_pturn(int m, int knob, int delta, int shift) { tr_turn(m, knob, delta, shift); }
const op1_seq_t SEQ_trackracer = { tr_active, tr_player_get, tr_pkey, tr_pturn, tr_midi_start, 0 };
#endif
