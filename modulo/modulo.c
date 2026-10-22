/* "modulo": a polyrhythmic step sequencer for op1r (synth and drum mode).
 *
 * 8 tracks, each one sound (drum mode: a drum key of the kit, kick / snare / hat...; synth mode: a note) on a row of
 * steps. Every track has its own length (1..32 steps) and its own step size (1/32 1/16 1/8 1/4 and triplets 1/16T
 * 1/8T); its playhead is (steps since PLAY) mod length, so a 16-step kick, a 7-step snare and a 5-step hat drift
 * against each other and meet again every 16 * 7 * 5 steps. A step is off, on (the track volume) or accented
 * (volume + 32). Notes last half a step.
 *
 * Controls (modulo screen shown; Shift = the shift key held):
 *   blue turn       select track 1..8 (wraps)          press: PLAY / STOP (all tracks, from step 1)
 *   green turn      cursor along the track (Shift: 4 steps)   press: the step under the cursor off -> on -> accent
 *   white turn      length 1..32                        press: page 2 = step size (1/32 1/16 1/8 1/4 1/16T 1/8T)
 *   red turn        volume 1..127                       press: mute / unmute the track
 *   REC             record on / off
 *   note key        REC on, stopped: the key becomes the track's sound, the step at the cursor is set, cursor + 1
 *                   REC on, playing: live, the key becomes the track's sound and the nearest step is set (it does
 *                   not play again in the same pass); REC off: just plays
 *   scissors        clear the step under the cursor; Shift: clear the track
 *   lift / drop     copy / paste the track (steps, length, size, volume, sound); Shift: the whole pattern
 * Any screen while modulo is the mode's sequencer (as euclid / dots / track racer):
 *   Shift + F G A B (keys 0 2 4 6)                       pattern 1..4 (playheads keep running)
 *   Shift + E F G A B C D E (11 12 14 16 18 19 21 23)    mute track 1..8; Shift + D# (22) mute all / none
 *   Shift + C# (20)                                      PLAY / STOP; with REC held (tape armed) also tape PLAY
 * Saved (sq_store, DB table moduloseq, one row per mode): 4 patterns x 8 tracks, selected track, pattern, cursor.
 * Comes back stopped.
 *
 * Clock: 96 ticks per beat (Q16) at the OP-1 tempo, advanced by the player tick. MIDI START restarts from step 1.
 * Platform independent (host test: test/modulo_test.c); firmware glue at the end.
 * Copyright (C) 2026 Manuel Montanari. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "seq/seq_text.h"

#define MD_VERSION "V1.0"
#define MD_TRACKS  8
#define MD_STEPS   32
#define MD_PATS    4
#define MD_PPQN    96
#define MD_NRES    6
#define MD_ACCENT  32                                   /* velocity added by an accent */
static const uint8_t md_res[MD_NRES] = { 12, 24, 48, 96, 16, 32 };            /* ticks per step (96 per beat) */
static const char *const md_res_names[MD_NRES] = { "1/32", "1/16", "1/8", "1/4", "1/16T", "1/8T" };
static const char *const md_res_short[MD_NRES] = { "32", "16", "8", "4", "16T", "8T" };

typedef struct {                                        /* a track (saved, per pattern) */
    uint8_t step[MD_STEPS];                             /* 0 off, 1 on, 2 accent */
    uint8_t len, res, vol, muted, note, key;            /* res: index into md_res; note: pitch, key: 0..23 */
} md_track_t;

typedef struct {                                        /* a playhead (live, not saved) */
    int64_t  last;                                      /* step count (since PLAY) last played, -1 none */
    int64_t  skip;                                      /* live record: do not play this step count (-1 none) */
    uint8_t  sounding;                                  /* note id + 1, 0 none */
    uint64_t off_at;                                    /* tick (Q16) of the note-off */
    int8_t   hit;                                       /* step index last played (screen) */
    uint32_t hit_tick;                                  /* tick of that hit (96 ppqn) */
} md_head_t;

