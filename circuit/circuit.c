/* "circuit": a circular sequencer for op1r (synth and drum mode), after Google's Racer experiment with Giorgio
 * Moroder. Six concentric lanes, a little car on each; a lane plays when its car drives over a stop sign.
 *
 * Lanes: 6 per mode (= the OP-1's 6 voices), lane 1 the outer one. Each lane has its own lap length (1 2 3 4 5 6 7 8
 * 12 16 beats), so the cars drive at different speeds and the lanes phase against each other (polyrhythms). A lap has
 * 64 positions; a stop sign sits on one of them and holds its own note (synth: a pitch; drum: a drum sound + pitch),
 * so a lane is usually one sound but can also play a line. One note at a time per lane (a new sign cuts the last).
 *
 * Controls (circuit screen shown; Shift = the shift key held):
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
 * Any screen while circuit is the mode's sequencer (as euclid / dots):
 *   Shift + F G A B (keys 0 2 4 6)            pattern 1..4 (cars keep running, in phase)
 *   Shift + G A B C D E (14 16 18 19 21 23)   mute lane 1..6 (the car keeps driving), Shift + D# (22) all
 *   Shift + C# (20)                           stop all if any car runs, else start all at once; with REC held
 *                                             (tape armed) the start also presses PLAY
 * Saved (bg_saver, DB table circuitseq): rows 0..7 = mode * 4 + pattern (lanes), rows 8 / 9 = selected lane,
 * pattern, cursor per mode. Cars come back stopped.
 *
 * Clock as euclid: 24 ticks per beat (Q16) at the OP-1 tempo, advanced by the player tick (96 per beat). A single
 * car starts on the next beat; Shift + C# starts all at once. Changing a running lane's lap keeps the car where it
 * is on the circle (the phase is kept).
 *
 * Platform independent (host test: test/circuit_test.c); firmware glue at the end.
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "seq/seq_text.h"

#define CI_LANES   6
#define CI_POS     64                                   /* positions per lap */
#define CI_PATS    4
#define CI_PPQN    24
#define CI_EMPTY   0xFF                                 /* key of an empty position */
#define CI_TIE     6                                    /* gate index: hold until the next sign */
#define CI_NLAPS   10
#define CI_NGATES  7
static const uint8_t ci_laps[CI_NLAPS] = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 16 };    /* beats per lap */
static const uint8_t ci_gates[CI_NGATES] = { 3, 6, 12, 24, 48, 96, 0 };          /* ticks (24 per beat); TIE */
static const char *const ci_gate_names[CI_NGATES] = { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1", "TIE" };

typedef struct {                                        /* a lane's settings (saved, per pattern) */
    uint8_t note[CI_POS], key[CI_POS];                  /* sign: key 0..23, note = absolute pitch; key CI_EMPTY: none */
    uint8_t lap, gate, vel, muted;                      /* lap / gate: index into ci_laps / ci_gates */
} ci_lane_t;

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
} ci_car_t;

typedef struct {
    ci_lane_t pats[CI_PATS][CI_LANES];
    ci_car_t  car[CI_LANES];
    uint8_t   init, sel, pat, cur, rec;                 /* selected lane, pattern, cursor (0..63), record on */
    uint8_t   page[4];                                  /* encoder page per knob (white: gate, red: level) */
    int32_t   acc[4];                                   /* encoder accumulators (one step per 1024) */
    uint64_t  tick_q16;                                 /* clock, ticks (24 per beat) Q16 */
} ci_bank_t;

static ci_bank_t ci[2];                                 /* 0 synth, 1 drum */
static ci_lane_t ci_clip_lane[2], ci_clip_pat[2][CI_LANES];
static uint8_t   ci_clip_full[2][2];

#ifdef OP1_HOST
int host_ci_changed;
void host_ci_note_on(int pos, int note, int key, int vel);
void host_ci_note_off(int pos, int note);
#define CI_CHANGED()             (host_ci_changed++)
#define CI_NOTE_ON(pos, n, k, v) (host_ci_note_on((pos), (n), (k), (v)), (n))
#define CI_NOTE_OFF(pos, n)      host_ci_note_off((pos), (n))
#else
static void ci_changed(void);
#define CI_CHANGED()             ci_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);    /* core (seq/seq_core.c), shared by the sequencers */
void eu_fw_note_off(int midi);
#define CI_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define CI_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#endif

static int ci_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* struct copies: no libc in a plugin (GCC turns a struct assignment or a plain copy loop into a memcpy call) */
static void ci_copy_lane(ci_lane_t *d, const ci_lane_t *s)
{
    volatile uint8_t *o = (volatile uint8_t *)d;
    const uint8_t *i = (const uint8_t *)s;
    for (unsigned n = 0; n < sizeof *d; n++) o[n] = i[n];
}

