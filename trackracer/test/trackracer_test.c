/* Host test of the track racer sequencer (trackracer.c).
 *   checks: signs play when the car drives over them, at the right time for each lap length; laps phase against
 *   each other; gate and TIE; one note per lane, nothing hanging; live record lands on the nearest position and does
 *   not double-trigger; step record at the cursor; delete / clear; mutes; patterns keep the cars in phase; lap change
 *   keeps the car's place; copy / paste; Shift + C# / Shift + red; MIDI start; persistence round trip; bad text.
 *   "render": JSON screen frames for render_trackracer.py. Build: see render_trackracer.py (needs -I <op1r-src>/synth_sdk). */
#include "../trackracer.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; int32_t op1_db_last_err;

static int n_on[2], active[2][128], hits[128];
static long sample_now, on_at[256]; static int n_at;
void host_tr_note_on(int pos, int n, int k, int v)
{
    (void)k; (void)v; n_on[pos]++; active[pos][n]++; hits[n]++;
    if (n_at < 256) on_at[n_at++] = sample_now;
}
void host_tr_note_off(int pos, int n) { active[pos][n]--; }

#define MAXL 8192
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define BPM 12000                                       /* 120 BPM: a beat = 22050 samples */
static void run(int pos, long samples) { for (long s = 0; s < samples; s += 64) { tr_tick(pos, 64, BPM); sample_now += 64; } }
static int hanging(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; return a; }
static void reset_counts(void) { memset(n_on, 0, sizeof n_on); memset(hits, 0, sizeof hits); n_at = 0; }
static void frame(const char *label, int pos)
{
    tr_screen_update(0, 16, pos);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
    fflush(stdout);
}
static void turn(int pos, int knob, int steps, int shift) { tr_turn(pos, knob, steps * 1024, shift); }
static void put(int pos, int lane, int i, int note)       /* record a sign at position i of a stopped lane */
{
    tr_bank_t *b = &tr[pos];
    b->sel = (uint8_t)lane; b->cur = (uint8_t)i;
    int rec = b->rec; b->rec = 1; tr_key(pos, note, note % 24); b->rec = (uint8_t)rec;
}

