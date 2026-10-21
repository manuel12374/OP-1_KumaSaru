/* Long render of monolith on the PC: held notes for minutes, as a drone is played (the stock harness plays a
 * 2.4 s phrase). Raw int16 mono 44.1 kHz on stdout; render_demo.py writes WAVs.
 *   args: seconds  knobs_start(a,b,c,d)  knobs_end(a,b,c,d)  midi_notes(n,n,...)
 * Knobs move linearly from start to end over the render. A stand-in envelope (6 s attack, 8 s release at the
 * end) is applied, as the firmware's ADSR would. Notes go to voices 0.. in order, each with its own velocity.
 * Built with -fsanitize=address,undefined like the stock harness. */
#include "../../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];

extern const op1_engine_t ENGINE_monolith;
static int32_t inc_of(int midi) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100); }
static void parse4(const char *s, int v[4]) { sscanf(s, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]); }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: seconds k0 k1 notes\n"); return 2; }
    double secs = atof(argv[1]); int ka[4], kb[4]; parse4(argv[2], ka); parse4(argv[3], kb);
    int notes[6], nn = 0; char *tok = strtok(argv[4], ",");
    while (tok && nn < OP1_VOICES) { notes[nn++] = atoi(tok); tok = strtok(0, ","); }
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));

    long blocks = (long)(secs * 44100 / OP1_BLOCK), att = (long)(6 * 44100 / OP1_BLOCK), rel = (long)(8 * 44100 / OP1_BLOCK);
    int16_t k[8] = { 0, 0, 0, 0, 0x5301, 0, 0, 0 }, vo[OP1_BLOCK];
    for (int vi = 0; vi < nn; vi++) {
        host_voice_vel[vi] = (int16_t)((vi ? 96 : 127) << 8);
        for (int i = 0; i < 4; i++) k[i] = (int16_t)ka[i];
        ENGINE_monolith.note_on(host_voice_mem + vi * 0x24, k);
    }
    int peak = 0;
    for (long b = 0; b < blocks; b++) {
        double t = (double)b / blocks;
        for (int i = 0; i < 4; i++) k[i] = (int16_t)lrint(ka[i] + (kb[i] - ka[i]) * t);
        double e = b < att ? (double)b / att : (b > blocks - rel ? (double)(blocks - b) / rel : 1.0);
        int32_t mix[OP1_BLOCK] = { 0 };
        for (int vi = 0; vi < nn; vi++) {
            for (int i = 0; i < OP1_BLOCK; i++) host_pitch_inc[i] = inc_of(notes[vi]);
            ENGINE_monolith.render(host_voice_mem + vi * 0x24, vo, k);
            for (int i = 0; i < OP1_BLOCK; i++) mix[i] += vo[i];
        }
        int16_t o[OP1_BLOCK];
        for (int i = 0; i < OP1_BLOCK; i++) {
            int32_t s = (int32_t)lrint(mix[i] * e * 0.8 / (nn > 1 ? 1.3 : 1.0));
            o[i] = op1_sat16(s); if (abs(o[i]) > peak) peak = abs(o[i]);
        }
        fwrite(o, 2, OP1_BLOCK, stdout);
    }
    fprintf(stderr, "peak %d (%.1f dBFS)\n", peak, 20 * log10((peak + 1e-9) / 32768.0));
    return peak < 64;
}