typedef struct {
    md_track_t pats[MD_PATS][MD_TRACKS];
    md_head_t  head[MD_TRACKS];
    uint8_t    init, sel, pat, cur, rec, running;
    uint8_t    page2;                                   /* white page 2: step size */
    int32_t    acc[4];                                  /* encoder accumulators (one step per 1024) */
    uint64_t   tick_q16, start;                         /* clock; tick of step 0 */
} md_bank_t;

static md_bank_t md[2];                                 /* 0 synth, 1 drum */
static md_track_t md_clip_track[2], md_clip_pat[2][MD_TRACKS];
static uint8_t md_clip_full[2][2];

#ifdef OP1_HOST
int host_md_changed;
void host_md_note_on(int pos, int note, int key, int vel);
void host_md_note_off(int pos, int note);
#define MD_CHANGED()             (host_md_changed++)
#define MD_NOTE_ON(pos, n, k, v) (host_md_note_on((pos), (n), (k), (v)), (n))
#define MD_NOTE_OFF(pos, n)      host_md_note_off((pos), (n))
#else
static void md_changed(void);
#define MD_CHANGED()             md_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);    /* core (seq/seq_core.c), shared by the sequencers */
void eu_fw_note_off(int midi);
#define MD_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define MD_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#endif

static int md_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void md_copy_track(md_track_t *d, const md_track_t *s)      /* no memcpy in a plugin */
{
    volatile uint8_t *o = (volatile uint8_t *)d;
    const uint8_t *i = (const uint8_t *)s;
    for (unsigned n = 0; n < sizeof *d; n++) o[n] = i[n];
}

/* defaults: drum mode keys F G A B C D E F (the low half of a kit: kick, snare, hats, ...), synth mode C3 D3 E3 G3
 * A3 C4 D4 E4. Drum pattern 1 is a demo of the idea: 16-step four on the floor, 7-step snare, 5-step hat. */
static const uint8_t md_dkeys[MD_TRACKS] = { 0, 2, 4, 6, 7, 9, 11, 12 };
static const uint8_t md_snotes[MD_TRACKS] = { 48, 50, 52, 55, 57, 60, 62, 64 };
static void md_track_default(md_track_t *t, int pos, int k)
{
    for (int i = 0; i < MD_STEPS; i++) t->step[i] = 0;
    t->len = 16; t->res = 1; t->vol = 96; t->muted = 0;
    if (pos) { t->key = md_dkeys[k]; t->note = (uint8_t)(0x35 + md_dkeys[k]); }
    else { t->note = md_snotes[k]; t->key = (uint8_t)md_clamp(md_snotes[k] - 0x35, 0, 23); }
}
static void md_bank_init(md_bank_t *b, int pos)
{
    if (b->init) return;
    b->init = 1; b->sel = 0; b->pat = 0; b->cur = 0; b->rec = 0; b->running = 0;
    for (int p = 0; p < MD_PATS; p++) for (int k = 0; k < MD_TRACKS; k++) md_track_default(&b->pats[p][k], pos, k);
    if (pos) {
        md_track_t *t = b->pats[0];
        for (int i = 0; i < 16; i += 4) t[0].step[i] = i ? 1 : 2;
        t[1].len = 7; t[1].step[3] = 1;
        t[2].len = 5; t[2].step[0] = 2; t[2].step[2] = 1; t[2].step[3] = 1;
    }
    for (int k = 0; k < MD_TRACKS; k++) { b->head[k].last = -1; b->head[k].skip = -1; b->head[k].hit = -1; }
}
static md_bank_t *md_bank(int pos) { md_bank_t *b = &md[pos & 1]; md_bank_init(b, pos & 1); return b; }
static md_track_t *md_track(md_bank_t *b, int k) { return &b->pats[b->pat][k]; }
static uint64_t md_step_q16(const md_track_t *t) { return (uint64_t)md_res[t->res] << 16; }
static int md_started(const md_bank_t *b) { return b->running && b->tick_q16 >= b->start; }

static void md_silence(int pos, md_head_t *h)
{
    if (h->sounding) { MD_NOTE_OFF(pos, h->sounding - 1); h->sounding = 0; }
}

