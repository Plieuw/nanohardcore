/* render.c - offline test renderer for the chain. Writes 16-bit mono WAVs.
 * usage: render <out_dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../dsp/chain.h"

#define FS 44100
static float buf_in[FS * 12], buf_out[FS * 12];

static void wav(const char *dir, const char *name, const float *x, int n)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s.wav", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); exit(1); }
    int bytes = n * 2;
    unsigned hdr[11] = { 0x46464952, 36u + bytes, 0x45564157, 0x20746d66, 16, 0x00010001, FS, FS * 2, 0x00100002, 0x61746164, (unsigned)bytes };
    fwrite(hdr, 4, 11, f);
    for (int i = 0; i < n; i++) { float v = x[i] * 32767.f; if (v > 32767) v = 32767; if (v < -32768) v = -32768; short s = (short)lrintf(v); fwrite(&s, 2, 1, f); }
    fclose(f);
}

static unsigned rng = 1;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)(int)rng / 2147483648.f; }

/* test inputs */
static int make_pulses(float *x, int n)        /* 5 ms trigger pulses, 120 BPM 8ths, accents */
{
    memset(x, 0, sizeof(float) * n);
    int step = FS / 4, k = 0;
    for (int t = FS / 10; t + FS / 200 < n; t += step, k++) {
        float a = (k % 4 == 0) ? 0.9f : 0.35f;
        for (int i = 0; i < FS / 200; i++) x[t + i] = a;
    }
    return k;
}
static int make_drums(float *x, int n)         /* synthetic acoustic-ish loop with ghost notes */
{
    memset(x, 0, sizeof(float) * n);
    const int pat[16] = { 3, 0, 1, 0, 2, 0, 1, 1, 3, 0, 1, 0, 2, 0, 4, 1 }; /* 3 kick 2 snare 1 hat(ghost) 4 tom */
    int step = FS / 8, hits = 0;
    for (int s = 0; s < 6 * 16 && FS / 10 + s * step < n - FS / 2; s++) {
        int t0 = FS / 10 + s * step, kind = pat[s % 16];
        if (!kind) continue;
        for (int i = 0; i < FS / 2 && t0 + i < n; i++) {
            float tt = (float)i / FS, v = 0.f;
            if (kind == 3) v = 0.8f * sinf(2 * 3.14159f * (60.f * tt + 40.f * (1 - expf(-tt * 40)) / 40.f)) * expf(-tt * 9);
            if (kind == 2) v = 0.6f * (0.6f * frand() * expf(-tt * 18) + 0.5f * sinf(2 * 3.14159f * 190 * tt) * expf(-tt * 25));
            if (kind == 1) v = 0.05f * frand() * expf(-tt * 60);
            if (kind == 4) v = 0.7f * sinf(2 * 3.14159f * 110 * tt) * expf(-tt * 4);   /* long ringing tom */
            x[t0 + i] += v;
        }
        if (kind != 1) hits++;
    }
    return hits;
}

static void cc(Chain *c, int num, int val) { chain_midi(c, 0xB0, (unsigned char)num, (unsigned char)val); }

