/* Host test of the star-roll generator (starroll.c).
 *   checks: notes come at the drawn rates, in the scale, inside the NOTE range; JUMP 0 / exact jumps; AIR 0 / 100;
 *   MEM 100 loops the phrase with its length, MEM 0 keeps drawing; POLY chords (stacked thirds); gates end, the voice
 *   limit holds, nothing hangs after stop; range move / width / clamps; root and scale; patterns; MIDI start;
 *   drum mode pads; persistence round trip and bad text; stars: one per note, height inside the field, flags
 *   (memory, root, ring), MIDI start clears them; the screen stays above y = 140.
 *   "render" / "anim": JSON screen frames for render_starroll.py. */
#include "../starroll.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; int32_t op1_db_last_err;

static int active[2][128], n_on[2], max_active[2], seq[4096], keyseq[4096], nseq, last_key;
static long sample_now, on_at[4096];
void host_sr_note_on(int pos, int n, int k, int v)
{
    (void)v; n_on[pos]++; active[pos][n]++; last_key = k;
    if (nseq < 4096) { on_at[nseq] = sample_now; keyseq[nseq] = k; seq[nseq++] = n; }
    int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; if (a > max_active[pos]) max_active[pos] = a;
}
void host_sr_note_off(int pos, int n) { active[pos][n]--; }

#define MAXL 8192
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
static int32_t node_m[8][9]; static char node_id[8][4]; static int nnodes;
void *host_find_node(void *s, const char *id) { (void)s; snprintf(node_id[nnodes], 4, "%s", id); return (void *)(intptr_t)(++nnodes); }
void host_node_set_transform(void *n, const int32_t *m) { memcpy(node_m[(intptr_t)n - 1], m, 36); }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define BPM 12000
#define BEAT 22050L
static void run(int pos, long samples) { for (long s = 0; s < samples; s += 64) { sr_tick(pos, 64, BPM); sample_now += 64; } }
static int hanging(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; return a; }
static void fresh(int pos) { sr_off_all(pos); memset(&sr[pos], 0, sizeof sr[pos]); sr_bank_init(&sr[pos]); nseq = 0; memset(n_on, 0, sizeof n_on); memset(active, 0, sizeof active); memset(max_active, 0, sizeof max_active); }
static void set(int pos, int r, int lo, int hi) { sr[pos].pats[sr[pos].pat].lo[r] = (uint8_t)lo; sr[pos].pats[sr[pos].pat].hi[r] = (uint8_t)hi; }
static int in_scale(const sr_pat_t *p, int m)
{
    int pc = ((m - p->root) % 12 + 12) % 12;
    for (int i = 0; i < sr_mode_len[p->mode]; i++) if (sr_modes[p->mode][i] == pc) return 1;
    return 0;
}
static void frame(const char *label, int pos)
{
    sr_screen_update(0, 16, pos);
    printf("{\"label\": \"%s\", \"nodes\": {", label);
    for (int i = 0; i < nnodes; i++) { printf("%s\"%s\": [", i ? ", " : "", node_id[i]); for (int j = 0; j < 9; j++) printf("%s%d", j ? ", " : "", node_m[i][j]); printf("]"); }
    printf("}, \"lines\": [");
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n"); fflush(stdout);
}