/* ---- the clock ---- */
static void md_play(int pos, md_bank_t *b, int k, int64_t n)
{
    md_head_t *h = &b->head[k];
    const md_track_t *t = md_track(b, k);
    int i = (int)(n % t->len);
    h->hit = (int8_t)i; h->hit_tick = (uint32_t)(b->tick_q16 >> 16);
    if (h->skip == n) { h->skip = -1; return; }          /* just recorded live (it already sounded) */
    if (!t->step[i] || t->muted) return;
    md_silence(pos, h);
    int vel = t->step[i] == 2 ? md_clamp(t->vol + MD_ACCENT, 1, 127) : t->vol;
    int id = MD_NOTE_ON(pos, t->note, t->key, vel);
    if (id >= 0) h->sounding = (uint8_t)(id + 1);
    uint64_t gate = md_step_q16(t) / 2;
    h->off_at = b->tick_q16 + (gate > (1u << 16) ? gate : (1u << 16));
}
static void md_advance(int pos, uint64_t inc)
{
    md_bank_t *b = md_bank(pos);
    uint64_t t = b->tick_q16;
    for (int k = 0; k < MD_TRACKS; k++) {
        md_head_t *h = &b->head[k];
        if (h->sounding && t >= h->off_at) md_silence(pos & 1, h);
        if (!b->running || t < b->start) continue;
        int64_t n = (int64_t)((t - b->start) / md_step_q16(md_track(b, k)));   /* steps since PLAY */
        if (n != h->last) { h->last = n; md_play(pos & 1, b, k, n); }
    }
    b->tick_q16 = t + inc;
}
void md_tick(int pos, int n, uint32_t bpm_x100)          /* host: advance by n samples at bpm_x100 */
{
    md_advance(pos, ((uint64_t)bpm_x100 * MD_PPQN << 16) * (uint64_t)n / (6000ull * 44100ull));
}

/* ---- play / stop ---- */
void md_all_off(int pos)
{
    for (int k = 0; k < MD_TRACKS; k++) md_silence(pos & 1, &md[pos & 1].head[k]);
}
/* PLAY / STOP: returns 1 if it started */
int md_toggle(int pos)
{
    md_bank_t *b = md_bank(pos);
    if (b->running) { b->running = 0; md_all_off(pos); for (int k = 0; k < MD_TRACKS; k++) b->head[k].hit = -1; return 0; }
    b->running = 1; b->start = b->tick_q16;
    for (int k = 0; k < MD_TRACKS; k++) { b->head[k].last = -1; b->head[k].skip = -1; }
    return 1;
}

/* ---- patterns, mutes ---- */
void md_recall(int pos, int p)
{
    md_bank_t *b = md_bank(pos);
    if (p < 0 || p >= MD_PATS || p == b->pat) return;
    md_all_off(pos);
    b->pat = (uint8_t)p;
    if (b->cur >= md_track(b, b->sel)->len) b->cur = 0;
    MD_CHANGED();
}
void md_mute(int pos, int k)
{
    md_bank_t *b = md_bank(pos);
    if (k < 0 || k >= MD_TRACKS) return;
    md_track_t *t = md_track(b, k);
    t->muted ^= 1;
    if (t->muted) md_silence(pos & 1, &b->head[k]);
    MD_CHANGED();
}
void md_mute_all(int pos)                               /* all muted -> unmute all, else mute all */
{
    md_bank_t *b = md_bank(pos);
    int all = 1;
    for (int k = 0; k < MD_TRACKS; k++) if (!md_track(b, k)->muted) all = 0;
    for (int k = 0; k < MD_TRACKS; k++) md_track(b, k)->muted = (uint8_t)!all;
    if (!all) md_all_off(pos);
    MD_CHANGED();
}
/* Shift + key. Returns 0 if not handled, 2 if C# started, else 1. */
int md_shift_key(int pos, int key)
{
    static const int8_t act[24] = { 20, -1, 21, -1, 22, -1, 23, -1, -1, -1, -1, 0,
                                    1, -1, 2, -1, 3, -1, 4, 5, 8, 6, 9, 7 };
    if (key < 0 || key > 23 || act[key] < 0) return 0;
    if (act[key] >= 20) { md_recall(pos, act[key] - 20); return 1; }
    if (act[key] == 8) return md_toggle(pos) ? 2 : 1;
    if (act[key] == 9) md_mute_all(pos); else md_mute(pos, act[key]);
    return 1;
}

