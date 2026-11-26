/* bd909.c - TR-909 BASS DRUM, component-level model.
 *
 * Mapping to the service-notes schematic (voicing board, BASS DRUM):
 *   pulse      Q1/Q2 trigger+accent pulse, ~1 ms                (amplitude: ASSUMED 4..12 V)
 *   ENV3       D7 + R21 2.2k charge C9 0.33u; discharge R57 22k + VR2 100k(A) TUNE, || R26 330k
 *   CV         IC12a/b: Vcv = C9*R28/R26 + 15V*R28/R27  (R28 150k, R26 330k, R27 1.5M) -> +1.5 V rest
 *   VCO        IC13a integrator C14 0.033u fed by Vcv/R59 (47k), Q10/Q11 slope reversal,
 *              IC13b Schmitt with R49 100k / R50 47k -> thresholds +-Vsat*47/147
 *              (component values alone predict ~56 Hz rest pitch, matching a real 909)
 *   shaper     R52 10k into antiparallel D10/D11, load R53 2.2k + R43 47 -> triangle to "sine"
 *   VCA        Q12: base gets the shaper node through R53/R43, emitter current set by ENV1
 *   ENV1       R35 1.8k + D9 charge C8 0.33u; discharge R58 47k + VR5 1M(A) DECAY
 *              (R54 470k only carries Q12 base current: negligible load on C8)
 *   attack     Q8 edge: C2 3.3n / R7 100k differentiator -> Q9 Sallen-Key LP (R16 39k, R7 100k,
 *              C3 4.7n, C4 100p => 3.7 kHz, Q ~3.1) ; noise via R46 4.7k / C13 0.1u LP (339 Hz)
 *              gated by Q6 with ENV2 (C12 0.033u, R41 22k charge, R42 470k decay)
 * Calibration constants (not derivable from the drawing alone) are marked CAL.
 */
#include "bd909.h"
#include "mathx.h"

/* components */
#define R21 2.2e3f
#define C9  0.33e-6f
#define R57 22e3f
#define VR2 100e3f
#define R26 330e3f
#define R27 1.5e6f
#define R28 150e3f
#define R59 47e3f
#define C14 0.033e-6f
#define R49 100e3f
#define R50 47e3f
#define R52 10e3f
#define R53 2.2e3f
#define R43 47.f
#define R35 1.8e3f
#define C8  0.33e-6f
#define R58 47e3f
#define VR5 1.0e6f
#define R54 470e3f
#define C2  3.3e-9f
#define R7  100e3f
#define R16 39e3f
#define C3  4.7e-9f
#define C4  100e-12f
#define R46 4.7e3f
#define C13 0.1e-6f
#define R41 22e3f
#define R42 470e3f
#define C12 0.033e-6f
#define VR4 100e3f

#define VSAT   13.5f          /* op-amp swing on +-15 V */
#define VT     0.02585f
#define D_IS   4e-9f          /* 1SS133 / 1N4148 class */
#define D_NVT  (1.9f * VT)

/* CAL */
#define PULSE_LO   7.0f       /* trigger pulse volts, accent 0 */
#define PULSE_HI  12.0f       /* trigger pulse volts, accent 1 */
/* fitted to measurements of real units: settles ~47 Hz, ~230 Hz @5 ms, ~160 Hz @10 ms,
 * body holds full level for 50-80 ms (Q12/IC11a saturation), Tune = sweep length */
#define CAL_VTH    5.14f      /* VCO threshold: printed parts give 4.32 V (=54 Hz); units settle ~47 Hz */
#define CAL_KCV    2.0f       /* ENV3 -> CV gain relative to R28/R26 */
#define CAL_PULSE  1.5f       /* trigger pulse ms */
#define CAL_SAT    0.2f       /* body saturation (V, before IC11a gain): 43 ms hold @ decay .5, 84 ms @ .7 */
#define CAL_OUT    10.0f      /* IC11a make-up so the body peaks ~2 V */
#define K_CLICK    0.11f      /* click path gain into IC11a (via R8/R44, VR3) */
#define K_NOISE    0.35f      /* noise path gain */
#define Q12_DEGEN  0.6f       /* fraction of base signal that appears as vbe (emitter degeneration) */

