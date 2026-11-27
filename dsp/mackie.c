/* mackie.c - Mackie 1202-VLZ PRO channel 1-4, LINE IN -> MAIN OUT, from the schematic
 * (main board 990-217-00 rev B, sheet 1, channel 1). Everything after the input runs at
 * 2x the host rate.
 *
 *  PREAMP  Q5/Q1 2SA1084 pair, emitters joined by R151 3R32 + TRIM R164; U7A/U7B (NJM2068,
 *          +-16 V) servo the emitters through R641/R642 5k11:
 *              U7A = vb1 + i*5k11,  U7B = vb2 - i*5k11,  i = (vb1 - vb2) / Rg
 *          U11A difference amp R188/R204 5k11 over R187/R203 10k = x0.511.
 *          Unbalanced line: only one base moves, so the legs hit the rails at
 *          different input levels -> two-step clipping knee.
 *  LO CUT  U13A (NJM4560) unity-gain 3rd order high-pass: C116/C158/C160 0.22u,
 *          R171 5k1, R224 1k3 (to output), R220 62k.            [circuit-simulated]
 *  EQ      U27B (NJM2068), inverting active EQ, all networks between the input rail
 *          and the op-amp output, wipers summed into the virtual earth:
 *            HI  C222 .018u - R341 50kB - C235 .018u, wiper R342 1k1
 *            MID R385 910 - C260 .012u || R391 50kB - R407 910, wiper R392 1k3
 *            LO  R424 6k8 - R434 50kB (C285/C305 .033u across each half) - R460 6k8,
 *                wiper R416 10k
 *            HI/MID wipers meet at C261 .012u; R428 1M || C286 10p feedback.
 *                                                                [circuit-simulated]
 *  GAIN    R565 30k + R605 50k(G) whose wiper is grounded: the top half is the input
 *          shunt, the bottom half + R570 2k sets U40A's gain with R555 36k:
 *              g40 = Rt/(R565+Rt) * (1 + R555/(R570+Rb))      (max +21.5 dB)
 *          Pan + mix-bus losses are folded into the main-mix normalisation so that
 *          GAIN at the detent is unity and fully up is +10 dB (manual).
 *  Clipping: every op-amp output has a soft-knee rail clip (ADAA-1) at +-14.3 V.
 */
#include "mackie.h"
#include "mathx.h"

#define R641 5110.f
#define K_DIFF (5110.f / 10000.f)
#define VSAT 14.3f
#define KNEE 0.6f
#define K_LINE 0.173f        /* CAL: line pad so TRIM-down = -15 dB (manual) */
#define R565 30e3f
#define R605 50e3f
#define R570 2e3f
#define R555 36e3f

static const float HB[MK_HB] = { 0.316382109f, -0.100428285f, 0.054581228f, -0.033495689f, 0.021144010f,
                                 -0.013162919f, 0.007865202f, -0.004378397f, 0.002187973f, -0.001012556f };

static const float CMP[7] = { 1.03687966f, 0.35232363f, -0.36440902f, -0.17302312f, 0.16270823f, 0.09012300f, -0.10287984f };

/* ---- soft-knee rail clip, ADAA-1 ---- */
static inline float clipf(float x)
{
    float a = mx_abs(x), y;
    if (a <= VSAT - KNEE) return x;
    if (a >= VSAT + KNEE) y = VSAT;
    else { float t = a - (VSAT - KNEE); y = a - t * t * (1.f / (4.f * KNEE)); }
    return x < 0.f ? -y : y;
}
static inline float clipF(float x)
{
    float a = mx_abs(x);
    const float lo = VSAT - KNEE, hi = VSAT + KNEE;
    if (a <= lo) return 0.5f * a * a;
    if (a < hi) { float t = a - lo; return 0.5f * a * a - t * t * t * (1.f / (12.f * KNEE)); }
    float t = 2.f * KNEE;
    return 0.5f * hi * hi - t * t * t * (1.f / (12.f * KNEE)) + VSAT * (a - hi);
}
static inline float adaa(MkAdaa *s, float x)
{
    float d = x - s->x1, F = clipF(x), y;
    if (mx_abs(d) > 1e-5f) y = (F - s->F1) / d; else y = clipf(0.5f * (x + s->x1));
    s->x1 = x; s->F1 = F;
    return y;
}

