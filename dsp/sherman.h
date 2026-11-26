/* sherman.h - Sherman Filterbank 2 style dual switched-capacitor filter.
 * Two LTC1060-class sampled-data state-variable filters clocked at ratio x fc.
 * In HARMONICS (sync) mode both filter clocks are divided from one fast master
 * clock, so filter 2's clock is phase-locked to filter 1 at f1*D1/D2. */
#ifndef SHERMAN_H
#define SHERMAN_H
#include <stdint.h>

#define SH_MAXEV 48

typedef struct {
    float lp, bp, hp;   /* integrator states (volts) */
    float y;            /* held output */
    float clk;          /* clock line level (+1/-1) */
    float bl;           /* current bleed amplitude */
    float res_next;     /* BLEP residual for next output sample */
    float morph, corr;  /* L-B-H morph 0..1, correction -1..1 (targets) */
    float sm_morph, sm_corr, sm_damp;   /* per-sample smoothed values actually used */
    float damp;         /* 1/Q from the RESO knob */
    float dq;           /* effective damping after clock-rate Q enhancement */
} ShFilter;

typedef struct {
    float fs, dt;
    /* knobs (0..1 unless noted) */
    float k_in;          /* input level -20..+40 dB (unity at 1/3): drive into overdrive */
    int   hi_sw;         /* 0 = HI cut, 1 = flat, 2 = HI boost */
    int   limit;         /* LIMIT switch: -6 dB into the filters, room for self-oscillation */
    float k_f1, k_f2;    /* cutoffs */
    float k_r1, k_r2;    /* resonance */
    int   harm;          /* 0=free, 1..11 = 1,1.5,2,3,4,5,6,7,8,9,16 */
    float k_ser;         /* 0 parallel .. 1 serial */
    float k_env1, k_env2;/* ADSR amount per filter, bipolar -1..1, exact 0 in the middle */
    float k_bleed;       /* clock feedthrough amount 0..1 */
    float k_a, k_d, k_s, k_r; /* ADSR */
    float ratio;         /* clock-to-centre ratio, 50 or 100 */
    float fmax;          /* cutoff ceiling (CPU guard) */
    /* state */
    ShFilter f[2];
    int   mi;            /* master clock count, integer part (mod D1*D2) */
    float mf;            /* master clock count, fractional part [0,1) */
    float ph1, ph2;      /* free-running clock phases [0,1) */
    float x_prev;        /* previous (driven) input sample */
    float env; int env_stage; int gate;
    float k_speed;       /* MIDI CC5-style speed-up, 1..4 */
    float a_att, a_dec, a_rel;
    float dc_x1, dc_y1, a_dc;
    float hi_lp, a_hi, bias, a_bias_att, a_bias_rel;
    float rect_dc, a_rdc;
    float sm_f1, sm_f2, a_sm; int sm_init;   /* knob smoothing (no zipper on MIDI sweeps) */
    float svf_F;         /* fixed Chamberlin coefficient 2 sin(pi/ratio) */
} Sherman;

void  sherman_init(Sherman *s, float fs);
void  sherman_update(Sherman *s);           /* call after changing knobs */
void  sherman_gate(Sherman *s, int on);     /* gate edge: on = trigger (re)starts the ADSR */
float sherman_tick(Sherman *s, float x);    /* x in volts */
#endif