/* ---- steps ---- */
/* note key on the modulo screen with REC on: the key becomes the track's sound and a step is set (stopped: at the
 * cursor, which moves on; playing: the nearest step to the playhead). Returns the step written, -1 if none. The key
 * is never consumed: it sounds as usual. */
int md_key(int pos, int note, int key)
{
    md_bank_t *b = md_bank(pos);
    if (!b->rec) return -1;
    md_track_t *t = md_track(b, b->sel);
    md_head_t *h = &b->head[b->sel];
    t->note = (uint8_t)md_clamp(note, 0, 127); t->key = (uint8_t)md_clamp(key, 0, 23);
    int i;
    if (md_started(b)) {                                 /* live: nearest step */
        uint64_t st = md_step_q16(t);
        int64_t n = (int64_t)((b->tick_q16 - b->start + st / 2) / st);
        i = (int)(n % t->len);
        if (n > h->last) h->skip = n;                    /* still ahead: do not play it again this pass */
        if (!t->step[i]) t->step[i] = 1;
        b->cur = (uint8_t)i;
    } else {
        i = b->cur;
        if (!t->step[i]) t->step[i] = 1;
        b->cur = (uint8_t)((i + 1) % t->len);
    }
    MD_CHANGED();
    return i;
}
void md_rec_key(int pos) { md_bank_t *b = md_bank(pos); b->rec ^= 1; }
void md_cycle_step(int pos)                              /* green press: off -> on -> accent -> off */
{
    md_bank_t *b = md_bank(pos);
    md_track_t *t = md_track(b, b->sel);
    t->step[b->cur] = (uint8_t)((t->step[b->cur] + 1) % 3);
    MD_CHANGED();
}
void md_delete(int pos, int all)                         /* scissors: the step at the cursor; Shift: the track */
{
    md_bank_t *b = md_bank(pos);
    md_track_t *t = md_track(b, b->sel);
    if (all) { for (int i = 0; i < MD_STEPS; i++) t->step[i] = 0; md_silence(pos & 1, &b->head[b->sel]); }
    else t->step[b->cur] = 0;
    MD_CHANGED();
}
/* lift / drop: copy / paste the selected track (all = 1: the whole pattern). One clipboard of each per mode. */
void md_clip(int pos, int paste, int all)
{
    md_bank_t *b = md_bank(pos);
    int m = pos & 1, a = all != 0;
    if (!paste) {
        if (a) for (int k = 0; k < MD_TRACKS; k++) md_copy_track(&md_clip_pat[m][k], md_track(b, k));
        else md_copy_track(&md_clip_track[m], md_track(b, b->sel));
        md_clip_full[m][a] = 1;
        return;
    }
    if (!md_clip_full[m][a]) return;
    for (int k = a ? 0 : b->sel; k < (a ? MD_TRACKS : b->sel + 1); k++) {
        md_silence(m, &b->head[k]);
        md_copy_track(md_track(b, k), a ? &md_clip_pat[m][k] : &md_clip_track[m]);
    }
    if (b->cur >= md_track(b, b->sel)->len) b->cur = 0;
    MD_CHANGED();
}