static void ci_lane_default(ci_lane_t *l)
{
    for (int i = 0; i < CI_POS; i++) { l->key[i] = CI_EMPTY; l->note[i] = 0; }
    l->lap = 3; l->gate = 1; l->vel = 100; l->muted = 0;                 /* 1 bar, 1/16, level 100 */
}
static void ci_bank_init(ci_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->sel = 0; b->pat = 0; b->cur = 0; b->rec = 0;
    for (int p = 0; p < CI_PATS; p++) for (int k = 0; k < CI_LANES; k++) ci_lane_default(&b->pats[p][k]);
    for (int k = 0; k < CI_LANES; k++) { b->car[k].last = -1; b->car[k].skip = -1; b->car[k].hit = -1; }
}
static ci_lane_t *ci_lane(ci_bank_t *b, int k) { return &b->pats[b->pat][k]; }
static uint64_t ci_lap_q16(const ci_lane_t *l) { return (uint64_t)ci_laps[l->lap] * CI_PPQN << 16; }
static int ci_started(const ci_bank_t *b, const ci_car_t *c) { return c->running && (int64_t)b->tick_q16 >= c->start; }
static uint64_t ci_elapsed(const ci_bank_t *b, const ci_car_t *c) { return (uint64_t)((int64_t)b->tick_q16 - c->start); }
/* keep a running car where it is on its circle when its lap changes from lo to ln (Q16 ticks) */
static void ci_rephase(ci_bank_t *b, ci_car_t *c, uint64_t lo, uint64_t ln)
{
    if (!ci_started(b, c)) return;
    uint64_t f = (ci_elapsed(b, c) % lo) * 65536 / lo;                   /* phase 0..65535 */
    c->start = (int64_t)b->tick_q16 - (int64_t)(f * ln / 65536);
    c->last = (int16_t)(ci_elapsed(b, c) % ln * CI_POS / ln);
}

static void ci_silence(int pos, ci_car_t *c)
{
    if (c->sounding) { CI_NOTE_OFF(pos, c->sounding - 1); c->sounding = 0; }
}

/* ---- the clock: advance by inc (Q16 ticks, 24 per beat); cars play the signs they drive over ---- */
static void ci_play(int pos, ci_bank_t *b, int k, int i)
{
    ci_car_t *c = &b->car[k];
    const ci_lane_t *l = ci_lane(b, k);
    if (c->skip == i) { c->skip = -1; return; }          /* just recorded live (it already sounded) */
    if (l->key[i] == CI_EMPTY || l->muted) return;
    ci_silence(pos, c);
    int id = CI_NOTE_ON(pos, l->note[i], l->key[i], l->vel);
    if (id >= 0) c->sounding = (uint8_t)(id + 1);
    c->off_at = l->gate == CI_TIE ? ~0ull : b->tick_q16 + ((uint64_t)ci_gates[l->gate] << 16);
    c->hit = (int16_t)i; c->hit_tick = (uint32_t)(b->tick_q16 >> 16);
}
static void ci_advance(int pos, uint64_t inc)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    uint64_t t = b->tick_q16;                            /* this tick's own position (as euclid) */
    b->tick_q16 = t + inc;
    for (int k = 0; k < CI_LANES; k++) {
        ci_car_t *c = &b->car[k];
        if (c->sounding && t >= c->off_at) ci_silence(pos & 1, c);
        if (!c->running || (int64_t)t < c->start) continue;
        uint64_t lap = ci_lap_q16(ci_lane(b, k));
        int i = (int)(((uint64_t)((int64_t)t - c->start) % lap) * CI_POS / lap);
        if (c->last < 0) { ci_play(pos & 1, b, k, i); c->last = (int16_t)i; continue; }
        for (int n = 0; c->last != i && n < CI_POS; n++) {        /* every position passed since the last tick */
            c->last = (int16_t)((c->last + 1) % CI_POS);
            ci_play(pos & 1, b, k, c->last);
        }
    }
}

/* host: advance by n samples at bpm_x100 (tempo * 100) */
void ci_tick(int pos, int n, uint32_t bpm_x100)
{
    ci_advance(pos, ((uint64_t)bpm_x100 * CI_PPQN << 16) * (uint64_t)n / (6000ull * 44100ull));
}