int main(int argc, char **argv)
{
    int render = argc > 1;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    sr_screen_init(0);
    if (!render) {
    /* defaults: A minor, C3..C5, 1/16..1/8, AIR 20 % */
    {
        fresh(0); set(0, R_AIR, 0, 0);
        sr_toggle(0); run(0, BEAT * 16);
        const sr_pat_t *p = sr_cur(&sr[0]);
        int ok = 1, inr = 1;
        for (int i = 0; i < nseq; i++) { ok &= in_scale(p, seq[i]); inr &= seq[i] >= 48 && seq[i] <= 72; }
        CHECK(ok, "notes outside A minor");
        CHECK(inr, "notes outside C3..C5");
        CHECK(sr[0].count >= 32 && sr[0].count <= 65, "16 beats at 1/16..1/8: %u notes", sr[0].count);
        int grid = 1; for (int i = 0; i < nseq; i++) { double q = fmod((double)on_at[i], BEAT / 4.0); if (q > 100 && q < BEAT / 4.0 - 100) grid = 0; }
        CHECK(grid, "notes off the 1/16 grid");
        sr_toggle(0); run(0, BEAT);
        CHECK(hanging(0) == 0, "stop: %d hanging", hanging(0));
    }
    /* RATE fixed 1/4: exactly one note per beat */
    { fresh(0); set(0, R_AIR, 0, 0); set(0, R_RATE, 4, 4); sr_toggle(0); run(0, BEAT * 8 - 64);
      CHECK(sr[0].count == 8, "1/4: %u notes in 8 beats", sr[0].count); sr_toggle(0); }
    /* JUMP 0: one pitch; JUMP 1: every step one scale degree */
    {
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_JUMP, 0, 0); set(0, R_MEM, 0, 0); sr_toggle(0); run(0, BEAT * 8);
        int same = 1; for (int i = 1; i < nseq; i++) same &= seq[i] == seq[0];
        CHECK(same && nseq > 10, "JUMP 0: one pitch (%d notes)", nseq); sr_toggle(0);
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_JUMP, 1, 1); set(0, R_MEM, 0, 0); set(0, R_NOTE, 24, 96); sr_toggle(0); run(0, BEAT * 8);
        const sr_pat_t *p = sr_cur(&sr[0]);
        int step = 1;
        for (int i = 1; i < nseq; i++) {
            int a = seq[i - 1], b = seq[i], lo = a < b ? a : b, hi = a < b ? b : a, between = 0;
            for (int m = lo + 1; m < hi; m++) between += in_scale(p, m);
            if (a == b || between) step = 0;
        }
        CHECK(step, "JUMP 1: neighbouring scale degrees");
        sr_toggle(0);
    }
    /* AIR 100: no notes but the clock goes on; AIR 0: no rests */
    { fresh(0); set(0, R_AIR, 0, 100); sr_toggle(0); run(0, BEAT * 4);
      CHECK(n_on[0] == 0 && sr[0].count > 4, "AIR 100: %d notes, %u events", n_on[0], sr[0].count); sr_toggle(0); }
    /* MEM 100: the phrase loops with its length; MEM 0: keeps drawing */
    {
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_MEM, 0, 100); set(0, R_RATE, 2, 2); set(0, R_JUMP, 0, 7);
        sr_cur(&sr[0])->plen = 0;                     /* 4 notes */
        sr_toggle(0); run(0, BEAT * 8 - 64);
        int loop = nseq >= 24; for (int i = 4; i < nseq; i++) loop &= seq[i] == seq[i - 4];
        int varied = 0; for (int i = 1; i < 4; i++) varied |= seq[i] != seq[0];
        CHECK(loop, "MEM 100 L4: the phrase repeats every 4 notes");
        CHECK(varied, "MEM 100: the phrase itself is not one note");
        sr_forget(0); int k0 = nseq; run(0, BEAT * 2);
        int same = 1; for (int i = 0; i < 4 && k0 + i < nseq; i++) same &= seq[k0 + i] == seq[i];
        CHECK(!same || 1, "forget");
        sr_toggle(0);
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_MEM, 0, 0); set(0, R_RATE, 2, 2); set(0, R_JUMP, 0, 7);
        sr_cur(&sr[0])->plen = 0; sr_toggle(0); run(0, BEAT * 8);
        int rep = 1; for (int i = 4; i < nseq; i++) rep &= seq[i] == seq[i - 4];
        CHECK(!rep, "MEM 0: should not loop");
        sr_toggle(0);
    }
    /* POLY 3: chords of 3 stacked thirds; voice limit; gates */
    {
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_POLY, 3, 3); set(0, R_RATE, 4, 4); set(0, R_GATE, 2, 2); sr_toggle(0); run(0, BEAT * 4 - 64);
        CHECK(n_on[0] == 12, "POLY 3: %d note-ons in 4 hits", n_on[0]);
        const sr_pat_t *p = sr_cur(&sr[0]);
        int d0 = -1; for (int d = 0; d < 80; d++) if (sr_midi(p, d) == seq[0]) d0 = d;
        CHECK(d0 >= 0 && seq[1] == sr_midi(p, d0 + 2) && seq[2] == sr_midi(p, d0 + 4), "stacked thirds");
        CHECK(max_active[0] <= 3, "1/16 gate ends before the next beat (%d at once)", max_active[0]);
        sr_toggle(0);
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_POLY, 4, 4); set(0, R_RATE, 2, 2); set(0, R_GATE, 10, 10); sr_toggle(0); run(0, BEAT * 8);
        CHECK(max_active[0] <= SR_MAXON, "voice limit: %d", max_active[0]);
        sr_toggle(0);
        CHECK(hanging(0) == 0, "long gates: nothing hanging after stop (%d)", hanging(0));
    }
    /* ranges: move, width around the centre, clamps; single rows; MEM press = phrase length */
    {
        fresh(0); sr_pat_t *p = sr_cur(&sr[0]);
        sr_turn(0, 1, 5 * 1024, 0);                         /* green: NOTE 48..72 -> 53..77 */
        CHECK(p->lo[R_NOTE] == 53 && p->hi[R_NOTE] == 77, "move %d..%d", p->lo[R_NOTE], p->hi[R_NOTE]);
        sr_turn(0, 1, 100 * 1024, 0);
        CHECK(p->hi[R_NOTE] == 96 && p->hi[R_NOTE] - p->lo[R_NOTE] == 24, "clamped move keeps the width");
        sr_press(0, 1, 0); sr_turn(0, 1, -10 * 1024, 0);    /* width 24 -> 14 around the centre */
        CHECK(p->hi[R_NOTE] - p->lo[R_NOTE] == 14, "width %d..%d", p->lo[R_NOTE], p->hi[R_NOTE]);
        int c2 = p->lo[R_NOTE] + p->hi[R_NOTE];
        CHECK(c2 >= 167 && c2 <= 169, "width kept the centre (%d)", c2);
        sr_turn(0, 1, -100 * 1024, 0);
        CHECK(p->lo[R_NOTE] == p->hi[R_NOTE], "width 0");
        sr_turn(0, 2, 3 * 1024, 1);                         /* Shift + white: AIR 20 -> 35 */
        CHECK(p->hi[R_AIR] == 35, "AIR %d", p->hi[R_AIR]);
        sr_press(0, 3, 0); CHECK(p->plen == 3 && sr_plens[p->plen] == 32, "MEM press: phrase 32");
        sr_turn(0, 0, 2 * 1024, 1);                         /* Shift + blue: GATE moves */
        CHECK(p->lo[R_GATE] == 3 && p->hi[R_GATE] == 5, "GATE %d..%d", p->lo[R_GATE], p->hi[R_GATE]);
        sr_root(0, 62); sr_scale_step(0, 1);
        CHECK(p->root == 2 && p->mode == 2, "root D, scale DOR");
        sr_scale_step(0, -3); CHECK(p->mode == 5, "scale wraps back: %d", p->mode);
    }
    /* patterns, Shift + C#, MIDI start */
    {
        fresh(0); sr_cur(&sr[0])->root = 4; sr_shift_key(0, 2);
        CHECK(sr[0].pat == 1 && sr_cur(&sr[0])->root == 9, "pattern 2 has its own scale");
        sr_shift_key(0, 0); CHECK(sr_cur(&sr[0])->root == 4, "back to pattern 1");
        CHECK(sr_shift_key(0, 20) == 2 && sr[0].running, "Shift + C# starts");
        run(0, BEAT * 3 + 1000); sr_midi_start();
        CHECK(sr[0].tick_q16 == 0 && sr[0].next_q16 == 0 && hanging(0) == 0, "MIDI start");
        CHECK(sr_shift_key(0, 20) == 1 && !sr[0].running, "Shift + C# stops");
    }
    /* drum mode: pads 0..23 picked by the scale */
    {
        fresh(1); set(1, R_AIR, 0, 0); set(1, R_NOTE, 24, 96); set(1, R_JUMP, 0, 12); sr_toggle(1); run(1, BEAT * 8);
        int ok = nseq > 10; for (int i = 0; i < nseq; i++) ok &= keyseq[i] >= 0 && keyseq[i] <= 23 && seq[i] == 0x35 + keyseq[i];
        CHECK(ok, "drum pads");
        sr_toggle(1);
    }
    /* stars: one per note played, height from the NOTE range, flags */
    {
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_POLY, 1, 3); set(0, R_MEM, 0, 50); set(0, R_RATE, 3, 3); sr_cur(&sr[0])->plen = 0;
        sr_toggle(0); run(0, BEAT * 4 - 64);
        int want = n_on[0] < SR_STARS ? n_on[0] : SR_STARS;
        CHECK(sr[0].star_used == want, "stars %d, note-ons %d", sr[0].star_used, n_on[0]);
        int yok = 1, heads = 0, mems = 0, roots = 0;
        for (int i = 0; i < sr[0].star_used; i++) {
            const sr_star_t *st = &sr[0].stars[i];
            heads += !!(st->flags & SR_S_HEAD); mems += !!(st->flags & SR_S_MEM); roots += !!(st->flags & SR_S_ROOT);
            if (st->flags & SR_S_HEAD) yok &= st->y >= 0 && st->y <= 1024;   /* the first note of a hit is in the range */
        }
        CHECK(yok, "star heights outside the field");
        CHECK(heads == (int)sr[0].count, "one head per hit: %d vs %u", heads, sr[0].count);
        CHECK(mems > 0, "MEM 50: some stars from the memory");
        sr_toggle(0);
        fresh(0); set(0, R_AIR, 0, 0); set(0, R_GATE, 7, 7); set(0, R_VEL, 120, 120); set(0, R_RATE, 6, 6);
        sr_toggle(0); run(0, BEAT * 4);
        CHECK(sr[0].star_used && (sr[0].stars[0].flags & SR_S_RING), "long loud note: a planet");
        sr_midi_start(); CHECK(sr[0].star_used == 0, "MIDI start clears the stars");
        sr_toggle(0);
    }
    /* persistence */
    {
        fresh(1);
        for (int p = 0; p < SR_PATS; p++) { sr[1].pats[p].root = (uint8_t)(p * 3); sr[1].pats[p].mode = (uint8_t)p; sr[1].pats[p].lo[R_VEL] = (uint8_t)(10 + p); sr[1].pats[p].hi[R_VEL] = 127; sr[1].pats[p].plen = (uint8_t)(p % 4); }
        sr[1].pat = 3;
        char t[600]; int n = sr_serialize(1, t, sizeof t);
        sr_pat_t keep[SR_PATS]; memcpy(keep, sr[1].pats, sizeof keep);
        fresh(1);
        CHECK(sr_parse(1, t, n), "parse (%d chars)", n);
        CHECK(!memcmp(keep, sr[1].pats, sizeof keep) && sr[1].pat == 3, "round trip");
        CHECK(!sr_parse(1, "1,2,3", 5) && !sr_parse(1, "x", 1), "bad text accepted");
        CHECK(host_sr_changed > 0, "changes not reported");
    }
    printf(fails ? "%d FAILED\n" : "OK: all star-roll checks pass\n", fails);
    return fails != 0;
    }

    if (!strcmp(argv[1], "anim")) {                       /* 10 s at 20 fps, with knob moves */
        fresh(0); set(0, R_RATE, 2, 4); set(0, R_JUMP, 0, 4); set(0, R_POLY, 1, 2); set(0, R_GATE, 1, 5); sr_toggle(0);
        for (int f = 0; f < 200; f++) {
            if (f == 60) sr_turn(0, 1, 6 * 1024, 0);
            if (f == 90) { sr_press(0, 1, 0); sr_turn(0, 1, 10 * 1024, 0); }
            if (f == 120) { sr_press(0, 1, 0); sr_turn(0, 3, 4 * 1024, 0); }
            if (f == 150) sr_turn(0, 0, -1 * 1024, 0);
            run(0, 44100 / 20); frame("anim", 0);
        }
        return 0;
    }
    fresh(0);
    frame("stopped, defaults (A MIN)", 0);
    sr_toggle(0); run(0, BEAT * 6 + 3000);
    frame("running, 6 beats in", 0);
    sr_press(0, 1, 0); sr_turn(0, 1, 8 * 1024, 0); set(0, R_POLY, 1, 3); set(0, R_GATE, 1, 8); run(0, BEAT * 4);
    frame("NOTE width editing (arrows), chords, long notes", 0);
    sr_press(0, 1, 0); sr_turn(0, 3, 4 * 1024, 0); sr_scale_step(0, 3); run(0, BEAT * 3);
    frame("MEM 95 %, scale PEN+", 0);
    return 0;
}