/* ---- encoders ---- */
static int md_steps(md_bank_t *b, int knob, int delta)
{
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    b->acc[knob] -= st * 1024;
    return st;
}
/* encoder press: blue PLAY / STOP (returns 2 if it started), green step, white page 2, red mute */
int md_press(int pos, int knob)
{
    md_bank_t *b = md_bank(pos);
    switch (knob) {
    case 0: return md_toggle(pos) ? 2 : 1;
    case 1: md_cycle_step(pos); break;
    case 2: b->page2 ^= 1; break;
    case 3: md_mute(pos, b->sel); break;
    }
    return 1;
}
void md_turn(int pos, int knob, int delta, int shift)
{
    md_bank_t *b = md_bank(pos);
    if (knob < 0 || knob > 3) return;
    int st = md_steps(b, knob, delta);
    if (!st) return;
    md_track_t *t = md_track(b, b->sel);
    switch (knob) {
    case 0:
        b->sel = (uint8_t)(((b->sel + st) % MD_TRACKS + MD_TRACKS) % MD_TRACKS);
        if (b->cur >= md_track(b, b->sel)->len) b->cur = (uint8_t)(md_track(b, b->sel)->len - 1);
        return;
    case 1:
        b->cur = (uint8_t)(((b->cur + st * (shift ? 4 : 1)) % t->len + t->len) % t->len);
        return;
    case 2:
        if (b->page2) {
            t->res = (uint8_t)md_clamp(t->res + st, 0, MD_NRES - 1);
            if (md_started(b))                           /* count steps at the new size from here, no retrigger */
                b->head[b->sel].last = (int64_t)((b->tick_q16 - b->start) / md_step_q16(t));
        } else {
            t->len = (uint8_t)md_clamp(t->len + st, 1, MD_STEPS);
            if (b->cur >= t->len) b->cur = (uint8_t)(t->len - 1);
        }
        break;
    case 3:
        t->vol = (uint8_t)md_clamp(t->vol + st * 4, 1, 127);
        break;
    }
    MD_CHANGED();
}

/* MIDI START: from step 1 on the next clock tick (the master's downbeat) */
void md_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        md_bank_t *b = &md[pos];
        b->tick_q16 = 0; b->start = 0;
        for (int k = 0; k < MD_TRACKS; k++) { md_silence(pos, &b->head[k]); b->head[k].last = -1; b->head[k].skip = -1; }
    }
}

/* ---- persistence text (hex, no libc), one row per mode ----
 * "m1" sel pat cur (2 hex each), then per pattern and track: len res vol muted note key (2 hex each) and the 32
 * steps, 2 bits each, as 16 hex digits (step 0 in the first digit's high bits). 8 + 32 * 28 = 904 chars. */
