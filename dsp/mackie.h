/* mackie.h - Mackie 1202-VLZ PRO channel 1-4 (line in) -> main out.
 * Source: Mackie 1202 VLZ PRO main board 990-217-00 rev B, sheet 1.
 * Runs at 2x the host rate (halfband up/down). */
#ifndef MACKIE_H
#define MACKIE_H
#include "mkcirc.h"

typedef struct { float x1, F1; } MkAdaa;
#define MK_HB 10           /* odd halfband taps */

typedef struct {
    float fs, dt;
    /* knobs */
    float k_trim;     /* 0..1 : TRIM R164 (line: -15..+45 dB) */
    int   lowcut;     /* SW2: U13A 75 Hz 3rd order */
    float k_lo, k_mid, k_hi;   /* -1..1 (centre detent 0) : R434 / R391 / R341 50kB */
    float k_gain;     /* 0..1 : GAIN R605 50k(G), unity at 0.75 */
    float k_main;     /* 0..1 : MAIN MIX, unity at 0.75 */
    /* derived */
    float rg, g40, g_main, a_unity;
    McCirc hpf, eq;
    /* oversampling */
    float up[2 * MK_HB], dn[4 * MK_HB + 2];
    int   upi, dni;
    /* clip states */
    MkAdaa c7a, c7b, c27, c40, cmx;
    float cmp[13];
} Mackie;

void  mackie_init(Mackie *m, float fs);
void  mackie_update(Mackie *m);
float mackie_tick(Mackie *m, float v);   /* v: volts at LINE IN tip (unbalanced) */
#endif