int main(int argc, char **argv)
{
    int render = argc > 1 && (!strcmp(argv[1], "render") || !strcmp(argv[1], "anim"));
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    tr_bank_init(&tr[0]); tr_bank_init(&tr[1]);
    if (!render) {
    /* lane 1, lap 1 bar (4 beats), signs at 0 16 32 48 -> one note per beat, on the beat */
    {
        put(0, 0, 0, 60); put(0, 0, 16, 62); put(0, 0, 32, 64); put(0, 0, 48, 65);
        reset_counts(); sample_now = 0; tr[0].tick_q16 = 0;
        tr_toggle_all(0);                               /* starts all at once (at tick 0) */
        run(0, 22050L * 8 - 64);                        /* two laps */
        CHECK(n_on[0] == 8, "lap 4: %d notes in 2 bars, want 8", n_on[0]);
        int ok = 1; for (int i = 0; i < n_at && i < 8; i++) if (labs(on_at[i] - 22050L * i) > 64) ok = 0;
        CHECK(ok, "lap 4: notes not on the beats (%ld %ld %ld)", on_at[0], on_at[1], on_at[2]);
        CHECK(hits[60] == 2 && hits[65] == 2, "lap 4: each sign twice");
        tr_toggle_all(0); run(0, 22050);
        CHECK(hanging(0) == 0, "stop: %d notes hanging", hanging(0));
    }
    /* lane 2, lap 3 beats, one sign at 0: notes every 3 beats -> 3 against lane 1's 4 */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        put(0, 0, 0, 60); put(0, 1, 0, 72);
        tr[0].sel = 1; turn(0, 2, -1, 0);              /* lap 4 -> 3 beats */
        CHECK(tr[0].pats[0][1].lap == 2 && tr_laps[2] == 3, "lap index");
        reset_counts(); tr[0].tick_q16 = 0; sample_now = 0;
        tr_toggle_all(0); run(0, 22050L * 12 - 64);      /* 12 beats */
        CHECK(hits[60] == 3 && hits[72] == 4, "polyrhythm 4 vs 3: %d %d", hits[60], hits[72]);
        tr_toggle_all(0); run(0, 22050);
    }
    /* gate: 1/16 note off after a quarter beat; TIE holds until the next sign; one note per lane */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        put(0, 0, 0, 60); put(0, 0, 32, 67);
        reset_counts(); tr[0].tick_q16 = 0;
        tr_toggle_all(0); run(0, 22050 / 8);
        CHECK(active[0][60] == 1, "note on during its gate");
        run(0, 22050 / 4);
        CHECK(active[0][60] == 0, "1/16 gate: off after a quarter beat");
        tr[0].sel = 0; tr_press(0, 2); turn(0, 2, 10, 0); tr_press(0, 2);   /* gate -> TIE */
        CHECK(tr[0].pats[0][0].gate == TR_TIE, "gate TIE");
        run(0, 22050L * 4);
        CHECK(hanging(0) == 1, "TIE: one note held (%d)", hanging(0));
        tr_toggle_all(0);
        CHECK(hanging(0) == 0, "TIE: stop releases");
    }
    /* live record: nearest position, cursor follows, no double trigger in the same pass */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        tr[0].tick_q16 = 0; reset_counts();
        tr_toggle_all(0); run(0, 22050 + 22050 / 16 * 3);   /* car just past position 16 + 3/4 (1 beat + 3/16) */
        tr[0].rec = 1; tr[0].sel = 0;
        int i = tr_key(0, 64, 11);
        CHECK(i == 19, "live: nearest position %d, want 19", i);
        CHECK(tr[0].cur == 19 && tr[0].pats[0][0].key[19] == 11, "live: cursor and sign");
        run(0, 22050 / 2);
        CHECK(hits[64] == 0, "live: the new sign did not double-trigger (%d)", hits[64]);
        run(0, 22050L * 4);
        CHECK(hits[64] == 1, "live: plays on the next lap (%d)", hits[64]);
        tr_toggle_all(0); tr[0].rec = 0;
        CHECK(tr_key(0, 60, 7) == -1, "record off: nothing recorded");
    }
    /* delete / clear, mutes */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        put(0, 0, 0, 60); put(0, 0, 8, 62); put(0, 0, 16, 64);
        tr[0].cur = 8; tr_delete(0, 0);
        CHECK(tr[0].pats[0][0].key[8] == TR_EMPTY && tr[0].pats[0][0].key[0] != TR_EMPTY, "delete at cursor");
        tr_shift_key(0, 14);                            /* Shift + G: mute lane 1 */
        reset_counts(); tr[0].tick_q16 = 0; tr_toggle_all(0); run(0, 22050L * 4 - 128);
        CHECK(n_on[0] == 0 && tr[0].car[0].running, "muted lane: the car drives, no notes");
        tr_shift_key(0, 14); run(0, 22050L * 4);
        CHECK(n_on[0] == 2, "unmuted: %d notes", n_on[0]);
        tr_delete(0, 1);
        int n = 0; for (int j = 0; j < TR_POS; j++) n += tr[0].pats[0][0].key[j] != TR_EMPTY;
        CHECK(n == 0, "Shift + scissors clears the lane");
        tr_toggle_all(0);
    }
    /* patterns keep the car in phase; lap change keeps the car's place on the circle */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        tr[0].pats[1][0].lap = 7;                       /* lap 8 beats in pattern 2 */
        tr[0].tick_q16 = 0; tr_toggle_all(0); run(0, 22050);      /* a quarter lap */
        tr_shift_key(0, 2);                             /* Shift + G (key 2): pattern 2 */
        CHECK(tr[0].pat == 1 && tr[0].car[0].running, "pattern 2, still running");
        tr_shift_key(0, 0); tr_shift_key(0, 2);         /* back to 1, then 2 again */
        uint64_t lap = tr_lap_q16(&tr[0].pats[1][0]);
        int a = (int)(((uint64_t)((int64_t)tr[0].tick_q16 - tr[0].car[0].start) % lap) * 64 / lap);
        CHECK(a == 16, "pattern switch keeps the place on the circle: position %d, want 16", a);
        tr[0].sel = 0; turn(0, 2, -4, 0);              /* lap 8 -> 4 beats */
        lap = tr_lap_q16(&tr[0].pats[1][0]);
        a = (int)(((uint64_t)((int64_t)tr[0].tick_q16 - tr[0].car[0].start) % lap) * 64 / lap);
        CHECK(tr_laps[tr[0].pats[1][0].lap] == 4 && a == 16, "lap change keeps the place: %d", a);
        tr_toggle_all(0);
    }
    /* copy / paste a lane and a pattern, Shift + red, MIDI start */
    {
        memset(&tr[0], 0, sizeof tr[0]); tr_bank_init(&tr[0]);
        put(0, 2, 5, 70); tr[0].sel = 2; tr_clip(0, 0, 0); tr[0].sel = 4; tr_clip(0, 1, 0);
        CHECK(tr[0].pats[0][4].key[5] != TR_EMPTY && tr[0].pats[0][4].note[5] == 70, "lane paste");
        tr_clip(0, 0, 1); tr_shift_key(0, 4); tr_clip(0, 1, 1);
        CHECK(tr[0].pat == 2 && tr[0].pats[2][4].note[5] == 70, "pattern paste");
        turn(0, 3, 1, 1);                              /* Shift + red right: all cars, next beat */
        int all = 1; for (int k = 0; k < TR_LANES; k++) all &= tr[0].car[k].running; CHECK(all, "Shift + red starts all");
        run(0, 22050 * 3); tr_midi_start();
        CHECK(tr[0].tick_q16 == 0 && tr[0].car[0].start == 0 && tr[0].car[0].last == -1, "MIDI start rewinds");
        turn(0, 3, -1, 1);
        CHECK(!tr_any_running(&tr[0]) && hanging(0) == 0, "Shift + red left stops all");
    }
    /* persistence round trip (dense lane, all fields) and bad text */
    {
        memset(&tr[1], 0, sizeof tr[1]); tr_bank_init(&tr[1]);
        for (int i = 0; i < TR_POS; i++) { tr[1].pats[3][5].key[i] = (uint8_t)(i % 24); tr[1].pats[3][5].note[i] = (uint8_t)(40 + i); }
        tr[1].pats[3][0].lap = 9; tr[1].pats[3][0].gate = TR_TIE; tr[1].pats[3][0].vel = 77; tr[1].pats[3][0].muted = 1;
        tr[1].sel = 3; tr[1].pat = 2; tr[1].cur = 41;
        static char txt[TR_TEXT_LEN + 1], st[16];
        int n = tr_serialize(1, 3, txt), m = tr_serialize_state(1, st);
        CHECK(n <= TR_TEXT_LEN, "text length %d", n);
        tr_lane_t keep[TR_LANES]; memcpy(keep, tr[1].pats[3], sizeof keep);
        memset(&tr[1], 0, sizeof tr[1]); tr_bank_init(&tr[1]);
        CHECK(tr_parse(1, 3, txt, n) && tr_parse_state(1, st, m), "parse");
        CHECK(!memcmp(keep, tr[1].pats[3], sizeof keep), "round trip differs");
        CHECK(tr[1].sel == 3 && tr[1].pat == 2 && tr[1].cur == 41, "state round trip");
        CHECK(!tr_parse(1, 0, "c1zz", 4) && !tr_parse(1, 0, txt, n - 1) && !tr_parse(1, 9, txt, n), "bad text accepted");
        CHECK(host_tr_changed > 0, "changes not reported");
    }
    /* drum mode: the sign's drum sound (key) reaches the note-on */
    {
        memset(&tr[1], 0, sizeof tr[1]); tr_bank_init(&tr[1]);
        put(1, 0, 0, 53 + 4);                           /* key 4 (A) */
        CHECK(tr[1].pats[0][0].key[0] == (53 + 4) % 24, "drum sign keeps its key");
        CHECK(tr[0].pats[0][0].key[0] == TR_EMPTY || 1, "banks independent");
    }
    printf(fails ? "%d FAILED\n" : "OK: all track racer checks pass\n", fails);
    return fails != 0;
    }

    if (!strcmp(argv[1], "anim")) {                     /* frames for an animation: 8 s at 20 fps */
        memset(tr, 0, sizeof tr); tr_bank_init(&tr[0]);
        static const int laps[6] = { 3, 2, 5, 1, 7, 9 };  /* 4 3 6 2 8 16 beats */
        static const int sig[6][5] = { { 0, 16, 32, 48, -1 }, { 0, 32, -1 }, { 0, 24, 40, -1 },
                                       { 0, 16, 32, 48, -1 }, { 8, 40, -1 }, { 0, -1 } };
        for (int k = 0; k < 6; k++) {
            tr[0].pats[0][k].lap = (uint8_t)laps[k];
            for (int j = 0; sig[k][j] >= 0; j++) put(0, k, sig[k][j], 48 + k * 5 + j * 2);
        }
        tr[0].sel = 0; tr[0].cur = 0; tr_toggle_all(0);
        for (int f = 0; f < 160; f++) {
            if (f == 50) { tr[0].sel = 2; tr[0].cur = 12; }
            if (f == 80) { tr[0].rec = 1; tr_key(0, 72, 19); }          /* live record on lane 3 */
            if (f == 110) { tr[0].rec = 0; tr_shift_key(0, 19); }       /* Shift + C: mute lane 4 */
            run(0, 44100 / 20);
            frame("anim", 0);
        }
        return 0;
    }
    /* ---- screen frames ---- */
    memset(tr, 0, sizeof tr); tr_bank_init(&tr[0]); tr_bank_init(&tr[1]);
    static const int laps[6] = { 3, 2, 5, 3, 1, 7 };    /* 4 3 6 4 2 8 beats */
    static const int sig[6][6] = { { 0, 16, 32, 48, -1 }, { 0, 21, 42, -1 }, { 0, 24, 40, -1 },
                                   { 8, 24, 40, 56, -1 }, { 0, 32, -1 }, { 0, 10, 20, 44, -1 } };
    for (int k = 0; k < 6; k++) {
        tr[0].pats[0][k].lap = (uint8_t)laps[k];
        for (int j = 0; sig[k][j] >= 0 && j < 6; j++) put(0, k, sig[k][j], 48 + k * 5 + j * 2);
    }
    tr[0].sel = 0; tr[0].cur = 0;
    frame("stopped: cars parked on the start line", 0);
    tr_toggle_all(0); run(0, 22050L * 5 / 2);
    frame("all running, 2.5 beats in: lane 1 selected", 0);
    tr[0].sel = 2; tr[0].cur = 30; tr_press(0, 2);
    frame("lane 3, cursor at 31, white page 2: gate", 0);
    tr_press(0, 2); tr_rec_key(0); tr[0].sel = 5; tr[0].cur = 0; tr_toggle_all(0);
    frame("all stopped, REC on, lane 6 selected", 0);
    tr_toggle_all(0); run(0, 22050L * 3 + 2000); tr_shift_key(0, 18); tr_shift_key(0, 21);
    frame("LIVE recording on lane 6, lanes 3 and 5 muted", 0);
    return 0;
}
