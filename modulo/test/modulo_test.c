/* Host test of the modulo step sequencer (modulo.c), sanitizers on.
 *   checks: timing of every step size; polyrhythm 16 / 7 / 5 (hit counts, steps in the right places, the rows meet
 *   again after lcm); length change while playing (playhead = steps since PLAY mod length, no gap / no double);
 *   step size change does not retrigger; accent / volume velocities; gate = half a step; nothing left hanging after
 *   STOP; mutes (Shift keys, red press, all); patterns; step entry (cursor moves on, the key becomes the sound); live
 *   record (nearest step, no double trigger); green press cycles; scissors / Shift; copy / paste; MIDI start;
 *   drum demo pattern; persistence round trip and bad text; the screen stays above y = 140.
 *   "render" / "anim": JSON screen frames for render_modulo.py. */
#include "../modulo.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; int32_t op1_db_last_err;

static int active[2][128], hits[128], n_on[2], lastvel[128];
static long sample_now, first_at[128], times[128][512]; static int ntimes[128];
void host_md_note_on(int pos, int n, int k, int v)
{
    (void)k; n_on[pos]++; active[pos][n]++; hits[n]++; lastvel[n] = v;
    if (!first_at[n]) first_at[n] = sample_now + 1;
    if (ntimes[n] < 512) times[n][ntimes[n]++] = sample_now;
}
void host_md_note_off(int pos, int n) { active[pos][n]--; }

#define MAXL 8192
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return (void *)1; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define BPM 12000                                        /* 120 BPM: a beat = 22050 samples, a 1/16 = 5512.5 */
#define BEAT 22050L
static void run(int pos, long samples) { for (long s = 0; s < samples; s += 64) { md_tick(pos, 64, BPM); sample_now += 64; } }
static int hanging(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; return a; }
static void counts(void) { memset(hits, 0, sizeof hits); memset(first_at, 0, sizeof first_at); memset(ntimes, 0, sizeof ntimes); memset(n_on, 0, sizeof n_on); }
static void fresh(int pos)
{
    md_all_off(pos); memset(&md[pos], 0, sizeof md[pos]); md_bank(pos);
    if (pos) for (int k = 0; k < MD_TRACKS; k++) for (int i = 0; i < MD_STEPS; i++) md[pos].pats[0][k].step[i] = 0;
    counts(); sample_now = 0;
}
static md_track_t *T(int pos, int k) { return &md[pos].pats[md[pos].pat][k]; }
static void turn(int pos, int knob, int st, int shift) { md_turn(pos, knob, st * 1024, shift); }