/* ---- starting / stopping cars ---- */
static void ci_start(ci_bank_t *b, int k, int now)
{
    ci_car_t *c = &b->car[k];
    if (c->running) return;
    uint64_t t = b->tick_q16, beat = (uint64_t)CI_PPQN << 16;
    c->start = (int64_t)(now ? t : (t / beat + 1) * beat);   /* now, or the next beat */
    c->last = -1; c->skip = -1; c->running = 1;
}
static void ci_stop(int pos, ci_bank_t *b, int k)
{
    ci_car_t *c = &b->car[k];
    if (!c->running) return;
    c->running = 0; c->last = -1; ci_silence(pos, c);
}
static int ci_any_running(const ci_bank_t *b)
{
    for (int k = 0; k < CI_LANES; k++) if (b->car[k].running) return 1;
    return 0;
}
/* Shift + C#: stop all if any car runs, else start all at once. Returns 1 if it started. */
int ci_toggle_all(int pos)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    int any = ci_any_running(b);
    for (int k = 0; k < CI_LANES; k++) { if (any) ci_stop(pos & 1, b, k); else ci_start(b, k, 1); }
    return !any;
}
void ci_all_off(int pos)
{
    for (int k = 0; k < CI_LANES; k++) ci_silence(pos & 1, &ci[pos & 1].car[k]);
}

/* ---- patterns, mutes ---- */
void ci_recall(int pos, int p)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    if (p < 0 || p >= CI_PATS || p == b->pat) return;
    for (int k = 0; k < CI_LANES; k++) {                 /* keep each car where it is on its circle */
        ci_car_t *c = &b->car[k];
        ci_silence(pos & 1, c);
        ci_rephase(b, c, ci_lap_q16(&b->pats[b->pat][k]), ci_lap_q16(&b->pats[p][k]));
    }
    b->pat = (uint8_t)p;
    CI_CHANGED();
}
void ci_mute(int pos, int k)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    if (k < 0 || k >= CI_LANES) return;
    ci_lane_t *l = ci_lane(b, k);
    l->muted ^= 1;
    if (l->muted) ci_silence(pos & 1, &b->car[k]);
    CI_CHANGED();
}
void ci_mute_all(int pos)                               /* all muted -> unmute all, else mute all */
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    int all = 1;
    for (int k = 0; k < CI_LANES; k++) if (!ci_lane(b, k)->muted) all = 0;
    for (int k = 0; k < CI_LANES; k++) { ci_lane(b, k)->muted = (uint8_t)!all; if (!all) ci_silence(pos & 1, &b->car[k]); }
    CI_CHANGED();
}
/* Shift + key: F G A B -> pattern 1..4, G A B C D E -> mute lane 1..6, D# -> mute all, C# -> start / stop all.
 * Returns 0 if not handled, 2 if C# started the cars, else 1. */
int ci_shift_key(int pos, int key)
{
    static const int8_t act[24] = { 10, -1, 11, -1, 12, -1, 13, -1, -1, -1, -1, -1, -1, -1,
                                    0, -1, 1, -1, 2, 3, 8, 4, 9, 5 };
    if (key < 0 || key > 23 || act[key] < 0) return 0;
    if (act[key] >= 10) { ci_recall(pos, act[key] - 10); return 1; }
    if (act[key] == 8) return ci_toggle_all(pos) ? 2 : 1;
    if (act[key] == 9) ci_mute_all(pos); else ci_mute(pos, act[key]);
    return 1;
}

/* ---- signs ---- */
static void ci_put(ci_bank_t *b, int k, int i, int note, int key)
{
    ci_lane_t *l = ci_lane(b, k);
    l->note[i] = (uint8_t)ci_clamp(note, 0, 127); l->key[i] = (uint8_t)ci_clamp(key, 0, 23);
}
/* note key on the circuit screen with record on: a sign at the cursor (car stopped) or under the car (running).
 * Returns the position written, -1 if nothing was recorded. The key is never consumed: it sounds as usual. */
int ci_key(int pos, int note, int key)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    if (!b->rec) return -1;
    int k = b->sel, i = b->cur;
    ci_car_t *c = &b->car[k];
    if (ci_started(b, c)) {                              /* live: the nearest position to the car */
        uint64_t lap = ci_lap_q16(ci_lane(b, k)), t = ci_elapsed(b, c) % lap;
        i = (int)((t * CI_POS + lap / 2) / lap) % CI_POS;
        if (i != c->last) c->skip = (int16_t)i;           /* still ahead: do not play it again this pass */
        b->cur = (uint8_t)i;                              /* the cursor follows what was recorded */
    }
    ci_put(b, k, i, note, key);
    CI_CHANGED();
    return i;
}
void ci_rec_key(int pos) { ci_bank_t *b = &ci[pos & 1]; ci_bank_init(b); b->rec ^= 1; }