/* ---- circuits ---- */
enum { G = 0, A = 1 };
/* EQ nodes */
enum { H1 = 2, H2, WH, M1, M2, WM, L1, L2, WL, X, Y, O, EQ_N };
/* element order matters: pot halves are rewritten in mackie_update */
static void eq_build_topology(McCirc *c, float T)
{
    mc_reset(c, T, EQ_N, O, G, X, O);
    mc_add(c, 1, A, H1, 0.018e-6f);            /* 0 C222 */
    mc_add(c, 0, H1, WH, 25e3f);               /* 1 R341 top    */
    mc_add(c, 0, WH, H2, 25e3f);               /* 2 R341 bottom */
    mc_add(c, 1, H2, O, 0.018e-6f);            /* 3 C235 */
    mc_add(c, 0, WH, Y, 1.1e3f);               /* 4 R342 */
    mc_add(c, 0, A, M1, 910.f);                /* 5 R385 */
    mc_add(c, 1, M1, M2, 0.012e-6f);           /* 6 C260 */
    mc_add(c, 0, M1, WM, 25e3f);               /* 7 R391 top */
    mc_add(c, 0, WM, M2, 25e3f);               /* 8 R391 bottom */
    mc_add(c, 0, M2, O, 910.f);                /* 9 R407 */
    mc_add(c, 0, WM, Y, 1.3e3f);               /* 10 R392 */
    mc_add(c, 0, A, L1, 6.8e3f);               /* 11 R424 */
    mc_add(c, 0, L1, WL, 25e3f);               /* 12 R434 top */
    mc_add(c, 0, WL, L2, 25e3f);               /* 13 R434 bottom */
    mc_add(c, 1, L1, WL, 0.033e-6f);           /* 14 C285 */
    mc_add(c, 1, L2, WL, 0.033e-6f);           /* 15 C305 */
    mc_add(c, 0, L2, O, 6.8e3f);               /* 16 R460 */
    mc_add(c, 0, WL, X, 10e3f);                /* 17 R416 */
    mc_add(c, 1, Y, X, 0.012e-6f);             /* 18 C261 */
    mc_add(c, 0, X, O, 1e6f);                  /* 19 R428 (C286 10p across it: < 0.01 dB in band, omitted) */
}
static void pot(McCirc *c, int top, float R, float pos)    /* pos 0 = wiper at top end */
{
    pos = mx_clamp(pos, 0.f, 1.f);
    c->el[top].v = R * pos + 5.f;              /* + wiper/track contact resistance */
    c->el[top + 1].v = R * (1.f - pos) + 5.f;
}
/* HPF nodes */
enum { Q1 = 2, Q2, Q3, HO, HP_N };
static void hpf_build_topology(McCirc *c, float T)
{
    mc_reset(c, T, HP_N, HO, Q3, HO, HO);
    mc_add(c, 1, A, Q1, 0.22e-6f);             /* C116 */
    mc_add(c, 0, Q1, G, 5.1e3f);               /* R171 */
    mc_add(c, 1, Q1, Q2, 0.22e-6f);            /* C158 */
    mc_add(c, 0, Q2, HO, 1.3e3f);              /* R224 */
    mc_add(c, 1, Q2, Q3, 0.22e-6f);            /* C160 */
    mc_add(c, 0, Q3, G, 62e3f);                /* R220 */
    mc_build(c);
}

static float g40_of(float a)       /* a: fraction of R605 between top and wiper */
{
    float rt = R605 * a, rb = R605 * (1.f - a);
    return rt / (R565 + rt) * (1.f + R555 / (R570 + rb));
}
static float gain_taper(const Mackie *m, float k)   /* G taper: detent (0.75) -> unity */
{
    k = mx_clamp(k, 0.f, 1.f);
    if (k <= 0.75f) { float u = k / 0.75f; return m->a_unity * u * u; }
    return m->a_unity + (1.f - m->a_unity) * (k - 0.75f) * 4.f;
}

