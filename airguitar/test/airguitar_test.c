/* Host test of the airguitar engine (airguitar.c), sanitizers on.
 *   checks: GESTURE mode: held keys stay silent until a stroke; a stroke down plucks the strings from the lowest note
 *   to the highest, up the other way, once per stroke (the braking does not fire again); a stroke with the OP-1 held
 *   on its side still reads down / up; sensor noise and slow tilting never fire; FORCE: a light stroke fires at
 *   FORCE 0 and not at 100; GESTURE + KEYS: a key plucks at once; tuning within 4 cents for every guitar, tone and
 *   sustain, from E2 to C6; palm mute dies sooner than full sustain; drive keeps the output in range; six strings
 *   strummed together stay finite; the screen stays above y = 140 for every guitar and page.
 *   "wav <file>": a strummed demo (every guitar, a chord progression); "render" / "anim": JSON screen frames. */
#include "../airguitar.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];

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

/* ---- a little OP-1: voices with a held flag and a simple release, the sensor at 69 Hz ---- */
static int16_t *K = host_knobs[0];
static int note_of[6], on[6], rel[6];                     /* rel: release countdown (blocks) */
static long now;                                           /* samples */
static uint32_t pluck_at_n[6]; static long pluck_time[6];
static int32_t inc_of(double midi) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100); }
static void key_on(int v, int midi, int vel)
{
    note_of[v] = midi; on[v] = 1; rel[v] = 0; host_voice_vel[v] = (int16_t)(vel << 8);
    ENGINE_airguitar.note_on(host_voice_mem + v * 0x24, K);
}
static void key_off(int v) { if (on[v]) { on[v] = 0; rel[v] = 20; ENGINE_airguitar.note_off(host_voice_mem + v * 0x24); } }
static void all_off(void) { for (int v = 0; v < 6; v++) key_off(v); }
/* the sensor: a reading every 639 samples (~69 Hz); motion = acceleration profile in g along `axis` (+ = toward the
 * sensor's positive side), gravity on `gaxis` */