static void frame(const char *label, int pos)
{
    md_screen_update(0, 16, pos);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n"); fflush(stdout);
}
static int lowest_ok(void)
{
    for (int i = 0; i < nlines; i++) {
        int32_t yb = (lines[i].y0 > lines[i].y1 ? lines[i].y0 : lines[i].y1) + lines[i].w / 2;
        if (yb > OP1_Q10(140)) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    if (argc < 2) {
    /* timing of every step size: a step on every position of a 4-step track */
    for (int r = 0; r < MD_NRES; r++) {
        fresh(0);
        md_track_t *t = T(0, 0); t->len = 4; t->res = (uint8_t)r; t->note = 60;
        for (int i = 0; i < 4; i++) t->step[i] = 1;
        md_toggle(0); run(0, BEAT * 4);
        double step = BEAT * md_res[r] / 96.0;
        int exp = (int)(BEAT * 4 / step + 0.999), ok = 1;
        for (int j = 0; j < ntimes[60]; j++) if (fabs(times[60][j] - j * step) > 130) ok = 0;   /* 64-sample blocks */
        CHECK(abs(hits[60] - exp) <= 1 && ok, "%s: %d notes (want %d), on time %d", md_res_names[r], hits[60], exp, ok);
        md_toggle(0);
        CHECK(hanging(0) == 0, "%s: notes hanging after STOP", md_res_names[r]);
    }
    /* polyrhythm 16 / 7 / 5, step 0 only: hits = ceil(steps / len); they meet again after lcm steps */
    {
        fresh(0);
        int len[3] = { 16, 7, 5 }, note[3] = { 36, 38, 42 };
        for (int k = 0; k < 3; k++) { T(0, k)->len = (uint8_t)len[k]; T(0, k)->note = (uint8_t)note[k]; T(0, k)->step[0] = 1; }
        long steps = 16 * 7 * 5;
        md_toggle(0); run(0, (long)(steps * 5512.5) + 1000);   /* (+ the host clock's rounding per block) */
        for (int k = 0; k < 3; k++) CHECK(hits[note[k]] == (int)(steps / len[k]) + 1, "len %d: %d hits, want %ld", len[k], hits[note[k]], steps / len[k] + 1);
        long last[3]; for (int k = 0; k < 3; k++) last[k] = times[note[k]][ntimes[note[k]] - 1];
        CHECK(labs(last[0] - last[1]) < 130 && labs(last[1] - last[2]) < 130 && last[0] > 0, "16/7/5 meet again after 560 steps (%ld %ld %ld)", last[0], last[1], last[2]);
        CHECK(labs(times[38][1] - (long)(7 * 5512.5)) < 130, "7-step track: second hit at step 7");
        md_toggle(0);
    }
    /* length change while playing: playhead = steps since PLAY mod length */
    {
        fresh(0);
        md_track_t *t = T(0, 0); t->len = 8; t->note = 60; t->step[0] = 1; t->step[3] = 1;
        md_toggle(0); run(0, (long)(10.5 * 5512.5));     /* step 10 = index 2 */
        CHECK(md[0].head[0].hit == 2, "playhead at %d, want 2", md[0].head[0].hit);
        md[0].sel = 0; turn(0, 2, -5, 0);                /* length 3: step 11 -> 11 mod 3 = 2, then 12 -> 0 */
        CHECK(t->len == 3, "length 3");
        counts(); run(0, (long)(2 * 5512.5));
        CHECK(md[0].head[0].hit == 0 && hits[60] == 1, "after the change: index %d, %d hits", md[0].head[0].hit, hits[60]);
        turn(0, 2, 40, 0); CHECK(t->len == 32, "length clamps at 32");
        turn(0, 2, -40, 0); CHECK(t->len == 1 && md[0].cur == 0, "length clamps at 1, cursor follows");
        md_toggle(0);
    }
    /* step size change while playing: no retrigger, continues at the new size */
    {
        fresh(0);
        md_track_t *t = T(0, 0); t->len = 1; t->note = 60; t->step[0] = 1;
        md_toggle(0); run(0, (long)(3.5 * 5512.5));
        counts();
        md_press(0, 2); turn(0, 2, 1, 0); md_press(0, 2);  /* page 2: 1/16 -> 1/8 */
        CHECK(t->res == 2 && md[0].page2 == 0, "step size 1/8");
        run(0, 2000);                                     /* the next 1/8 boundary is step 4 of 1/16: 2756 later */
        CHECK(hits[60] == 0, "size change retriggered (%d)", hits[60]);
        run(0, BEAT * 2);
        CHECK(hits[60] == 4, "1/8 after the change: %d hits in 2 beats", hits[60]);
        CHECK(labs(times[60][0] - (sample_now - BEAT * 2 - 2000 + 2756)) < 130, "first 1/8 on its boundary");
        md_toggle(0);
    }
    /* velocities, gate */
    {
        fresh(0);
        md_track_t *t = T(0, 0); t->len = 2; t->note = 60; t->step[0] = 1; t->step[1] = 2; t->vol = 80;
        md_toggle(0); run(0, 1000);
        CHECK(lastvel[60] == 80 && active[0][60] == 1, "normal step: vel %d", lastvel[60]);
        run(0, 2756 - 1000 + 200);
        CHECK(active[0][60] == 0, "gate: off after half a step");
        run(0, 5512 - 2756);
        CHECK(lastvel[60] == 112, "accent: vel %d, want 112", lastvel[60]);
        md[0].sel = 0; turn(0, 3, 10, 0); CHECK(t->vol == 120, "volume +4 per step: %d", t->vol);
        run(0, 5512 * 2);
        CHECK(lastvel[60] == 127, "accent clamps at 127: %d", lastvel[60]);
        md_toggle(0); CHECK(hanging(0) == 0, "STOP releases");
    }
    /* mutes: Shift + E (11) = track 1, red press = selected track, Shift + D# = all */
    {
        fresh(0);
        for (int k = 0; k < 3; k++) { T(0, k)->len = 4; T(0, k)->note = (uint8_t)(60 + k); T(0, k)->step[0] = 1; }
        CHECK(md_shift_key(0, 11) == 1 && T(0, 0)->muted, "Shift + E mutes track 1");
        md[0].sel = 1; md_press(0, 3); CHECK(T(0, 1)->muted, "red press mutes the selected track");
        md_toggle(0); run(0, BEAT * 2);
        CHECK(hits[60] == 0 && hits[61] == 0 && hits[62] == 2, "muted tracks silent: %d %d %d", hits[60], hits[61], hits[62]);
        md_shift_key(0, 22); int all = 1; for (int k = 0; k < MD_TRACKS; k++) all &= T(0, k)->muted; CHECK(all, "Shift + D# mutes all");
        md_shift_key(0, 22); all = 0; for (int k = 0; k < MD_TRACKS; k++) all |= T(0, k)->muted; CHECK(!all, "Shift + D# again unmutes all");
        CHECK(md_shift_key(0, 23) == 1 && T(0, 7)->muted, "Shift + high E mutes track 8");
        md_toggle(0);
        CHECK(md_shift_key(0, 20) == 2 && md[0].running, "Shift + C# plays");
        CHECK(md_shift_key(0, 20) == 1 && !md[0].running, "Shift + C# stops");
        CHECK(md_press(0, 0) == 2 && md[0].running, "blue press plays");
        md_press(0, 0); CHECK(!md[0].running && hanging(0) == 0, "blue press stops");
        CHECK(md_shift_key(0, 1) == 0, "Shift + F# not a shortcut");
    }
    /* patterns */
    {
        fresh(0);
        T(0, 0)->step[0] = 1; T(0, 0)->note = 60;
        md_shift_key(0, 4);                              /* Shift + A: pattern 3 */
        CHECK(md[0].pat == 2 && T(0, 0)->step[0] == 0, "pattern 3 is its own");
        md_shift_key(0, 0); CHECK(md[0].pat == 0 && T(0, 0)->step[0] == 1, "back to pattern 1");
    }
    /* step entry: the key becomes the sound, cursor moves on (wraps at the length); green press cycles */
    {
        fresh(1);
        md[1].sel = 2; T(1, 2)->len = 3; md_rec_key(1);
        CHECK(md_key(1, 57, 4) == 0 && md_key(1, 57, 4) == 1, "step entry at the cursor");
        CHECK(md[1].cur == 2 && T(1, 2)->key == 4 && T(1, 2)->note == 57, "cursor and sound");
        md_key(1, 57, 4); CHECK(md[1].cur == 0, "cursor wraps at the length");
        md[1].cur = 1; md_cycle_step(1); CHECK(T(1, 2)->step[1] == 2, "green press: on -> accent");
        md_cycle_step(1); CHECK(T(1, 2)->step[1] == 0, "accent -> off");
        md_press(1, 1); CHECK(T(1, 2)->step[1] == 1, "off -> on");
        md_delete(1, 0); CHECK(T(1, 2)->step[1] == 0 && T(1, 2)->step[0] == 1, "scissors: the step at the cursor");
        md_delete(1, 1); CHECK(!T(1, 2)->step[0] && !T(1, 2)->step[2], "Shift + scissors: the track");
        md_rec_key(1); CHECK(md_key(1, 60, 7) == -1, "REC off: nothing recorded");
        md[1].cur = 0; turn(1, 1, -1, 0); CHECK(md[1].cur == 2, "cursor wraps backwards: %d", md[1].cur);
        turn(1, 1, 1, 1); CHECK(md[1].cur == (2 + 4) % 3, "Shift + green: 4 steps");
        turn(1, 0, -1, 0); CHECK(md[1].sel == 1, "track select");
        turn(1, 0, -2, 0); CHECK(md[1].sel == 7, "track select wraps");
    }
    /* live record: nearest step, no double trigger */
    {
        fresh(0);
        T(0, 0)->len = 16; md[0].sel = 0;
        md_toggle(0); run(0, (long)(5.8 * 5512.5));      /* step 5, 80 % of the way to 6 */
        md_rec_key(0);
        int i = md_key(0, 64, 11);
        CHECK(i == 6 && T(0, 0)->step[6] == 1 && md[0].cur == 6, "live: nearest step %d, want 6", i);
        run(0, 5512);
        CHECK(hits[64] == 0, "live: no double trigger (%d)", hits[64]);
        run(0, 5512L * 16);
        CHECK(hits[64] == 1, "live: plays on the next pass (%d)", hits[64]);
        md_rec_key(0); md_toggle(0);
    }
    /* copy / paste, MIDI start */
    {
        fresh(0);
        T(0, 2)->step[5] = 2; T(0, 2)->len = 9; md[0].sel = 2; md_clip(0, 0, 0);
        md[0].sel = 4; md_clip(0, 1, 0);
        CHECK(T(0, 4)->step[5] == 2 && T(0, 4)->len == 9, "track paste");
        md_clip(0, 0, 1); md_shift_key(0, 6); md_clip(0, 1, 1);
        CHECK(md[0].pat == 3 && T(0, 4)->step[5] == 2, "pattern paste");
        md_toggle(0); run(0, BEAT * 3); md_midi_start();
        CHECK(md[0].tick_q16 == 0 && md[0].start == 0 && md[0].head[0].last == -1, "MIDI start rewinds");
        md_toggle(0);
    }
    /* drum demo: 16-step four on the floor, 7-step snare, 5-step hat */
    {
        memset(&md[1], 0, sizeof md[1]); md_bank(1); counts(); sample_now = 0;
        CHECK(md[1].pats[0][0].len == 16 && md[1].pats[0][1].len == 7 && md[1].pats[0][2].len == 5, "drum demo lengths");
        md_toggle(1); run(1, (long)(16 * 5512.5));
        CHECK(hits[53] == 4 && hits[55] == 2 && hits[57] == 10, "demo: kick %d snare %d hat %d", hits[53], hits[55], hits[57]);
        md_toggle(1);
    }
    /* persistence round trip (all fields) and bad text */
    {
        fresh(1);
        for (int p = 0; p < MD_PATS; p++) for (int k = 0; k < MD_TRACKS; k++) {
            md_track_t *t = &md[1].pats[p][k];
            t->len = (uint8_t)(1 + (p * 8 + k) % 32); t->res = (uint8_t)((p + k) % MD_NRES); t->vol = (uint8_t)(1 + k * 15);
            t->muted = (uint8_t)(k & 1); t->note = (uint8_t)(30 + k + p); t->key = (uint8_t)((k * 3) % 24);
            for (int i = 0; i < MD_STEPS; i++) t->step[i] = (uint8_t)((i * 7 + k + p) % 3);
        }
        md[1].sel = 5; md[1].pat = 2; md[1].cur = 3;
        static char txt[MD_TEXT_LEN + 1];
        int n = md_serialize(1, txt, sizeof txt);
        CHECK(n == MD_TEXT_LEN, "text length %d", n);
        static md_track_t keep[MD_PATS][MD_TRACKS]; memcpy(keep, md[1].pats, sizeof keep);
        memset(&md[1], 0, sizeof md[1]); md_bank(1);
        CHECK(md_parse(1, txt, n), "parse");
        CHECK(!memcmp(keep, md[1].pats, sizeof keep), "round trip differs");
        CHECK(md[1].sel == 5 && md[1].pat == 2 && md[1].cur == 3, "state round trip");
        CHECK(!md_parse(1, "m1zz", 4) && !md_parse(1, txt, n - 1), "bad text accepted");
        txt[20] = 'x'; CHECK(!md_parse(1, txt, n), "bad digit accepted");
        CHECK(md_serialize(1, txt, 100) == 0, "short buffer");
        CHECK(host_md_changed > 0, "changes not reported");
    }
    /* screen: every page / state stays above y = 140 */
    {
        int low = 1;
        for (int v = 0; v < 4; v++) {
            fresh(v & 1);
            for (int k = 0; k < MD_TRACKS; k++) { T(v & 1, k)->len = 32; T(v & 1, k)->res = 5; for (int i = 0; i < 32; i++) T(v & 1, k)->step[i] = (uint8_t)((i + k) % 3); }
            md[v & 1].page2 = (uint8_t)(v >> 1); md[v & 1].rec = 1; T(v & 1, 0)->muted = 1;
            md_toggle(v & 1); run(v & 1, 30000);
            md_screen_update(0, 16, v & 1); low &= lowest_ok();
            md_toggle(v & 1);
        }
        CHECK(low, "screen draws below y = 140");
    }
    printf(fails ? "%d FAILED\n" : "OK: all modulo checks pass\n", fails);
    return fails != 0;
    }

    if (!strcmp(argv[1], "render")) {
        memset(md, 0, sizeof md); md_bank(1); md_bank(0);
        frame("drum mode, pattern 1 (demo 16 / 7 / 5), stopped", 1);
        md_toggle(1); run(1, (long)(37 * 5512.5)); frame("playing: each row at its own step", 1);
        md_track_t *t = md[1].pats[0];
        t[3].len = 12; t[3].res = 4; for (int i = 0; i < 12; i += 3) t[3].step[i] = 1;
        t[4].len = 32; t[4].res = 0; for (int i = 0; i < 32; i += 5) t[4].step[i] = (uint8_t)(i % 2 + 1);
        t[5].len = 3; t[5].res = 2; t[5].step[0] = 2; t[6].len = 11; t[6].step[4] = 1; t[6].step[9] = 1; t[6].muted = 1;
        t[7].len = 6; t[7].res = 5; t[7].step[0] = 1; t[7].step[3] = 1;
        md[1].sel = 4; md[1].cur = 10; md[1].rec = 1; run(1, 5512);
        frame("8 tracks: 1/32, triplets, muted track 7; REC, cursor", 1);
        md[1].page2 = 1; frame("white page 2: step size", 1);
        return 0;
    }
    if (!strcmp(argv[1], "anim")) {                     /* 8 s at 20 fps */
        memset(md, 0, sizeof md); md_bank(1);
        md_track_t *t = md[1].pats[0];
        t[3].len = 12; t[3].res = 4; for (int i = 0; i < 12; i += 3) t[3].step[i] = 1;
        t[4].len = 9; t[4].step[0] = 2; t[4].step[4] = 1; t[5].len = 3; t[5].res = 2; t[5].step[0] = 1;
        md_toggle(1);
        for (int f = 0; f < 160; f++) { run(1, 2205); frame("anim", 1); }
        return 0;
    }
    return 2;
}