#define MD_TEXT_LEN (8 + MD_PATS * MD_TRACKS * 28)
static const char md_hexd[] = "0123456789ABCDEF";
static char *md_hex(char *p, int v) { *p++ = md_hexd[(v >> 4) & 15]; *p++ = md_hexd[v & 15]; return p; }
static int md_unhex1(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static int md_unhex(const char *s) { int a = md_unhex1(s[0]), c = md_unhex1(s[1]); return a < 0 || c < 0 ? -1 : a * 16 + c; }
int md_serialize(int pos, char *out, int n)
{
    md_bank_t *b = md_bank(pos);
    if (n < MD_TEXT_LEN + 1) { out[0] = 0; return 0; }
    char *o = out;
    *o++ = 'm'; *o++ = '1';
    o = md_hex(md_hex(md_hex(o, b->sel), b->pat), b->cur);
    for (int p = 0; p < MD_PATS; p++) for (int k = 0; k < MD_TRACKS; k++) {
        const md_track_t *t = &b->pats[p][k];
        o = md_hex(md_hex(md_hex(md_hex(md_hex(md_hex(o, t->len), t->res), t->vol), t->muted), t->note), t->key);
        for (int i = 0; i < MD_STEPS; i += 2) *o++ = md_hexd[(t->step[i] & 3) << 2 | (t->step[i + 1] & 3)];
    }
    *o = 0;
    return (int)(o - out);
}
int md_parse(int pos, const char *s, int len)
{
    static md_track_t tmp[MD_PATS][MD_TRACKS];          /* (not on the stack: 1.2 KB) */
    if (len != MD_TEXT_LEN || s[0] != 'm' || s[1] != '1') return 0;
    int sel = md_unhex(s + 2), pat = md_unhex(s + 4), cur = md_unhex(s + 6), o = 8;
    if (sel < 0 || pat < 0 || cur < 0) return 0;
    for (int p = 0; p < MD_PATS; p++) for (int k = 0; k < MD_TRACKS; k++) {
        md_track_t *t = &tmp[p][k];
        int v[6];
        for (int j = 0; j < 6; j++) { v[j] = md_unhex(s + o); o += 2; if (v[j] < 0) return 0; }
        t->len = (uint8_t)md_clamp(v[0], 1, MD_STEPS); t->res = (uint8_t)md_clamp(v[1], 0, MD_NRES - 1);
        t->vol = (uint8_t)md_clamp(v[2], 1, 127); t->muted = (uint8_t)(v[3] & 1);
        t->note = (uint8_t)md_clamp(v[4], 0, 127); t->key = (uint8_t)md_clamp(v[5], 0, 23);
        for (int i = 0; i < MD_STEPS; i += 2) {
            int d = md_unhex1(s[o++]);
            if (d < 0) return 0;
            t->step[i] = (uint8_t)((d >> 2) % 3); t->step[i + 1] = (uint8_t)((d & 3) % 3);
        }
    }
    md_bank_t *b = md_bank(pos);
    for (int p = 0; p < MD_PATS; p++) for (int k = 0; k < MD_TRACKS; k++) md_copy_track(&b->pats[p][k], &tmp[p][k]);
    b->sel = (uint8_t)md_clamp(sel, 0, MD_TRACKS - 1); b->pat = (uint8_t)md_clamp(pat, 0, MD_PATS - 1);
    b->cur = (uint8_t)md_clamp(cur, 0, md_track(b, b->sel)->len - 1);
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Eight rows of steps, one per track (only its length is drawn, so the rows end at different places: the
 * polyrhythm is the picture), in groups of four. Off = a dim dot, on = a block in the track's colour, accent = a
 * taller, brighter block. Each row's playhead: a white frame on its current step (the step flashes white when it
 * plays). Left: track number in its colour (selected: with a marker; muted: dark, crossed out); right: step size.
 * Under the selected row's cursor: a bar, red while recording. Bottom line (encoder colours): track + sound, step,
 * length or SIZE (page 2), volume or MUTE; top right: pattern, PLAY, REC. Everything at y <= 138 (the OP-1 hides the
 * bottom rows). The SVG (modulo.svg) is empty; all raster lines (stroke text: seq_text.h). */
#define MD_X0   18                                       /* first step */
#define MD_Y0   20                                       /* first row centre */
#define MD_DY   12
static const uint8_t md_col[MD_TRACKS][4] = {
    { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF },
    { 0xFF, 0x9A, 0x3C, 0xFF }, { 0xB9, 0x8C, 0xFF, 0xFF }, { 0x3C, 0xDC, 0xE6, 0xFF }, { 0xF5, 0xD7, 0x3C, 0xFF } };
static const uint8_t md_knob[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                       { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t md_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t md_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t md_dark[4]  = { 0x3A, 0x3C, 0x3E, 0xFF };
static const uint8_t md_red[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };

static void md_dim(uint8_t *o, const uint8_t *c, int l)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
static int32_t md_x(int i) { return OP1_Q10(MD_X0 + i * 8 + (i / 4) * 2); }      /* left edge of step i */
static void md_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (const char *s = nm[n % 12]; *s; ) *p++ = *s++;
    int o = n / 12 - 1;
    if (o < 0) { *p++ = '-'; o = -o; }
    *p++ = (char)('0' + o % 10); *p = 0;
}

static void md_screen_draw(void *r, int pos)
{
    md_bank_t *b = md_bank(pos);
    uint32_t now = (uint32_t)(b->tick_q16 >> 16);
    uint8_t c[4];
    char s[24], *p;

    sq_text(r, OP1_Q10(2), OP1_Q10(2), OP1_Q10(1), MD_VERSION, 0x280, md_grey);
    sq_text(r, OP1_Q10(34), OP1_Q10(2), OP1_Q10(1), "MODULO", 0x300, md_grey);
    s[0] = 'P'; s[1] = (char)('1' + b->pat); s[2] = 0;
    sq_text(r, OP1_Q10(220), OP1_Q10(2), OP1_Q10(1), s, 0x300, md_white);
    if (b->running) sq_text(r, OP1_Q10(242), OP1_Q10(2), OP1_Q10(1), "PLAY", 0x300, md_knob[1]);
    if (b->rec) sq_text(r, OP1_Q10(276), OP1_Q10(2), OP1_Q10(1), md_started(b) ? "LIVE" : "REC", 0x300, md_red);

    for (int k = 0; k < MD_TRACKS; k++) {
        const md_track_t *t = md_track(b, k);
        const md_head_t *h = &b->head[k];
        int sel = k == b->sel;
        int32_t y = OP1_Q10(MD_Y0 + k * MD_DY);
        /* track number */
        s[0] = (char)('1' + k); s[1] = 0;
        const uint8_t *nc = md_col[k];
        if (t->muted) { md_dim(c, md_col[k], 70); nc = c; }
        sq_text(r, OP1_Q10(7), y - OP1_Q10(3), OP1_Q10(1), s, sel ? 0x400 : 0x280, nc);
        if (t->muted) sq_line(r, OP1_Q10(5), y, OP1_Q10(13), y, 0x200, md_grey);
        if (sel) sq_line(r, OP1_Q10(1), y - OP1_Q10(3), OP1_Q10(1), y + OP1_Q10(3), 0x300, md_white);
        /* steps */
        for (int i = 0; i < t->len; i++) {
            int32_t x = md_x(i);
            int lit = h->hit == i && b->running && now - h->hit_tick < 8;
            if (!t->step[i]) {
                sq_line(r, x + OP1_Q10(2), y, x + OP1_Q10(4), y, 0x200, sel ? md_grey : md_dark);
            } else {
                const uint8_t *bc;
                if (lit && !t->muted) bc = md_white;
                else { md_dim(c, md_col[k], t->muted ? 60 : t->step[i] == 2 ? 256 : 150); bc = c; }
                int32_t hh = t->step[i] == 2 ? OP1_Q10(9) : OP1_Q10(6);
                sq_line(r, x, y, x + OP1_Q10(6), y, hh, bc);
            }
            if (h->hit == i && b->running) {             /* playhead frame */
                int32_t x0 = x - OP1_Q10(1), x1 = x + OP1_Q10(7), y0 = y - OP1_Q10(5.5), y1 = y + OP1_Q10(5.5);
                const uint8_t *fc = t->muted ? md_grey : md_white;
                sq_line(r, x0, y0, x1, y0, 0x180, fc); sq_line(r, x0, y1, x1, y1, 0x180, fc);
                sq_line(r, x0, y0, x0, y1, 0x180, fc); sq_line(r, x1, y0, x1, y1, 0x180, fc);
            }
        }
        if (sel) {                                       /* cursor */
            int32_t x = md_x(b->cur);
            sq_line(r, x, y + OP1_Q10(5.5), x + OP1_Q10(6), y + OP1_Q10(5.5), 0x200, b->rec ? md_red : md_white);
        }
        sq_text(r, OP1_Q10(294), y - OP1_Q10(3), OP1_Q10(1), md_res_short[t->res], 0x280, sel ? md_white : md_grey);
    }

    /* bottom line: what the encoders show, in their colours */
    const md_track_t *t = md_track(b, b->sel);
    int32_t Y = OP1_Q10(124), S = OP1_Q10(1.3), W = 0x380;
    p = s; *p++ = 'T'; *p++ = (char)('1' + b->sel); *p++ = ' '; md_note_name(p, t->note);
    sq_text(r, OP1_Q10(4), Y, S, s, W, md_knob[0]);
    p = sq_num(sq_cat(s, "STEP "), (uint32_t)b->cur + 1, 2);
    sq_text(r, OP1_Q10(68), Y, S, s, W, md_knob[1]);
    if (b->page2) sq_cat(sq_cat(s, "SIZE "), md_res_names[t->res]);
    else sq_num(sq_cat(s, "LEN "), t->len, 2);
    sq_text(r, OP1_Q10(152), Y, S, s, W, md_knob[2]);
    if (t->muted) sq_cat(s, "MUTE");
    else sq_num(sq_cat(s, "VOL "), t->vol, 0);
    sq_text(r, OP1_Q10(226), Y, S, s, W, md_knob[3]);
}

static void md_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    md_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "modulo", type 3 + md_active[mode], as euclid) =========
 * Player vtable as euclid's (ok_player layout): +0x14 key_down(key 0..23) -> 1 = consumed, +0x24 reset,
 * +0x2c tick (96 per beat), +0x3c stop. Screen in the finger screen slot 0x2D. */
#define MD_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define MD_SHIFT()     (*(volatile uint8_t *)0x01DD7F78)
#define MD_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define MD_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
#define MD_TAPE_ARMED() (*(volatile int *)0x018C17D4 == 3 || *(volatile int *)0x018C17D4 == 6)
extern volatile uint8_t eu_rec_play;                    /* core: start with REC held -> tape PLAY */
static int md_nop(void *self) { (void)self; return 0; }
static int md_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = MD_MODE();
    if (MD_SHIFT()) {
        int h = md_shift_key(m, key);
        if (h == 2 && MD_TAPE_ARMED()) eu_rec_play = 1;
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)            /* modulo screen shown: record (if on) */
        md_key(m, key + 0x35 + (m ? MD_DRUM_OCT() : MD_SYNTH_OCT()) * 12, key);
    return 0;                                           /* never consumed: the key sounds as usual */
}
static void md_reset(void *self) { (void)self; md_all_off(MD_MODE()); }
static void md_ptick(void *self, int counter) { (void)self; (void)counter; md_advance(MD_MODE(), 1u << 16); }   /* 1/96 beat */
static void md_pstop(void *self) { (void)self; md_all_off(0); md_all_off(1); }
static const uint32_t md_vt[16] = {
    0, 0,  0, (uint32_t)md_nop,  0, (uint32_t)md_key_down,  0, (uint32_t)md_nop,
    0, (uint32_t)md_reset,  0, (uint32_t)md_ptick,  0, (uint32_t)md_nop,  0, (uint32_t)md_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } md_player = { md_vt, { 0 } };