/* scissors: delete the sign at the cursor; Shift: clear the lane */
void ci_delete(int pos, int all)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    ci_lane_t *l = ci_lane(b, b->sel);
    if (all) { for (int i = 0; i < CI_POS; i++) l->key[i] = CI_EMPTY; ci_silence(pos & 1, &b->car[b->sel]); }
    else l->key[b->cur] = CI_EMPTY;
    CI_CHANGED();
}
/* lift / drop: copy / paste the selected lane (all = 1: the whole pattern). One clipboard of each per mode. */
void ci_clip(int pos, int paste, int all)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    int m = pos & 1, a = all != 0;
    if (!paste) {
        if (a) for (int k = 0; k < CI_LANES; k++) ci_copy_lane(&ci_clip_pat[m][k], ci_lane(b, k));
        else ci_copy_lane(&ci_clip_lane[m], ci_lane(b, b->sel));
        ci_clip_full[m][a] = 1;
        return;
    }
    if (!ci_clip_full[m][a]) return;
    for (int k = a ? 0 : b->sel; k < (a ? CI_LANES : b->sel + 1); k++) {
        ci_silence(m, &b->car[k]);
        uint64_t lo = ci_lap_q16(ci_lane(b, k));
        ci_copy_lane(ci_lane(b, k), a ? &ci_clip_pat[m][k] : &ci_clip_lane[m]);
        ci_rephase(b, &b->car[k], lo, ci_lap_q16(ci_lane(b, k)));       /* keep the car's place on the circle */
    }
    CI_CHANGED();
}

/* ---- encoders ---- */
static int ci_steps(ci_bank_t *b, int knob, int delta)
{
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;   /* direction change */
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    b->acc[knob] -= st * 1024;
    return st;
}
void ci_press(int pos, int knob)
{
    ci_bank_t *b = &ci[pos & 1];
    if (knob == 2 || knob == 3) b->page[knob] ^= 1;     /* white: lap / gate, red: run / level */
}
void ci_turn(int pos, int knob, int delta, int shift)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int st = ci_steps(b, knob, delta);
    if (!st) return;
    ci_lane_t *l = ci_lane(b, b->sel);
    ci_car_t *c = &b->car[b->sel];
    switch (knob) {
    case 0:                                              /* blue: lane (not saved alone: written with the next change) */
        b->sel = (uint8_t)(((b->sel + st) % CI_LANES + CI_LANES) % CI_LANES);
        return;
    case 1:                                              /* green: cursor */
        b->cur = (uint8_t)(((b->cur + st * (shift ? 4 : 1)) % CI_POS + CI_POS) % CI_POS);
        return;
    case 2:
        if (b->page[2]) { l->gate = (uint8_t)ci_clamp(l->gate + st, 0, CI_NGATES - 1); break; }
        {                                                /* lap: keep the car's place on the circle */
            uint64_t lo = ci_lap_q16(l);
            int nl = ci_clamp(l->lap + st, 0, CI_NLAPS - 1);
            if (nl == l->lap) return;
            l->lap = (uint8_t)nl;
            ci_rephase(b, c, lo, ci_lap_q16(l));
        }
        break;
    case 3:
        if (b->page[3]) { l->vel = (uint8_t)ci_clamp(l->vel + st * 4, 1, 127); break; }
        for (int k = shift ? 0 : b->sel; k < (shift ? CI_LANES : b->sel + 1); k++) {   /* run (not saved) */
            if (st > 0) ci_start(b, k, 0); else ci_stop(pos & 1, b, k);
        }
        return;
    }
    CI_CHANGED();
}

/* MIDI START: running cars restart from the start line on the next clock tick (the master's downbeat) */
void ci_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        ci_bank_t *b = &ci[pos];
        b->tick_q16 = 0;
        for (int k = 0; k < CI_LANES; k++) {
            ci_car_t *c = &b->car[k];
            if (!c->running) continue;
            ci_silence(pos, c);
            c->start = 0; c->last = -1; c->skip = -1;
        }
    }
}

/* ---- persistence text (hex, no libc) ----
 * lanes row (mode, pattern): "c1" then per lane: lap gate vel muted count (2 hex each), then count signs as
 * position note key (2 hex each). Longest: 2 + 6 * (10 + 64 * 6) = 2366 chars.
 * state row (mode): "s1" sel pat cur (2 hex each). */