static double prof[4096]; static int nprof, prof_i, axis = 2, gaxis = 2; static double noise;
static uint32_t nrng = 12345; static int manual;
static void sensor_step(void)
{
    if (manual) return;
    double a[3] = { 0, 0, 0 };
    a[gaxis] = 1.0;
    if (prof_i < nprof) a[axis] += prof[prof_i++];
    for (int i = 0; i < 3; i++) {
        nrng = nrng * 1664525u + 1013904223u;
        double nz = noise * (((nrng >> 8) & 0xFFFF) / 32768.0 - 1.0);
        double x = (a[i] + nz) * 16384;                   /* the sensor saturates at +-2 g */
        host_accel[i] = (int16_t)lrint(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
    }
}
static double mixbuf[44100 * 120]; static long nmix;
static double peak_out;
static void run(long samples)
{
    for (long s = 0; s < samples; s += OP1_BLOCK) {
        if (now % 639 < OP1_BLOCK) sensor_step();
        double m[OP1_BLOCK] = { 0 };
        for (int v = 0; v < 6; v++) {
            if (!on[v] && !rel[v]) continue;
            for (int i = 0; i < OP1_BLOCK; i++) host_pitch_inc[i] = inc_of(note_of[v]);
            uint32_t pl = gv[v].plucks;
            int16_t o[OP1_BLOCK];
            ENGINE_airguitar.render(host_voice_mem + v * 0x24, o, K);
            if (gv[v].plucks != pl) { pluck_at_n[v]++; pluck_time[v] = now; }
            double g = on[v] ? 1.0 : rel[v] / 20.0;
            if (!on[v]) rel[v]--;
            for (int i = 0; i < OP1_BLOCK; i++) {
                m[i] += o[i] * g;
                if (fabs((double)o[i]) > peak_out) peak_out = fabs((double)o[i]);
                if (!isfinite((double)o[i])) fails++;
            }
        }
        for (int i = 0; i < OP1_BLOCK && nmix < (long)(sizeof mixbuf / sizeof *mixbuf); i++) mixbuf[nmix++] = m[i];
        now += OP1_BLOCK;
    }
}
/* a stroke: `g` g along the axis for `n` readings, then the braking (the opposite) */
static void stroke(double g, int n) { nprof = prof_i = 0; for (int i = 0; i < n; i++) prof[nprof++] = g; for (int i = 0; i < n; i++) prof[nprof++] = -g; }
static double rms(long from, long to) { double s = 0; for (long i = from; i < to; i++) s += mixbuf[i] * mixbuf[i]; return sqrt(s / (to - from + 1e-9)); }
static void fresh(void)
{
    all_off(); run(44100 / 4);
    memset(gv, 0, sizeof gv); memset(&gs, 0, sizeof gs); memset(pluck_at_n, 0, sizeof pluck_at_n);
    for (int v = 0; v < 6; v++) rel[v] = 0;
    nprof = prof_i = 0; axis = gaxis = 2; noise = 0.003; nmix = 0; now = 0;
    host_accel[0] = 0; host_accel[1] = 0; host_accel[2] = 16384;
}
static const int EMAJ[6] = { 40, 47, 52, 56, 59, 64 };    /* E2 B2 E3 G#3 B3 E4 */
static void hold(const int *ch, int n) { for (int v = 0; v < n; v++) key_on(v, ch[v], 100); }

/* pitch of the mix (one string) by autocorrelation around the expected period, parabolic peak */
static double cents_off(double midi, long from, long len)
{
    double f0 = 440.0 * pow(2, (midi - 69) / 12.0), p0 = 44100.0 / f0, best = -1e300; int bl = 0;
    int lo = (int)(p0 * 0.94), hi = (int)(p0 * 1.06) + 1;
    static double c[8192];
    for (int l = lo; l <= hi; l++) {
        double s = 0; for (long i = from; i < from + len; i++) s += mixbuf[i] * mixbuf[i + l];
        c[l - lo] = s; if (s > best) { best = s; bl = l; }
    }
    double y0 = c[bl - lo - 1 < 0 ? 0 : bl - lo - 1], y1 = c[bl - lo], y2 = c[bl - lo + 1 > hi - lo ? hi - lo : bl - lo + 1];
    double d = (y0 - 2 * y1 + y2) != 0 ? 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) : 0;
    return 1200 * log2(p0 / (bl + d));
}

