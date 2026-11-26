/* bd909.h - Roland TR-909 bass drum, component-level model.
 * Source: TR-909 Service Notes (1984), voicing board, BASS DRUM section.
 * All internal quantities are volts / amps / seconds. Output in volts at IC11a. */
#ifndef BD909_H
#define BD909_H
#include <stdint.h>

#define BD_SHAPER_N 256

typedef struct {
    float fs, dt;
    /* knobs 0..1 */
    float k_tune, k_decay, k_attack, k_level;
    /* calibration to measured units (see bd909.c) */
    float cal_vth, cal_kcv, cal_pulse_ms, cal_sat;
    float k_pitch, k_depth;   /* mods: base pitch +-12 st, sweep depth 0..2x (0 = stock) */
    /* derived */
    float r_dis9;      /* C9 discharge resistance (R57+VR2) || R26 */
    float r_dis8;      /* C8 discharge resistance (R58+VR5) || R54 */
    float a_click_hp;  /* C2/R7 differentiator */
    float g_sk, k_sk;  /* Q9 Sallen-Key LP (TPT SVF form) */
    float a_nlp;       /* R46/C13 noise LP */
    float a_env2;      /* C12 decay */
    float a_dc;        /* output coupling */
    /* state */
    float vc9, vc8, vc12;
    float tri, dir;
    float pulse_prev, hp_x1, hp_y1;
    float sk_s1, sk_s2;
    float nlp;
    float dc_x1, dc_y1;
    int   trig_left;
    float vtrig;
    uint32_t seed;
    float shaper[BD_SHAPER_N + 1];
} BD909;

void  bd909_init(BD909 *b, float fs);
void  bd909_set(BD909 *b, float tune, float decay, float attack, float level);
void  bd909_trigger(BD909 *b, float accent);   /* accent 0..1 */
float bd909_tick(BD909 *b);
#endif