#define CI_TEXT_LEN (2 + CI_LANES * (10 + CI_POS * 6))
static const char ci_hexd[] = "0123456789ABCDEF";
static char *ci_hex(char *p, int v) { *p++ = ci_hexd[(v >> 4) & 15]; *p++ = ci_hexd[v & 15]; return p; }
static int ci_unhex(const char *s)
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
int ci_serialize(int pos, int p, char *out)              /* out: CI_TEXT_LEN + 1 */
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    char *o = out;
    *o++ = 'c'; *o++ = '1';
    for (int k = 0; k < CI_LANES; k++) {
        const ci_lane_t *l = &b->pats[p][k];
        int n = 0;
        for (int i = 0; i < CI_POS; i++) n += l->key[i] != CI_EMPTY;
        o = ci_hex(ci_hex(ci_hex(ci_hex(ci_hex(o, l->lap), l->gate), l->vel), l->muted), n);
        for (int i = 0; i < CI_POS; i++) if (l->key[i] != CI_EMPTY) o = ci_hex(ci_hex(ci_hex(o, i), l->note[i]), l->key[i]);
    }
    *o = 0;
    return (int)(o - out);
}
int ci_parse(int pos, int p, const char *s, int len)
{
    if (p < 0 || p >= CI_PATS || len < 2 || s[0] != 'c' || s[1] != '1') return 0;
    ci_lane_t tmp[CI_LANES];
    int o = 2;
    for (int k = 0; k < CI_LANES; k++) {
        ci_lane_t *l = &tmp[k];
        ci_lane_default(l);
        if (o + 10 > len) return 0;
        int lap = ci_unhex(s + o), gate = ci_unhex(s + o + 2), vel = ci_unhex(s + o + 4), mu = ci_unhex(s + o + 6), n = ci_unhex(s + o + 8);
        if (lap < 0 || gate < 0 || vel < 0 || mu < 0 || n < 0 || n > CI_POS) return 0;
        o += 10;
        l->lap = (uint8_t)ci_clamp(lap, 0, CI_NLAPS - 1); l->gate = (uint8_t)ci_clamp(gate, 0, CI_NGATES - 1);
        l->vel = (uint8_t)ci_clamp(vel, 1, 127); l->muted = (uint8_t)(mu & 1);
        for (int j = 0; j < n; j++) {
            if (o + 6 > len) return 0;
            int i = ci_unhex(s + o), note = ci_unhex(s + o + 2), key = ci_unhex(s + o + 4);
            if (i < 0 || i >= CI_POS || note < 0 || key < 0) return 0;
            l->note[i] = (uint8_t)ci_clamp(note, 0, 127); l->key[i] = (uint8_t)ci_clamp(key, 0, 23);
            o += 6;
        }
    }
    if (o != len) return 0;
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    for (int k = 0; k < CI_LANES; k++) ci_copy_lane(&b->pats[p][k], &tmp[k]);
    return 1;
}
int ci_serialize_state(int pos, char *out)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    char *o = out;
    *o++ = 's'; *o++ = '1';
    o = ci_hex(ci_hex(ci_hex(o, b->sel), b->pat), b->cur);
    *o = 0;
    return (int)(o - out);
}
int ci_parse_state(int pos, const char *s, int len)
{
    if (len != 8 || s[0] != 's' || s[1] != '1') return 0;
    int sel = ci_unhex(s + 2), pat = ci_unhex(s + 4), cur = ci_unhex(s + 6);
    if (sel < 0 || pat < 0 || cur < 0) return 0;
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    b->sel = (uint8_t)ci_clamp(sel, 0, CI_LANES - 1); b->pat = (uint8_t)ci_clamp(pat, 0, CI_PATS - 1);
    b->cur = (uint8_t)(cur % CI_POS);
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Left: the circuit, 6 concentric lanes around (80, 80), lane 1 the outer one; the start / finish line at the top
 * (chequered); each lane's car where it is on its lap (stopped: parked on the start line, dim); stop signs on their
 * positions (selected lane: red octagons with the white band; other lanes: red dots), white for a moment when
 * played; the cursor (white, red while recording) across the selected lane.
 * Right: the selected lane and what the encoders do, in their colours (page 2 marked with a bar):
 *   LANE n / POS p + the sign's note / LAP n (page 2: GATE x) / DRIVE or PARK (page 2: LVL n); bottom: P1..P4, REC.
 * The SVG (circuit.svg) holds a divider only; everything else is raster lines (stroke text: seq_text.h). */
#define CI_CX   80
#define CI_CY   80
static const uint8_t ci_r[CI_LANES] = { 74, 63, 52, 41, 30, 19 };
static const uint8_t ci_col[CI_LANES][4] = {            /* car colours: the encoder colours, then orange and violet */
    { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF },
    { 0xFF, 0x3A, 0x5D, 0xFF }, { 0xFF, 0x9A, 0x3C, 0xFF }, { 0xB9, 0x8C, 0xFF, 0xFF } };
static const uint8_t ci_knob[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                       { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t ci_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t ci_lite[4]  = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t ci_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t ci_dark[4]  = { 0x3A, 0x3C, 0x3E, 0xFF };
static const uint8_t ci_red[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };

static inline int32_t ci_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t ci_cos(int32_t a) { return OP1_SINE_TABLE[(a + 2048) & 8191]; }
/* point at angle a (8192 = full turn, 0 = top, clockwise) and radius rad (Q10) */
static void ci_pt(int32_t a, int32_t rad, int32_t *x, int32_t *y)
{
    *x = OP1_Q10(CI_CX) + (int32_t)(((int64_t)ci_sin(a) * rad) >> 15);
    *y = OP1_Q10(CI_CY) - (int32_t)(((int64_t)ci_cos(a) * rad) >> 15);
}
static void ci_dim(uint8_t *o, const uint8_t *c, int l)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
static void ci_note_name(char *p, int n)
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
static void ci_sign(void *r, int32_t x, int32_t y, int32_t rs, int full, const uint8_t *c)
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
    if (c[1] < 0x80) sq_line(r, x - ((rs * 640) >> 10), y, x + ((rs * 640) >> 10), y, (rs * 300) >> 10, ci_white);   /* band */
}

/* a little car at angle a on radius rad, driving clockwise: body, cabin, wheels */
static void ci_car_draw(void *r, int32_t a, int32_t rad, const uint8_t *c)
{
    int32_t x, y; ci_pt(a, rad, &x, &y);
    int32_t tx = ci_cos(a) >> 5, ty = ci_sin(a) >> 5;    /* direction of travel (Q10 unit vector) */
    int32_t nx = ci_sin(a) >> 5, ny = -ci_cos(a) >> 5;   /* outward */
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

static void ci_screen_draw(void *r, int pos)
{
    ci_bank_t *b = &ci[pos & 1];
    ci_bank_init(b);
    uint64_t now = b->tick_q16;
    uint8_t c[4];

    /* start / finish line: chequered dashes at the top, across all lanes */
    for (int y = CI_CY - ci_r[0] - 5, n = 0; y < CI_CY - ci_r[CI_LANES - 1] + 5; y += 3, n++)
        sq_line(r, OP1_Q10(CI_CX + (n & 1 ? 1 : -1)), OP1_Q10(y), OP1_Q10(CI_CX + (n & 1 ? 1 : -1)), OP1_Q10(y + 3), 0x200, n & 1 ? ci_white : ci_grey);

    for (int k = 0; k < CI_LANES; k++) {
        const ci_lane_t *l = ci_lane(b, k);
        const ci_car_t *car = &b->car[k];
        int sel = k == b->sel;
        int32_t rad = OP1_Q10(ci_r[k]);
        /* the lane: a polygon, finer for the outer lanes; muted lanes dashed */
        int nseg = ci_r[k] / 2 + 8;
        const uint8_t *lc = sel ? ci_lite : car->running ? ci_grey : ci_dark;
        int32_t px, py; ci_pt(0, rad, &px, &py);
        for (int j = 1; j <= nseg; j++) {
            int32_t qx, qy; ci_pt(j * 8192 / nseg, rad, &qx, &qy);
            if (!l->muted || (j & 1)) sq_line(r, px, py, qx, qy, sel ? 0x500 : 0x400, lc);
            px = qx; py = qy;
        }
        /* stop signs; white for 1/16 beat after they play */
        int32_t rs = OP1_Q10(sel ? 2.6 : 2.1) + (int32_t)((int64_t)OP1_Q10(1.1) * (ci_r[k] - 19) / 55);
        for (int i = 0; i < CI_POS; i++) if (l->key[i] != CI_EMPTY) {
            int32_t x, y; ci_pt(i * 8192 / CI_POS, rad, &x, &y);
            int lit = car->hit == i && (uint32_t)(now >> 16) - car->hit_tick < CI_PPQN / 4;
            const uint8_t *sc = lit ? ci_white : ci_red;
            if (l->muted && !lit) { ci_dim(c, ci_red, 110); sc = c; }
            ci_sign(r, x, y, rs, sel, sc);
        }
        /* cursor on the selected lane */
        if (sel) {
            int32_t a = b->cur * 8192 / CI_POS, x0, y0, x1, y1;
            ci_pt(a, rad - OP1_Q10(5), &x0, &y0); ci_pt(a, rad + OP1_Q10(5), &x1, &y1);
            sq_line(r, x0, y0, x1, y1, 0x300, b->rec ? ci_red : ci_white);
        }
        /* the car: where it is on its lap; stopped = parked on the start line, dim */
        int32_t a = 0;
        if (ci_started(b, car)) {
            uint64_t lap = ci_lap_q16(l);
            a = (int32_t)((ci_elapsed(b, car) % lap) * 8192 / lap);
        }
        if (car->running) ci_car_draw(r, a, rad, ci_col[k]);
        else { ci_dim(c, ci_col[k], 90); ci_car_draw(r, a, rad, c); }
    }

    /* panel */
    const ci_lane_t *l = ci_lane(b, b->sel);
    const ci_car_t *car = &b->car[b->sel];
    char t[24], *p;
    int32_t X = OP1_Q10(186), S = OP1_Q10(1.7), W = 0x400;
    for (int k = 0; k < 4; k++) {
        int32_t y = OP1_Q10(14 + k * 26);
        sq_dot(r, OP1_Q10(178), y + OP1_Q10(5), 0x1600, ci_knob[k]);
        if (b->page[k]) sq_line(r, OP1_Q10(172), y, OP1_Q10(172), y + OP1_Q10(10), 0x300, ci_knob[k]);   /* page 2 */
        p = t;
        if (k == 0) { p = sq_cat(t, "LANE "); *p++ = (char)('1' + b->sel); *p = 0; }
        else if (k == 1) {
            p = sq_cat(t, "POS "); p = sq_num(p, (uint32_t)b->cur + 1, 0);
            sq_text(r, X, y, S, t, W, ci_knob[k]);
            if (l->key[b->cur] != CI_EMPTY) ci_note_name(t, l->note[b->cur]); else sq_cat(t, "--");
            sq_text(r, OP1_Q10(270), y, S, t, W, ci_white);
            continue;
        }
        else if (k == 2) {
            if (b->page[2]) sq_cat(sq_cat(t, "GATE "), ci_gate_names[l->gate]);
            else { p = sq_num(sq_cat(t, "LAP "), ci_laps[l->lap], 0); }
        }
        else {
            if (b->page[3]) sq_num(sq_cat(t, "LVL "), l->vel, 0);
            else sq_cat(t, l->muted ? "MUTE" : car->running ? "DRIVE" : "PARK");
        }
        sq_text(r, X, y, S, t, W, ci_knob[k]);
    }
    /* signs on this lane, pattern, record */
    {
        int n = 0;
        for (int i = 0; i < CI_POS; i++) n += l->key[i] != CI_EMPTY;
        p = sq_num(t, (uint32_t)n, 0); sq_cat(p, n == 1 ? " STOP" : " STOPS");
        sq_text(r, OP1_Q10(178), OP1_Q10(120), OP1_Q10(1.4), t, 0x380, ci_grey);
    }
    t[0] = 'P'; t[1] = (char)('1' + b->pat); t[2] = 0;
    sq_text(r, OP1_Q10(178), OP1_Q10(140), OP1_Q10(1.7), t, W, ci_white);
    if (b->rec) sq_text(r, OP1_Q10(236), OP1_Q10(140), OP1_Q10(1.7), car->running ? "LIVE" : "REC", W, ci_red);
}

static void ci_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    ci_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "circuit", type 3 + ci_active[mode], as euclid) =======
 * The plugin loader adds the seq_types row; dispatcher.c selects it (ci_active), creates the player through
 * ci_player_get and shows SCREEN_circuit in the finger screen slot 0x2D. Player vtable as euclid's (ok_player
 * layout): +0x14 key_down(key 0..23) -> 1 = consumed, +0x24 reset, +0x2c tick (96 per beat), +0x3c stop. */
#define CI_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define CI_SHIFT()     (*(volatile uint8_t *)0x01DD7F78)
#define CI_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define CI_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
extern volatile uint8_t eu_rec_play;                    /* core: Shift + C# with REC held -> PLAY */
static int ci_nop(void *self) { (void)self; return 0; }
static int ci_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = CI_MODE();
    if (CI_SHIFT()) {                                   /* Shift + key shortcuts (any screen) */
        int h = ci_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)            /* circuit screen shown: record (if on) */
        ci_key(m, key + 0x35 + (m ? CI_DRUM_OCT() : CI_SYNTH_OCT()) * 12, key);
    return 0;                                           /* never consumed: the key sounds as usual */
}
static void ci_reset(void *self) { (void)self; ci_all_off(CI_MODE()); }
static void ci_ptick(void *self, int counter) { (void)self; (void)counter; ci_advance(CI_MODE(), 1u << 14); }   /* 1/96 beat */
static void ci_pstop(void *self) { (void)self; ci_all_off(0); ci_all_off(1); }
static const uint32_t ci_vt[16] = {
    0, 0,  0, (uint32_t)ci_nop,  0, (uint32_t)ci_key_down,  0, (uint32_t)ci_nop,
    0, (uint32_t)ci_reset,  0, (uint32_t)ci_ptick,  0, (uint32_t)ci_nop,  0, (uint32_t)ci_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } ci_player = { ci_vt, { 0 } };
