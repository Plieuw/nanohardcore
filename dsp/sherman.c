/* sherman.c - dual clocked switched-capacitor filter (Sherman Filterbank 2 style).
 *
 * What is modelled from the hardware facts:
 *  - LTC1060 is a sampled-data state-variable filter; f0 = fclk / ratio (50:1 or 100:1).
 *    The integrator coefficient is fixed by on-chip capacitor ratios, so tuning is
 *    done ONLY by the clock. Here: Chamberlin/LDI SVF with constant F = 2 sin(pi/ratio)
 *    that is executed once per clock edge, at the exact (sub-sample) edge time.
 *  - The input is sampled at the clock edges without anti-alias filtering, and the
 *    outputs are held between edges (staircase). Images around k*fclk and folded
 *    input components therefore appear exactly where the hardware puts them.
 *  - HARMONICS: filter-2 clock = master / D2 while filter-1 clock = master / D1,
 *    both from one fast master oscillator, so edges stay phase-locked. Odd dividers
 *    give non-50% duty clocks (floor(D/2)/D), which adds even harmonics to the bleed.
 *  - Clock feedthrough ("eee") is injected on each filter output.
 *  - The input overdrive is evaluated at the sampling instants on the band-limited
 *    input, so its ultrasonic harmonics are folded by the clock like in the unit.
 *  - Op-amp/integrator swing limits (LTC1060 on +-5 V) bound resonance -> scream.
 * Every discontinuity (output step, clock edge) is band-limited with a 2-sample
 * polyBLEP, so only the hardware's aliasing survives, not the model's.
 * NOT from a schematic (none public): input overdrive curve, knob laws.
 * ADSR: capacitor envelope gated by the input level (like the unit's audio trigger),
 * a trigger during the attack is absorbed (as documented), bipolar amount per filter with an exact
 * zero at the centre. In HARMONICS (sync) mode filter 2 follows filter 1, so its own
 * amount has no effect - as on the hardware.
 */
#include "sherman.h"
#include "mathx.h"

static const unsigned char HARM_D2[12] = { 0, 2, 3, 4, 6, 8, 10, 12, 14, 16, 18, 32 }; /* /D1=2 */

/* input stage ("tube sound overdrive"): no schematic is public, so the structure follows
 * the documented behaviour and the levels are CAL:
 *   gain -20..+40 dB -> HI cut/flat/boost shelf -> asymmetric triode-like saturation with
 *   grid-current bias shift -> (LIMIT: -6 dB) -> LTC1060, whose +-5 V integrators then
 *   saturate: that is what "pushes away the resonance peaks" at high input. */
#define VK_IN    1.5f   /* overdrive knee volts (CAL) */
#define OD_OUT   2.4f   /* drive stage output scale: ~+-3.6 V max into the LTC1060 (CAL) */
#define NEG_SAT  0.85f  /* cut-off side saturates harder than the conduction side */
#define BIAS_K   0.35f  /* grid-current bias shift per unit of positive overdrive (CAL) */
#define BIAS_MAX 1.5f   /* saturating limit of that shift (in knee units) */
#define HI_F     3000.f /* HI boost/cut shelf corner (CAL) */
#define OVER_K0  0.6f   /* knob position where the second (hard) stage starts to get gain */
#define OVER_G   20.f   /* extra gain of that stage at full knob (+26 dB) */
#define OVER_V   4.0f   /* its rail (V): at the LTC1060 swing, so the chip clips along */
#define OVER_PRES 0.0f  /* (presence into the clipper: off) */
#define RECT_K0  0.8f   /* last part of INPUT: morph normal -> half-wave -> full-wave rectify */
#define SC_KNEE 3.0f   /* integrator soft limit start (V) */
#define SC_SPAN 1.2f   /* knee to rail (V): rail ~4.2 V on +-5 V */
#define BLEED_V 0.06f  /* clock feedthrough volts at k_bleed = 1 (CAL) */
#define BLEED_F0 8000.f   /* clock freq below which the bleed is fully present */
#define BLEED_F1 20000.f  /* clock freq above which it is gone (inaudible)   */