volatile uint8_t md_active[2];

static char md_sql[MD_TEXT_LEN + 120];
typedef char md_sql_fits[sizeof md_sql < 0x2000 - 64 ? 1 : -1];
static sq_store_t md_store = { .name = "moduloseq", .ser = md_serialize, .parse = md_parse,
                               .sql = md_sql, .cap = sizeof md_sql, .maxlen = MD_TEXT_LEN + 1 };
static void md_changed(void) { sq_store_dirty(&md_store); }
void *md_player_get(void)
{
    sq_store_load(&md_store);
    return &md_player;
}
static void md_scr_init(void *scr) { (void)scr; }
static void md_scr_update(void *scr, int dt) { md_screen_update(scr, dt, MD_MODE()); }
const op1_screen_t SCREEN_modulo = { md_scr_init, md_scr_update };

/* plugin interface (docs/44): lift / drop / scissors, encoder presses, REC; turns (Shift passed on) */
static int md_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code == 0x0A || code == 0x0B) { if (pressed) md_clip(m, code == 0x0B, shift); return 1; }
    if (code == 0x0C) { if (pressed) md_delete(m, shift); return 1; }
    if (knob >= 0) { if (pressed && md_press(m, knob) == 2 && MD_TAPE_ARMED()) eu_rec_play = 1; return 1; }
    if (code == 0x21) { if (pressed) md_rec_key(m); return 1; }
    return 0;
}
static void md_pturn(int m, int knob, int delta, int shift) { md_turn(m, knob, delta, shift); }
const op1_seq_t SEQ_modulo = { md_active, md_player_get, md_pkey, md_pturn, md_midi_start, 0 };
#endif