volatile uint8_t ci_active[2];                          /* circuit selected (synth, drum) */

/* persistence: bg_saver + DB as dots, table circuitseq: ids 0..7 lanes (mode * 4 + pattern), 8 / 9 state */
#define CI_BG_REGISTER ((void (*)(void *))0x0193F010)
#define CI_BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define CI_DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define CI_DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define CI_DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } ci_dstr;
static const char CI_CREATE[] = "CREATE TABLE IF NOT EXISTS circuitseq (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static char ci_sql[64 + CI_TEXT_LEN];                   /* < 8 KB: the firmware's SQL format buffer */
typedef char ci_sql_fits[sizeof ci_sql < 0x2000 - 64 ? 1 : -1];
static void ci_bg_nop(void *self) { (void)self; }
static void ci_bg_save(void *self)                      /* saver thread */
{
    (void)self;
    void *db = CI_DB_HANDLE();
    CI_DB_EXEC(db, CI_CREATE);
    for (int id = 0; id < 2 * CI_PATS + 2; id++) {
        char *o = sq_cat(ci_sql, "INSERT OR REPLACE INTO circuitseq VALUES (");
        *o++ = (char)('0' + id); *o++ = ','; *o++ = '\'';
        o += id < 2 * CI_PATS ? ci_serialize(id / CI_PATS, id % CI_PATS, o) : ci_serialize_state(id - 2 * CI_PATS, o);
        *o++ = '\''; *o++ = ')'; *o = 0;
        op1_db_write(db, ci_sql);                       /* (no assert: long statement, docs/17) */
    }
}
static const uint32_t ci_bg_vt[6] = { 0, 0, 0, (uint32_t)ci_bg_nop, 0, (uint32_t)ci_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } ci_bg;
static uint8_t ci_bg_ready;
static void ci_changed(void) { if (ci_bg_ready) CI_BG_DIRTY(&ci_bg); }
static void ci_load(void)
{
    void *db = CI_DB_HANDLE();
    CI_DB_EXEC(db, CI_CREATE);
    for (int id = 0; id < 2 * CI_PATS + 2; id++) {
        static char q[] = "SELECT data FROM circuitseq WHERE id=0";
        q[sizeof q - 2] = (char)('0' + id);
        ci_dstr s = { 0 };
        CI_DB_GET(&s, db, q);
        const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
        if (s.size && s.size <= CI_TEXT_LEN) {           /* (heap copy, once per boot, not freed) */
            if (id < 2 * CI_PATS) ci_parse(id / CI_PATS, id % CI_PATS, p, (int)s.size);
            else ci_parse_state(id - 2 * CI_PATS, p, (int)s.size);
        }
    }
    CI_BG_REGISTER(&ci_bg);
    ci_bg.vt = ci_bg_vt;
    ci_bg_ready = 1;
}
void *ci_player_get(void)
{
    if (!ci_bg_ready) ci_load();
    return &ci_player;
}

static void ci_scr_init(void *scr) { (void)scr; }
static void ci_scr_update(void *scr, int dt) { ci_screen_update(scr, dt, CI_MODE()); }
const op1_screen_t SCREEN_circuit = { ci_scr_init, ci_scr_update };

/* plugin interface (docs/44): lift / drop / scissors, encoder presses, REC; turns (Shift passed on) */
static int ci_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code == 0x0A || code == 0x0B) { if (pressed) ci_clip(m, code == 0x0B, shift); return 1; }
    if (code == 0x0C) { if (pressed) ci_delete(m, shift); return 1; }
    if (knob >= 0) { if (pressed) ci_press(m, knob); return 1; }
    if (code == 0x21) { if (pressed) ci_rec_key(m); return 1; }
    return 0;
}
static void ci_pturn(int m, int knob, int delta, int shift) { ci_turn(m, knob, delta, shift); }
const op1_seq_t SEQ_circuit = { ci_active, ci_player_get, ci_pkey, ci_pturn, ci_midi_start, 0 };
#endif