typedef struct { float t; unsigned char k, rise; } Ev;

static inline float sc_lim(float v)
{
    float a = mx_abs(v);
    if (a <= SC_KNEE) return v;
    float u = (a - SC_KNEE) * (1.f / SC_SPAN);
    float r = SC_KNEE + SC_SPAN * u / (1.f + u);
    return v < 0.f ? -r : r;
}

/* triode-like transfer, unit slope and zero curvature at 0 (clean at low level):
 * conduction side saturates late and soft (scale POS_SAT), cut-off side earlier and
 * harder (NEG_SAT). The asymmetry, and so the 2nd harmonic, grows with drive; the
 * grid-current bias shift adds more when driven hard. */
#define POS_SAT 1.35f
static inline float padet(float v) { v = mx_clamp(v, -3.f, 3.f); return v * (27.f + v * v) / (27.f + 9.f * v * v); }
/* second stage: op-amp style rail clip with a narrow knee -> sharp edges, odd harmonics */
static inline float hardclip(float v)
{
    const float k = 0.25f, lo = OVER_V - k;
    float a = mx_abs(v), y;
    if (a <= lo) return v;
    if (a >= OVER_V + k) y = OVER_V; else { float t = a - lo; y = a - t * t * (1.f / (4.f * k)); }
    return v < 0.f ? -y : y;
}
static inline float tube(float u)
{
    return u >= 0.f ? POS_SAT * padet(u * (1.f / POS_SAT)) : NEG_SAT * padet(u * (1.f / NEG_SAT));
}

static float knob_fc(float k) { return 2.f * mx_exp2(12.6f * mx_clamp(k, 0.f, 1.f)); } /* 2 Hz .. 12.4 kHz */
/* RESO: the LTC1060's Q is set by a resistor ratio (Q = R3/R2 in mode 1), so the pot
 * moves 1/Q logarithmically: a little reso already goes a long way. The knob alone never
 * reaches zero damping; filter 2 is built with more resonance than filter 1. (CAL) */
#define Q_MIN_DAMP  1.4f          /* reso 0: Q ~0.7                                    */
#define Q1_MAX_DAMP 0.050f        /* reso max, filter 1: Q 20                          */
#define Q2_MAX_DAMP 0.030f        /* reso max, filter 2: Q 33                          */
/* Q enhancement of the switched-capacitor integrators at high clock rates (finite op-amp
 * settling): the effective damping drops with fclk, so self-oscillation only becomes
 * possible in the mid/upper kHz range - no sub-bass self-oscillation. */
#define QE_FCLK 500000.f          /* clock at which the damping has dropped by QE_AMT   */
#define QE_AMT  0.25f
static float knob_damp(float r, float qmax_damp)
{
    r = mx_clamp(r, 0.f, 1.f);
    return Q_MIN_DAMP * mx_pow(qmax_damp / Q_MIN_DAMP, mx_pow(r + 1e-6f, 0.7f));
}
static inline float q_enh(float fclk) { float u = fclk * (1.f / QE_FCLK); return QE_AMT * u * (0.6f + 0.4f * u); }
/* ADSR knob law: 2 ms .. 30 s (manual fig. 22 runs to 30 s), divided by the speed-up */
static float knob_time(float k) { return 0.002f * mx_exp2(13.87f * mx_clamp(k, 0.f, 1.f)); }
/* bipolar ADSR amount: exact zero at the centre, square law for fine control near zero */
static float env_depth_oct(float d) { d = mx_clamp(d, -1.f, 1.f); return 7.f * d * mx_abs(d); }