static void frame(const char *label)
{
    SCREEN_airguitar.update(0, 16);
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
    for (int v = 0; v < 6; v++) *(uint32_t *)(host_voice_mem + v * 0x24) = 0xdeadbeef;
    K[0] = 0; K[1] = 0; K[2] = 16384; K[3] = 22000; K[4] = OP1_ENGINE_MAGIC + 1;
    SCREEN_airguitar.init(0);
    const char *mode = argc > 1 ? argv[1] : "";

    if (!*mode) {
        /* GESTURE: held keys silent, a stroke down plucks low -> high, once */
        fresh(); hold(EMAJ, 6); run(44100 / 2);
        CHECK(rms(0, nmix) < 1, "held keys sound without a stroke (rms %.1f)", rms(0, nmix));
        uint32_t id0 = gs.id; long t0 = now;
        stroke(-1.5, 5); run(44100 / 2);
        CHECK(gs.id == id0 + 1, "stroke down fired %u times", gs.id - id0);
        CHECK(gs.dir == 1, "stroke down read as up");
        int ord = 1; for (int v = 1; v < 6; v++) ord &= pluck_time[v] > pluck_time[v - 1];
        CHECK(ord, "down: strings not low -> high");
        for (int v = 0; v < 6; v++) CHECK(pluck_at_n[v] == 1, "down: string %d plucked %u times", v, pluck_at_n[v]);
        double span = (pluck_time[5] - pluck_time[0]) / 44.1;
        CHECK(span > 15 && span < 100, "down: strum spread %.1f ms", span);
        CHECK(pluck_time[0] - t0 < 44100 / 8, "down: first string %.0f ms after the stroke began", (pluck_time[0] - t0) / 44.1);
        CHECK(rms(t0, nmix) > 1500, "strummed chord too quiet (rms %.0f)", rms(t0, nmix));
        /* up */
        memset(pluck_at_n, 0, sizeof pluck_at_n);
        stroke(1.5, 5); run(44100 / 2);
        CHECK(gs.id == id0 + 2 && gs.dir == -1, "stroke up: id %u dir %d", gs.id - id0, gs.dir);
        ord = 1; for (int v = 1; v < 6; v++) ord &= pluck_time[v] < pluck_time[v - 1];
        CHECK(ord, "up: strings not high -> low");
        /* stronger stroke: tighter */
        memset(pluck_at_n, 0, sizeof pluck_at_n);
        stroke(-4.0, 4); run(44100 / 2);
        double span2 = (pluck_time[5] - pluck_time[0]) / 44.1;
        CHECK(span2 < span, "hard stroke not tighter (%.1f vs %.1f ms)", span2, span);
        /* new keys pressed after a stroke do not sound by themselves */
        all_off(); run(44100 / 4); nmix = 0;
        key_on(0, 45, 100); run(44100 / 4);
        CHECK(rms(0, nmix) < 1, "new key sounds from an old stroke");

        /* continuous strumming (the hand swings down and up): eighths at 70 / 110 / 150 BPM, 0.5 .. 2 g */
        for (int bpm = 70; bpm <= 150; bpm += 40) for (int a = 0; a < 3; a++) {
            fresh(); hold(EMAJ, 6); run(44100 / 4);
            double per = 60.0 / bpm * 69, amp = 0.5 * (1 << a);
            nprof = prof_i = 0; for (int i = 0; i < (int)(per * 8); i++) prof[nprof++] = amp * sin(2 * M_PI * i / per);
            uint32_t id0 = gs.id; int8_t d0 = 0, alt = 1; uint32_t seen = id0;
            for (long t = 0; t < (long)(per * 8 * 639) + 44100 / 4; t += OP1_BLOCK) {
                run(OP1_BLOCK);
                if (gs.id != seen) { if (seen != id0 && gs.dir == d0) alt = 0; d0 = gs.dir; seen = gs.id; }
            }
            CHECK(gs.id - id0 >= 16 && gs.id - id0 <= 17 && alt, "strumming %d BPM %.1f g: %u strokes%s", bpm, amp, gs.id - id0, alt ? "" : ", not alternating");
        }
        /* held on its side (gravity on X): a move toward the floor still reads down */
        fresh(); gaxis = 0; axis = 0; host_accel[0] = 16384; host_accel[2] = 0;
        hold(EMAJ, 3); run(44100 / 2);
        stroke(-1.5, 5); run(44100 / 3);
        CHECK(gs.id == 1 && gs.dir == 1, "on its side: id %u dir %d", gs.id, gs.dir);
        /* sideways motion (perpendicular to gravity) does not strum */
        axis = 1; stroke(1.0, 5); run(44100 / 3);
        CHECK(gs.id == 1, "sideways motion strummed");

        /* noise and slow tilting never fire */
        fresh(); hold(EMAJ, 6); noise = 0.06; run(44100 * 4);
        CHECK(gs.id == 0, "sensor noise fired %u strokes", gs.id);
        manual = 1;
        for (int i = 0; i <= 200; i++) {                   /* turned 60 degrees and back over 6 s */
            double th = 1.047 * sin(i * M_PI / 200);
            host_accel[0] = (int16_t)lrint(16384 * sin(th)); host_accel[2] = (int16_t)lrint(16384 * cos(th));
            run(1323);
        }
        manual = 0;
        CHECK(gs.id == 0, "slow tilt fired %u strokes", gs.id);

        /* FORCE: a light stroke fires at 0, not at 100 */
        fresh(); hold(EMAJ, 6); run(44100 / 4);
        stroke(-0.5, 5); run(44100 / 3);
        CHECK(gs.id == 1, "light stroke at FORCE 0: %u", gs.id);
        K[6] = 32767; stroke(-0.7, 5); run(44100 / 3);
        CHECK(gs.id == 1, "medium stroke at FORCE 100 fired");
        stroke(-5.0, 4); run(44100 / 3);
        CHECK(gs.id == 2, "hard stroke at FORCE 100: %u", gs.id - 1);
        K[6] = 0;

        /* GESTURE + KEYS: a key plucks at once */
        fresh(); K[7] = 32767; key_on(0, 52, 127); run(44100 / 4);
        CHECK(pluck_at_n[0] == 1 && pluck_time[0] < 300, "keys mode: pluck %u at %ld", pluck_at_n[0], pluck_time[0]);
        CHECK(rms(0, nmix) > 1000, "keys mode: silent");
        K[7] = 0;

        /* tuning */
        static const double notes[] = { 40, 45, 52, 57, 64, 69, 76, 84 };
        double worst = 0;
        for (int ty = 0; ty < AG_TYPES; ty++)
            for (int tn = 0; tn < 3; tn++)
                for (int su = 0; su < 2; su++)
                    for (unsigned ni = 0; ni < sizeof notes / sizeof *notes; ni++) {
                        fresh(); K[0] = (int16_t)(ty * 4096 + 100); K[2] = (int16_t)(tn * 16383); K[3] = su ? 32767 : 12000; K[7] = 32767;
                        key_on(0, (int)notes[ni], 127); run(44100 / 2);
                        double c = cents_off(notes[ni], 2000, 8000);
                        if (fabs(c) > fabs(worst)) worst = c;
                        CHECK(fabs(c) < 4, "%s tone %d sus %d note %.0f: %+.1f cents", ag_types[ty].name, tn, su, notes[ni], c);
                    }
        printf("tuning: worst %+.2f cents\n", worst);
        K[0] = 0; K[2] = 16384; K[3] = 22000; K[7] = 0;

        /* ring time (T60 of the level) per guitar, A2 and E4, default sustain */
        for (int ty = 0; ty < AG_TYPES; ty++) {
            double t60[2]; static const int nn[2] = { 45, 64 };
            for (int q = 0; q < 2; q++) {
                fresh(); K[0] = (int16_t)(ty * 4096 + 100); K[7] = 32767; key_on(0, nn[q], 127); run(44100 * 3);
                double a0 = rms(2205, 6615), a1 = rms(44100 * 2, 44100 * 2 + 4410);
                t60[q] = 60.0 / (20 * log10(a0 / (a1 + 1e-9)) / 1.85);
            }
            printf("%-8s T60 A2 %.1f s, E4 %.1f s (strokes %u, plucks %u)\n", ag_types[ty].name, t60[0], t60[1], gs.id, pluck_at_n[0]);
        }
        K[0] = 0; K[7] = 0;
        /* palm mute dies sooner */
        double tail[2];
        for (int m = 0; m < 2; m++) {
            fresh(); K[3] = m ? 32767 : 0; K[7] = 32767; key_on(0, 45, 127); run(44100 * 2);
            tail[m] = rms(44100, 44100 * 2) / (rms(0, 4410) + 1e-9);
        }
        CHECK(tail[0] < tail[1] * 0.1, "palm mute tail %.3f vs sustain %.3f", tail[0], tail[1]);
        K[3] = 22000; K[7] = 0;

        /* every guitar, drive min and max: in range, and six strings strummed hard stay finite */
        for (int ty = 0; ty < AG_TYPES; ty++) for (int d = 0; d < 2; d++) {
            fresh(); K[0] = (int16_t)(ty * 4096 + 100); K[1] = d ? 32767 : 0; peak_out = 0;
            hold(EMAJ, 6); run(4410); stroke(-6.0, 4); run(44100);
            CHECK(peak_out <= 32767 && peak_out > 4000, "%s drive %d: peak %.0f", ag_types[ty].name, d, peak_out);
        }
        K[0] = 0; K[1] = 0;
        for (int v = 0; v < 6; v++) CHECK(*(uint32_t *)(host_voice_mem + v * 0x24) == 0xdeadbeef, "voice %d word overwritten", v);

        /* the screen: every guitar and page, strings ringing */
        int low = 1;
        for (int ty = 0; ty < AG_TYPES; ty++) for (int pg = 0; pg < 4; pg++) {
            fresh(); K[0] = (int16_t)(ty * 4096 + 100); op1_eng_page[1] = pg & 1; op1_eng_page[3] = pg >> 1; K[7] = pg == 3 ? 32767 : 0;
            hold(EMAJ, 6); run(4410); stroke(-2.0, 5); run(8000);
            for (int f = 0; f < 4; f++) SCREEN_airguitar.update(0, 16);
            low &= lowest_ok();
        }
        CHECK(low, "screen draws below y = 140");
        op1_eng_page[1] = op1_eng_page[3] = 0; K[0] = 0; K[7] = 0;
        printf(fails ? "%d FAILED\n" : "all airguitar checks passed\n", fails);
        return fails != 0;
    }

    if (!strcmp(mode, "wav")) {                         /* a strummed demo: every guitar, one progression each */
        static const int prog[4][6] = { { 40, 47, 52, 55, 59, 64 },    /* Em */
                                        { 36, 43, 48, 52, 55, 64 },    /* C (x32010 + low C) */
                                        { 43, 47, 50, 55, 59, 67 },    /* G */
                                        { 38, 45, 50, 57, 62, 66 } };  /* D */
        static const int pattern[8] = { -1, 0, -1, 1, 0, 1, -1, 1 };  /* D . D U . U D U (eighths) */
        static const int16_t types[] = { 0, 2, 4, 5, 6, 7 };
        fresh();
        long eighth = 44100 * 60 / 96 / 2;
        for (unsigned ti = 0; ti < sizeof types / sizeof *types; ti++) {
            K[0] = (int16_t)(types[ti] * 4096 + 100); K[1] = types[ti] == 6 ? 20000 : 0;
            for (int c = 0; c < 4; c++) {
                all_off(); hold(prog[c], 6);
                for (int b = 0; b < 8; b++) {
                    if (pattern[b]) stroke(pattern[b] < 0 ? -2.5 + 0.4 * (b == 0) : 1.6, 3);
                    run(eighth);
                }
            }
        }
        all_off(); run(44100);
        FILE *f = fopen(argv[2], "wb");
        double mx = 1; for (long i = 0; i < nmix; i++) if (fabs(mixbuf[i]) > mx) mx = fabs(mixbuf[i]);
        double g = 0.9 * 32767 / mx;
        unsigned char h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\1\0\x44\xac\0\0\x88\x58\1\0\2\0\x10\0data....";
        uint32_t ds = (uint32_t)nmix * 2, rs = ds + 36;
        memcpy(h + 4, &rs, 4); memcpy(h + 40, &ds, 4); fwrite(h, 1, 44, f);
        for (long i = 0; i < nmix; i++) { int16_t s = (int16_t)lrint(mixbuf[i] * g); fwrite(&s, 2, 1, f); }
        fclose(f);
        fprintf(stderr, "%.1f s, mix peak %.0f (gain %.2f)\n", nmix / 44100.0, mx, g);
        return 0;
    }

    if (!strcmp(mode, "render")) {
        static const char *lab[] = { "CLASSIC: E major strummed down", "FLAMENCO", "FOLK, page 2 on green (FORCE)",
                                     "JUMBO", "JAZZ: archtop", "CLEAN: electric", "CRUNCH, page 2 on red (play mode)", "BANJO: three strings" };
        for (int ty = 0; ty < AG_TYPES; ty++) {
            fresh(); K[0] = (int16_t)(ty * 4096 + 100); K[1] = ty == 6 ? 20000 : 0;
            op1_eng_page[1] = ty == 2; op1_eng_page[3] = ty == 6; K[6] = ty == 2 ? 8000 : 0; K[7] = ty == 6 ? 32767 : 0;
            hold(EMAJ, ty == 7 ? 3 : 6); run(4410); stroke(-2.0, 5); run(ty * 2000 + 6000);
            frame(lab[ty]);
        }
        return 0;
    }

    if (!strcmp(mode, "anim")) {                         /* 20 fps: chords, strokes down and up */
        static const int prog[2][6] = { { 40, 47, 52, 56, 59, 64 }, { 45, 52, 57, 61, 64, 69 } };
        fresh(); K[0] = 2 * 4096 + 100;
        hold(prog[0], 6);
        for (int f = 0; f < 120; f++) {
            if (f == 60) { all_off(); hold(prog[1], 6); }
            int b = f % 15;
            if (b == 2) stroke(-2.2, 3);
            if (b == 9) stroke(1.6, 3);
            run(2205);
            frame("anim");
        }
        return 0;
    }
    return 2;
}
