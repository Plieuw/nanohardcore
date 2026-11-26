/* mkcirc.h - tiny linear circuit simulator: R, C and one ideal op-amp, modified nodal
 * analysis with trapezoidal (bilinear) capacitor companions. Node 0 = ground,
 * node 1 = input source; nodes 2.. are unknowns. The matrix is inverted only when a
 * component value (pot) changes; per sample it is one mat-vec. */
#ifndef MKCIRC_H
#define MKCIRC_H

#define MC_MAXN 12      /* unknown nodes */
#define MC_MAXE 24
#define MC_MAXC 8

typedef struct { unsigned char t, a, b; float v, g, J; } McEl;   /* t: 0 = R, 1 = C */

typedef struct {
    int nu;                      /* unknowns */
    int ne;
    McEl el[MC_MAXE];
    int op_out, op_p, op_n;      /* ideal op-amp: V(p) = V(n), output node free */
    int out;                     /* node read as output */
    float T;
    float Minv[MC_MAXN * MC_MAXN];
    float win[MC_MAXN];          /* response to the input source */
    /* exact discrete state-space over the capacitor history currents:
     *   y = SD*vin + SC.J ;  J' = SA.J + SB*vin   (built from the MNA inverse) */
    int nc; unsigned char cap[MC_MAXC];
    float SA[MC_MAXC * MC_MAXC], SB[MC_MAXC], SC[MC_MAXC], SD, J[MC_MAXC];
} McCirc;

void  mc_reset(McCirc *c, float T, int nodes_total, int op_out, int op_p, int op_n, int out);
void  mc_add(McCirc *c, int type, int a, int b, float val);   /* returns nothing; index = order */
void  mc_build(McCirc *c);                                     /* after (re)setting values */
float mc_tick(McCirc *c, float vin);
#endif