void sherman_update(Sherman *s)
{
    s->f[0].damp = knob_damp(s->k_r1, Q1_MAX_DAMP);
    s->f[1].damp = knob_damp(s->k_r2, Q2_MAX_DAMP);
    s->svf_F = 2.f * mx_sin(MX_PI / s->ratio);
    {   /* analog ADSR: one capacitor, charged/discharged through the A/D/R resistors.
         * Attack charges towards an overshoot target (ATT_TGT) and the comparator ends it
         * at 1.0: time to 1.0 = tau*ln(ATT_TGT/(ATT_TGT-1)). Knob time = that duration. */
        float sp = mx_clamp(s->k_speed, 1.f, 4.f);
        const float att_ln = 1.0986123f;                  /* ln(1.5/(1.5-1)) with target 1.5 */
        s->a_att = mx_rc_coef(knob_time(s->k_a) / sp / att_ln, s->dt);
        s->a_dec = mx_rc_coef(knob_time(s->k_d) / sp / 4.6f, s->dt);   /* ~-40 dB in knob time */
        s->a_rel = mx_rc_coef(knob_time(s->k_r) / sp / 4.6f, s->dt);
    }
    float fmax_cpu = 20.f * s->fs / s->ratio;       /* <= 20 clock edges / sample / filter */
    if (s->fmax > fmax_cpu) s->fmax = fmax_cpu;
}

void sherman_init(Sherman *s, float fs)
{
    char *p = (char *)s;
    for (unsigned i = 0; i < sizeof(*s); i++) p[i] = 0;
    s->fs = fs; s->dt = 1.f / fs;
    s->ratio = 50.f; s->fmax = 12000.f;
    s->k_in = 0.333f; s->k_f1 = 0.75f; s->k_f2 = 0.70f; s->k_r1 = 0.35f; s->k_r2 = 0.35f; /* clocks ~70 kHz / ~45 kHz: no audible whine by default */
    s->harm = 0; s->k_ser = 0.f; s->k_env1 = 0.5f; s->k_env2 = 0.f; s->k_bleed = 0.15f;
    s->k_speed = 1.f;
    s->k_a = 0.f; s->k_d = 0.35f; s->k_s = 0.f; s->k_r = 0.3f;
    s->f[0].clk = s->f[1].clk = -1.f;
    s->f[0].morph = 0.f; s->f[1].morph = 0.f;
    s->a_dc = mx_rc_coef(1.f / (2.f * MX_PI * 5.f), s->dt);
    s->hi_sw = 1; s->limit = 0;
    s->a_hi = 1.f - mx_rc_coef(1.f / (2.f * MX_PI * HI_F), s->dt);
    s->a_bias_att = mx_rc_coef(0.010f, s->dt);
    s->a_bias_rel = mx_rc_coef(0.150f, s->dt);
    s->a_rdc = mx_rc_coef(1.f / (2.f * MX_PI * 8.f), s->dt);
    s->a_sm = mx_rc_coef(0.012f, s->dt);              /* 12 ms glide on knob moves */
    sherman_update(s);
}

void sherman_gate(Sherman *s, int on)
{
    if (on) {
        /* analog: (re)start the attack from the current capacitor level. A trigger during
         * the attack changes nothing, so a long attack skips every 2nd / 3rd trigger
         * (manual lesson 6) by itself. */
        s->env_stage = 1;
        s->gate = 1;
    } else {
        s->gate = 0;
    }
}

static float filt_out(const ShFilter *f)
{
    float m = f->sm_morph, y;
    if (m < 0.5f) y = f->lp + (f->bp - f->lp) * (2.f * m);
    else          y = f->bp + (f->hp - f->bp) * (2.f * m - 1.f);
    float c = f->sm_corr;
    if (c < 0.f) y += c * f->bp;                       /* towards -B : B - B = 0 */
    else         y += c * (f->lp + f->hp - f->bp);     /* towards -B+LH : notch  */
    return y;
}