/* current through series R + diode for source voltage dv (>=0), Newton on
 * dv = i R + nVt ln(1 + i/Is) */
static float diode_r_current(float dv, float r)
{
    if (dv <= 0.f) return 0.f;
    float i = dv > 0.5f ? (dv - 0.5f) / r : dv * 1e-6f / r;
    if (i < 1e-12f) i = 1e-12f;
    for (int k = 0; k < 4; k++) {
        float f  = i * r + D_NVT * mx_log(1.f + i / D_IS) - dv;
        float fp = r + D_NVT / (D_IS + i);
        i -= f / fp;
        if (i < 1e-12f) i = 1e-12f;
    }
    return i;
}

static void build_shaper(BD909 *b)
{
    const float rl = R53 + R43;
    for (int n = 0; n <= BD_SHAPER_N; n++) {
        float vt = -6.f + 12.f * (float)n / BD_SHAPER_N;
        float lo = -6.f, hi = 6.f;
        for (int it = 0; it < 40; it++) {            /* bisection, monotone */
            float v = 0.5f * (lo + hi);
            float e = mx_exp(mx_clamp(v / D_NVT, -60.f, 60.f));
            float g = (vt - v) / R52 - v / rl - D_IS * (e - 1.f / e);
            if (g > 0.f) lo = v; else hi = v;
        }
        b->shaper[n] = 0.5f * (lo + hi);
    }
}

static inline float shaper(const BD909 *b, float vt)
{
    float x = (mx_clamp(vt, -5.999f, 5.999f) + 6.f) * (BD_SHAPER_N / 12.f);
    int i = (int)x; float f = x - (float)i;
    return b->shaper[i] + f * (b->shaper[i + 1] - b->shaper[i]);
}

static float par(float a, float b) { return a * b / (a + b); }

void bd909_set(BD909 *b, float tune, float decay, float attack, float level)
{
    b->k_tune = tune; b->k_decay = decay; b->k_attack = attack; b->k_level = level;
    b->r_dis9 = par(R57 + VR2 * mx_taper_a(tune), R26);
    b->r_dis8 = R58 + VR5 * mx_taper_a(decay);
}

void bd909_init(BD909 *b, float fs)
{
    char *p = (char *)b;
    for (unsigned i = 0; i < sizeof(*b); i++) p[i] = 0;
    b->fs = fs; b->dt = 1.f / fs;
    b->dir = 1.f;
    b->seed = 0x909u;
    b->a_click_hp = mx_rc_coef(C2 * R7, b->dt);
    {   /* unity-gain Sallen-Key: w0 = 1/sqrt(R1R2C1C2), Q = sqrt(R1R2C1C2)/(C4 (R1+R2)) */
        float t = mx_pow(R16 * R7 * C3 * C4, 0.5f);
        float f0 = 1.f / (2.f * MX_PI * t);
        float q = t / (C4 * (R16 + R7));
        b->g_sk = mx_tan(MX_PI * f0 / fs);
        b->k_sk = 1.f / q;
    }
    b->a_nlp = mx_rc_coef(R46 * C13, b->dt);
    b->a_env2 = mx_rc_coef(R42 * C12, b->dt);
    b->a_dc = mx_rc_coef(1.f / (2.f * MX_PI * 4.f), b->dt);
    b->cal_vth = CAL_VTH; b->cal_kcv = CAL_KCV; b->cal_pulse_ms = CAL_PULSE; b->cal_sat = CAL_SAT;
    build_shaper(b);
    bd909_set(b, 0.5f, 0.5f, 0.5f, 0.8f);
}

void bd909_trigger(BD909 *b, float accent)
{
    b->trig_left = (int)(b->cal_pulse_ms * 1e-3f * b->fs + 0.5f);
    b->vtrig = PULSE_LO + (PULSE_HI - PULSE_LO) * mx_clamp(accent, 0.f, 1.f);
    b->tri = 0.f; b->dir = 1.f;      /* Q7 reset: integrator restarts (start phase ASSUMED) */
}