void mackie_update(Mackie *m)
{
    float db = -15.f + 60.f * mx_clamp(m->k_trim, 0.f, 1.f);
    float g = mx_exp2(db * (1.f / 20.f) * 3.32192809f);
    float inst = g / (K_LINE * K_DIFF);
    if (inst < 1.0001f) inst = 1.0001f;
    m->rg = 2.f * R641 / (inst - 1.f);
    /* EQ pots: centre detent = 0.5; knob +1 = boost = wiper towards the input side */
    /* het EQ-netwerk alleen opnieuw oplossen als een EQ-pot echt verandert:
     * mc_build is een volledige eliminatie, te zwaar voor elke knopbeweging */
    if (!m->eq_built || m->k_hi != m->eq_hi || m->k_mid != m->eq_mid || m->k_lo != m->eq_lo) {
        pot(&m->eq, 1, 50e3f, 0.5f - 0.5f * m->k_hi);
        pot(&m->eq, 7, 50e3f, 0.5f - 0.5f * m->k_mid);
        pot(&m->eq, 12, 50e3f, 0.5f - 0.5f * m->k_lo);
        mc_build(&m->eq);
        m->eq_hi = m->k_hi; m->eq_mid = m->k_mid; m->eq_lo = m->k_lo; m->eq_built = 1;
    }
    m->g40 = g40_of(gain_taper(m, m->k_gain));
    float km = mx_clamp(m->k_main, 0.f, 1.f), km2 = km * km;
    m->g_main = (km2 * km2) / (0.75f * 0.75f * 0.75f * 0.75f) / g40_of(m->a_unity);
}

void mackie_init(Mackie *m, float fs)
{
    char *p = (char *)m;
    for (unsigned i = 0; i < sizeof(*m); i++) p[i] = 0;
    m->fs = fs; m->dt = 1.f / fs;
    m->k_trim = 0.25f; m->lowcut = 0; m->k_gain = 0.75f; m->k_main = 0.75f;
    /* GAIN detent: the wiper position where U40A gives max/3.16 (unity overall, +10 dB max) */
    {
        float target = g40_of(1.f) * 0.31622777f, lo = 0.f, hi = 1.f;
        for (int i = 0; i < 40; i++) { float a = 0.5f * (lo + hi); if (g40_of(a) < target) lo = a; else hi = a; }
        m->a_unity = 0.5f * (lo + hi);
    }
    float T = 0.5f / fs;
    hpf_build_topology(&m->hpf, T);
    eq_build_topology(&m->eq, T);
    mackie_update(m);
}

/* one sample at 2x rate */
static float run2x(Mackie *m, float v)
{
    float vb1 = v * K_LINE, vb2 = 0.f;
    float i = (vb1 - vb2) / m->rg;
    float u7a = adaa(&m->c7a, vb1 + i * R641);
    float u7b = adaa(&m->c7b, vb2 - i * R641);
    float y = clipf(K_DIFF * (u7a - u7b));                 /* U11A (inputs already bounded) */
    if (m->lowcut) y = clipf(mc_tick(&m->hpf, y));        /* SW2 LO CUT -> U13A */
    y = -mc_tick(&m->eq, y);                               /* U27B inverts; restore polarity */
    y = adaa(&m->c27, y);
    y = adaa(&m->c40, y * m->g40);                         /* GAIN -> U40A */
    y = adaa(&m->cmx, y * m->g_main);                      /* mix bus + main amp */
    /* the 4 cascaded ADAA-1 stages each average two 2x samples (cos(w/2) roll-off);
     * a 13-tap linear-phase FIR restores the top: +-0.3 dB to 18 kHz, -1.7 dB @ 20 kHz */
    float *h = m->cmp;
    for (int k = 0; k < 12; k++) h[k] = h[k + 1];
    h[12] = y;
    y = CMP[0] * h[6];
    for (int k = 1; k <= 6; k++) y += CMP[k] * (h[6 - k] + h[6 + k]);
    return y;
}

float mackie_tick(Mackie *m, float v)
{
    /* halfband interpolation: even = delayed input, odd = halfway sample */
    float *u = m->up;                                      /* 2*MK_HB history, newest last */
    for (int k = 0; k < 2 * MK_HB - 1; k++) u[k] = u[k + 1];
    u[2 * MK_HB - 1] = v;
    float even = u[MK_HB - 1], odd = 0.f;
    for (int k = 0; k < MK_HB; k++) odd += HB[k] * (u[MK_HB - 1 - k] + u[MK_HB + k]);
    odd *= 2.f;
    float w0 = run2x(m, even), w1 = run2x(m, odd);
    /* halfband decimation */
    float *d = m->dn;                                      /* 4*MK_HB+2 history of 2x samples */
    const int L = 4 * MK_HB + 2;
    for (int k = 0; k < L - 2; k++) d[k] = d[k + 2];
    d[L - 2] = w0; d[L - 1] = w1;
    const int c = L - 1 - (2 * MK_HB - 1);                 /* centre tap index (odd phase) */
    float y = 0.5f * d[c];
    for (int k = 0; k < MK_HB; k++) y += HB[k] * (d[c - 1 - 2 * k] + d[c + 1 + 2 * k]);
    return y;
}