/* collect clock events of one filter in the count interval (p0, p1]: rising edges at
 * i*per, falling edges at i*per + doff. Both filters of a synced pair use the same
 * master count, so coincident edges get bit-identical times. */
static int collect(Ev *ev, int n, float p0, float p1, float per, float doff, int k, int edges)
{
    float inv = 1.f / (p1 - p0);
    int i0 = mx_floor(p0 / per);
    for (int i = i0; ; i++) {
        float tr = (float)i * per, tf = tr + doff;
        if (tr > p1 && (!edges || tf > p1)) break;
        if (tr > p0 && tr <= p1 && n < SH_MAXEV) { ev[n].t = (tr - p0) * inv; ev[n].k = (unsigned char)k; ev[n].rise = 1; n++; }
        if (edges && tf > p0 && tf <= p1 && n < SH_MAXEV) { ev[n].t = (tf - p0) * inv; ev[n].k = (unsigned char)k; ev[n].rise = 0; n++; }
    }
    return n;
}

float sherman_tick(Sherman *s, float x)
{
    /* ---- input level; the overdrive is evaluated at the clock edges (see below):
     * the LTC1060 samples the continuous, already distorted signal, so harmonics of
     * the drive above fs/2 fold down by the filter clock, exactly as in hardware. ---- */
    float xg = x * mx_exp2((s->k_in * 60.f - 20.f) * 0.16609640f);  /* -20..+40 dB, unity at 1/3 */
    /* "OVER" region: the top of the INPUT knob also raises a hard second stage and pushes
     * extra highs into it (sharper edges, more bite) */
    float ov = s->k_in > OVER_K0 ? (s->k_in - OVER_K0) * (1.f / (1.f - OVER_K0)) : 0.f;
    {   /* HI cut / flat / boost shelf (1st order), plus OVER presence */
        float hp = xg - s->hi_lp;
        s->hi_lp += s->a_hi * hp;
        float hg = (s->hi_sw == 2 ? 2.f : (s->hi_sw == 0 ? 0.5f : 1.f)) + OVER_PRES * ov;
        xg = s->hi_lp + hp * hg;
    }
    /* grid-current bias shift: positive overdrive charges the coupling cap (10 ms / 150 ms) */
    {
        float pos = xg * (1.f / VK_IN); pos = pos > 1.f ? pos - 1.f : 0.f;
        pos = pos * BIAS_MAX / (BIAS_MAX + pos);           /* grid conduction limits the shift */
        float a = pos > s->bias ? s->a_bias_att : s->a_bias_rel;
        s->bias = pos + a * (s->bias - pos);
    }
    const float bsh = -BIAS_K * s->bias, od0 = tube(bsh);
    const float lim = s->limit ? 0.5f : 1.f;
    const float g2 = 1.f + (OVER_G - 1.f) * ov * ov;
    /* rectifier amount (in front of the drive): negative half is scaled by (1 - 2 rho):
     * 0 normal, .5 half-wave, 1 full-wave (octave up), DC removed like a coupling cap */
    const float rho = s->k_in > RECT_K0 ? (s->k_in - RECT_K0) * (1.f / (1.f - RECT_K0)) : 0.f;
    const float nsc = 1.f - 2.f * rho, rdc = s->rect_dc;
    float rsum = 0.f; int rcnt = 0;

    /* ---- envelope: capacitor ADSR, runs while the gate is high ---- */
    if (s->env_stage == 1) {                                   /* attack: overshoot target 1.5 */
        s->env = 1.5f + s->a_att * (s->env - 1.5f);
        if (s->env >= 1.f) { s->env = 1.f; s->env_stage = s->gate ? 2 : 3; }
    } else if (s->env_stage == 2) {                            /* decay -> sustain, held by gate */
        s->env = s->k_s + s->a_dec * (s->env - s->k_s);
        if (!s->gate) s->env_stage = 3;
    } else if (s->env_stage == 3) {                            /* release */
        s->env *= s->a_rel;
    }

    /* ---- clocks ---- */
    /* ---- knob smoothing: 7-bit MIDI steps and block-rate updates become glides ---- */
    if (!s->sm_init) {
        s->sm_f1 = s->k_f1; s->sm_f2 = s->k_f2; s->sm_init = 1;
        for (int k = 0; k < 2; k++) { s->f[k].sm_morph = s->f[k].morph; s->f[k].sm_corr = s->f[k].corr; s->f[k].sm_damp = s->f[k].damp; }
    }
    {
        const float a = s->a_sm;
        s->sm_f1 = s->k_f1 + a * (s->sm_f1 - s->k_f1);
        s->sm_f2 = s->k_f2 + a * (s->sm_f2 - s->k_f2);
        for (int k = 0; k < 2; k++) {
            ShFilter *f = &s->f[k];
            f->sm_morph = f->morph + a * (f->sm_morph - f->morph);
            f->sm_corr  = f->corr  + a * (f->sm_corr  - f->corr);
            f->sm_damp  = f->damp  + a * (f->sm_damp  - f->damp);
        }
    }
    float fc1 = knob_fc(s->sm_f1) * mx_exp2(env_depth_oct(s->k_env1) * s->env);
    fc1 = mx_clamp(fc1, 0.3f, s->fmax);
    float fclk1 = fc1 * s->ratio;
    float p0[2], p1[2], per[2], doff[2], fclk[2];
    if (s->harm > 0) {
        /* one fast master clock; filter clocks = master / D. The count is kept as an
         * integer (mod D1*D2) + fraction so long runs never lose precision. */
        int D1 = 2, D2 = HARM_D2[s->harm], L = D1 * D2;
        float inc = fclk1 * (float)D1 * s->dt;            /* master cycles per sample */
        float r = (float)s->mi + s->mf;
        p0[0] = p0[1] = r; p1[0] = p1[1] = r + inc;
        per[0] = (float)D1; doff[0] = (float)(D1 / 2);
        per[1] = (float)D2; doff[1] = (float)(D2 / 2);     /* odd D: floor(D/2)/D duty */
        fclk[0] = fclk1; fclk[1] = fclk1 * (float)D1 / (float)D2;
        float mf = s->mf + inc; int n = (int)mf;
        s->mf = mf - (float)n; s->mi = (s->mi + n) % L;
    } else {
        float fc2 = knob_fc(s->sm_f2) * mx_exp2(env_depth_oct(s->k_env2) * s->env);
        fc2 = mx_clamp(fc2, 0.3f, s->fmax);
        fclk[0] = fclk1; fclk[1] = fc2 * s->ratio;
        p0[0] = s->ph1; p1[0] = s->ph1 + fclk[0] * s->dt;
        p0[1] = s->ph2; p1[1] = s->ph2 + fclk[1] * s->dt;
        per[0] = per[1] = 1.f; doff[0] = doff[1] = 0.5f;
        s->ph1 = p1[0] - (float)(int)p1[0];
        s->ph2 = p1[1] - (float)(int)p1[1];
    }

    /* effective damping incl. clock-rate Q enhancement */
    s->f[0].dq = s->f[0].sm_damp - q_enh(fclk[0]);
    s->f[1].dq = s->f[1].sm_damp - q_enh(fclk[1]);

    /* ---- events, time-ordered ---- */
    Ev e0[SH_MAXEV], e1[SH_MAXEV];
    /* clock feedthrough only becomes audible when the clock itself drops into the audio
     * band: the bottom segment of the cutoff range (fc < ~400 Hz at 50:1). In HARMONICS
     * mode the divided clock rides on the fast master, so filter 1's clock decides. */
    float fref0 = fclk[0], fref1 = s->harm > 0 ? fclk[0] : fclk[1];
    float g0 = mx_clamp((BLEED_F1 - fref0) * (1.f / (BLEED_F1 - BLEED_F0)), 0.f, 1.f);
    float g1 = mx_clamp((BLEED_F1 - fref1) * (1.f / (BLEED_F1 - BLEED_F0)), 0.f, 1.f);
    g0 = g0 * g0 * (3.f - 2.f * g0); g1 = g1 * g1 * (3.f - 2.f * g1);
    float bl0 = s->k_bleed * BLEED_V * g0, bl1 = s->k_bleed * BLEED_V * g1;
    int edges0 = bl0 > 0.f, edges1 = bl1 > 0.f;
    float bla[2] = { bl0, bl1 };
    int n0 = collect(e0, 0, p0[0], p1[0], per[0], doff[0], 0, edges0);
    int n1 = collect(e1, 0, p0[1], p1[1], per[1], doff[1], 1, edges1);

    float out_prev[2], acc_prev[2] = { 0.f, 0.f }, acc_cur[2] = { 0.f, 0.f };
    for (int k = 0; k < 2; k++) {
        /* bleed level changes are applied as a step at the sample boundary */
        out_prev[k] = s->f[k].y + s->f[k].bl * s->f[k].clk + s->f[k].res_next;
        s->f[k].bl = bla[k];
    }

    const float Fc = s->svf_F, ser = s->k_ser, x0 = s->x_prev, xd = xg;
    int i0 = 0, i1 = 0;
    while (i0 < n0 || i1 < n1) {
        Ev *e;
        /* ties: filter 2 samples before filter 1 updates (S/H aperture before output change) */
        if (i1 >= n1 || (i0 < n0 && e0[i0].t < e1[i1].t)) e = &e0[i0++]; else e = &e1[i1++];
        ShFilter *f = &s->f[e->k];
        float d = 1.f - e->t;                      /* time from edge to sample n */
        float wp = 0.5f * d * d, wc = -0.5f * (1.f - d) * (1.f - d);
        float delta;
        if (e->rise) {
            float ui = (x0 + (xd - x0) * e->t) * (1.f / VK_IN);
            if (rho > 0.f) {                   /* rectifier ahead of the drive stages */
                if (ui < 0.f) ui *= nsc;
                rsum += ui; rcnt++;
                ui -= rdc;                     /* coupling cap: remove the rectifier's DC */
            }
            float xin = (tube(ui + bsh) - od0) * (VK_IN * OD_OUT);
            if (g2 > 1.f) xin = hardclip(xin * g2);
            xin *= lim;
            if (e->k == 1) xin = (1.f - ser) * xin + ser * s->f[0].y;
            f->lp = sc_lim(f->lp + Fc * f->bp);
            f->hp = xin - f->lp - f->dq * f->bp;
            f->bp = sc_lim(f->bp + Fc * f->hp);
            float y = filt_out(f);
            delta = y - f->y; f->y = y;
            if ((e->k == 0 && edges0) || (e->k == 1 && edges1)) { delta += f->bl * (1.f - f->clk); f->clk = 1.f; }
        } else {
            delta = -f->bl * (1.f + f->clk); f->clk = -1.f;
        }
        acc_prev[e->k] += delta * wp;
        acc_cur[e->k]  += delta * wc;
    }
    s->x_prev = xd;
    {   /* track the rectified DC (8 Hz one-pole at audio rate) */
        if (rho <= 0.f) s->rect_dc = 0.f;
        else if (rcnt) { float m = rsum / (float)rcnt; s->rect_dc = m + s->a_rdc * (s->rect_dc - m); }
    }

    float o[2];
    for (int k = 0; k < 2; k++) { o[k] = out_prev[k] + acc_prev[k]; s->f[k].res_next = acc_cur[k]; }
    float y = (1.f - ser) * o[0] + o[1];

    float yo = s->a_dc * (s->dc_y1 + y - s->dc_x1);
    s->dc_x1 = y; s->dc_y1 = yo;
    return yo;
}