static int verbose = 0;
static int run(Chain *c, const float *in, float *out, int n)
{
    uint32_t t0 = c->trig_count;
    for (int i = 0; i < n; i += 64) {
        int m = n - i < 64 ? n - i : 64;
        uint32_t b = c->trig_count;
        chain_process(c, in + i, out + i, NULL, (uint32_t)m);
        if (verbose && c->trig_count != b) printf("  trig @ %.3f s (step %.2f)\n", (double)i / FS, (i / (double)FS - 0.1) * 8);
    }
    return (int)(c->trig_count - t0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static Chain c;
    int n = FS * 6;

    /* 1: 909 alone (direct), accents low/high, tune/decay sweep */
    {
        BD909 b; bd909_init(&b, FS);
        float *o = buf_out; int len = FS * 4; memset(o, 0, sizeof(float) * len);
        float set[4][3] = { {0.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 1.0f}, {0.1f, 0.9f, 0.5f}, {1.0f, 0.2f, 0.5f} };
        for (int k = 0; k < 4; k++) {
            bd909_set(&b, set[k][0], set[k][1], set[k][2], 0.8f);
            bd909_trigger(&b, k == 0 ? 0.2f : 1.0f);
            for (int i = 0; i < FS; i++) o[k * FS + i] = bd909_tick(&b) * 0.25f;
        }
        wav(dir, "01_bd909_alone", o, len);
    }

    /* 2: trigger pulses through default chain */
    int np = make_pulses(buf_in, n);
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    int got = run(&c, buf_in, buf_out, n);
    printf("pulses: sent %d detected %d\n", np, got);
    wav(dir, "02_in_pulses", buf_in, n);
    wav(dir, "02_chain_default_pulses", buf_out, n);

    /* 3: drum loop as trigger */
    int nd = make_drums(buf_in, n);
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    verbose = 1; got = run(&c, buf_in, buf_out, n); verbose = 0;
    printf("drums: hits %d detected %d\n", nd, got);
    wav(dir, "03_in_drums", buf_in, n);
    wav(dir, "03_chain_drums", buf_out, n);

    /* 4: Sherman harmonics: sync 16 (/32 divider), high reso, low cutoff, serial */
    make_pulses(buf_in, n);
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    cc(&c, 86, 127); cc(&c, 25, 70); cc(&c, 26, 110); cc(&c, 30, 120); cc(&c, 87, 100);
    cc(&c, 88, 90); cc(&c, 90, 100); cc(&c, 24, 70);
    run(&c, buf_in, buf_out, n);
    wav(dir, "04_sherman_harm16", buf_out, n);

    /* 5: Sherman sync ratio 1.5 / 3 (odd divider), band-pass */
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    cc(&c, 86, 23); cc(&c, 25, 60); cc(&c, 26, 115); cc(&c, 27, 64); cc(&c, 30, 115); cc(&c, 31, 64);
    cc(&c, 88, 100); cc(&c, 90, 127);
    run(&c, buf_in, buf_out, n);
    wav(dir, "05_sherman_harm1p5", buf_out, n);

    /* 6: Mackie cooked: trim high, mid boost, gain up */
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    cc(&c, 107, 100); cc(&c, 110, 110); cc(&c, 109, 100); cc(&c, 112, 115); cc(&c, 117, 90);
    run(&c, buf_in, buf_out, n);
    wav(dir, "06_mackie_cooked", buf_out, n);

    /* 7: full monty on drum loop */
    make_drums(buf_in, n);
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    cc(&c, 86, 46); cc(&c, 25, 66); cc(&c, 26, 100); cc(&c, 30, 104); cc(&c, 87, 64); cc(&c, 88, 100);
    cc(&c, 24, 90); cc(&c, 90, 90); cc(&c, 21, 90); cc(&c, 22, 100);
    cc(&c, 107, 90); cc(&c, 110, 100); cc(&c, 112, 105);
    run(&c, buf_in, buf_out, n);
    wav(dir, "07_full_drums", buf_out, n);

    /* 8: Sherman aliasing check: sine sweep via thru, kick level 0, no drive, free mode */
    {
        chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
        cc(&c, 23, 0); cc(&c, 116, 127); cc(&c, 24, 30); cc(&c, 25, 64); cc(&c, 26, 30);
        cc(&c, 29, 127); cc(&c, 88, 64); cc(&c, 90, 0); cc(&c, 107, 15); cc(&c, 114, 0);
        double ph = 0;
        for (int i = 0; i < n; i++) { double f = 50.0 * pow(400.0, (double)i / n); ph += 2 * M_PI * f / FS; buf_in[i] = 0.3f * (float)sin(ph); }
        run(&c, buf_in, buf_out, n);
        wav(dir, "08_sweep_lowfc", buf_out, n);
    }
    /* 12: ADSR bipolar on both filters (free, parallel): f1 up, f2 down, drum loop gates */
    make_drums(buf_in, n);
    chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
    cc(&c, 25, 55); cc(&c, 29, 95); cc(&c, 26, 100); cc(&c, 30, 100); cc(&c, 27, 64); cc(&c, 31, 0);
    cc(&c, 88, 120); cc(&c, 89, 10); cc(&c, 102, 10); cc(&c, 103, 45); cc(&c, 104, 50); cc(&c, 105, 55);
    run(&c, buf_in, buf_out, n);
    wav(dir, "12_env_bipolar", buf_out, n);
    return 0;
}