float bd909_tick(BD909 *b)
{
    const float dt = b->dt;
    float vp = 0.f;
    if (b->trig_left > 0) { vp = b->vtrig; b->trig_left--; }

    /* ENV3: pitch envelope on C9 (backward Euler on the linear discharge) */
    {
        float id = diode_r_current(vp - b->vc9, R21);
        b->vc9 = (b->vc9 + dt * id / C9) / (1.f + dt / (C9 * b->r_dis9));
    }
    /* ENV1: amplitude envelope on C8 */
    {
        float id = diode_r_current(vp - b->vc8, R35);
        b->vc8 = (b->vc8 + dt * id / C8) / (1.f + dt / (C8 * b->r_dis8));
    }
    /* ENV2: attack/noise gate on C12 */
    {
        float ic = (vp > b->vc12) ? (vp - b->vc12) / R41 : 0.f;
        b->vc12 = b->vc12 * b->a_env2 + dt * ic / C12;
    }

    /* VCO: triangle integrator + Schmitt, sub-sample exact reflection */
    {
        /* pitch mod = scaling the R27 offset & integrator rate together (whole VCO up/down);
         * depth mod = R26 (ENV3 -> CV gain), like the classic Pitch / Tune-Depth mods */
        const float vth = b->cal_vth;                    /* components: VSAT*R50/(R49+R50) */
        float pm = mx_exp2(b->k_pitch * 1.f), dm = 1.f + b->k_depth;
        float vcv = (b->cal_kcv * dm * b->vc9 * (R28 / R26) + 15.f * (R28 / R27)) * pm;
        float step = vcv / (R59 * C14) * dt;
        b->tri += b->dir * step;
        if (b->tri > vth)       { b->tri = 2.f * vth - b->tri;  b->dir = -1.f; }
        else if (b->tri < -vth) { b->tri = -2.f * vth - b->tri; b->dir = 1.f; }
    }

    /* shaper + Q12 VCA -> IC11a (transimpedance, LEVEL = VR4) */
    float body;
    {
        float v  = shaper(b, b->tri);
        float vb = v * (R43 / (R53 + R43)) * Q12_DEGEN;
        /* base drive through R54; soft turn-off around Vbe (softplus) instead of a hard cut */
        float vo = b->vc8 - 0.65f, ie;
        if (vo > 0.3f) ie = vo / R54;
        else ie = D_NVT * mx_log(1.f + mx_exp(mx_clamp(vo / D_NVT, -30.f, 30.f))) / R54;
        body = ie * (mx_exp(vb / VT) - 1.f) * VR4;
        /* Q12 / IC11a running out of current/swing: soft saturation -> the measured
         * full-level "hold" at the start of the body; asymmetric because of exp() */
        float sv = b->cal_sat, u = mx_clamp(body / sv, -3.f, 3.f);
        body = CAL_OUT * sv * u * (27.f + u * u) / (27.f + 9.f * u * u);   /* Pade tanh */
    }

    /* attack: pulse edge -> HP (C2/R7) -> Q9 Sallen-Key LP ; noise LP x ENV2 */
    float click;
    {
        float hp = b->a_click_hp * (b->hp_y1 + vp - b->pulse_prev);
        b->hp_y1 = hp; b->pulse_prev = vp;
        float g = b->g_sk, k = b->k_sk;
        float h  = (hp - (k + g) * b->sk_s1 - b->sk_s2) / (1.f + g * (k + g));
        float bp = g * h + b->sk_s1; b->sk_s1 = g * h + bp;
        float lp = g * bp + b->sk_s2; b->sk_s2 = g * bp + lp;
        float n = mx_noise(&b->seed);
        b->nlp = n + b->a_nlp * (b->nlp - n);
        click = b->k_attack * (K_CLICK * lp + K_NOISE * b->nlp * b->vc12 * (1.f / PULSE_HI) * 4.f);
    }

    float y = (body + click) * b->k_level;
    /* output coupling capacitor */
    float o = b->a_dc * (b->dc_y1 + y - b->dc_x1);
    b->dc_x1 = y; b->dc_y1 = o;
    return o;
}
