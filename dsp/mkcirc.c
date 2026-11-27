#include "mkcirc.h"
#include "mathx.h"

void mc_reset(McCirc *c, float T, int nodes_total, int op_out, int op_p, int op_n, int out)
{
    char *p = (char *)c;
    for (unsigned i = 0; i < sizeof(*c); i++) p[i] = 0;
    c->T = T; c->nu = nodes_total - 2;
    c->op_out = op_out; c->op_p = op_p; c->op_n = op_n; c->out = out;
}

void mc_add(McCirc *c, int type, int a, int b, float val)
{
    McEl *e = &c->el[c->ne++];
    e->t = (unsigned char)type; e->a = (unsigned char)a; e->b = (unsigned char)b; e->v = val; e->J = 0.f;
}

void mc_build(McCirc *c)
{
    const int n = c->nu;
    float M[MC_MAXN * MC_MAXN * 2];            /* augmented [M | I] */
    float rin[MC_MAXN];
    for (int i = 0; i < n * n * 2; i++) M[i] = 0.f;
    for (int i = 0; i < n; i++) rin[i] = 0.f;
    const int W = 2 * n;
    for (int k = 0; k < c->ne; k++) {
        McEl *e = &c->el[k];
        float r = e->t ? 0.f : (e->v < 1e-3f ? 1e-3f : e->v);
        e->g = e->t ? 2.f * e->v / c->T : 1.f / r;
        int nd[2] = { e->a, e->b };
        for (int s = 0; s < 2; s++) {
            int p = nd[s], q = nd[1 - s];
            if (p < 2 || p == c->op_out) continue;
            M[(p - 2) * W + (p - 2)] += e->g;
            if (q >= 2) M[(p - 2) * W + (q - 2)] -= e->g;
            else if (q == 1) rin[p - 2] += e->g;        /* input source, unit voltage */
        }
    }
    {   /* op-amp constraint row */
        int r = c->op_out - 2;
        if (c->op_p >= 2) M[r * W + c->op_p - 2] += 1.f; else if (c->op_p == 1) rin[r] -= 1.f;
        if (c->op_n >= 2) M[r * W + c->op_n - 2] -= 1.f; else if (c->op_n == 1) rin[r] += 1.f;
    }
    for (int i = 0; i < n; i++) M[i * W + n + i] = 1.f;
    for (int col = 0; col < n; col++) {          /* Gauss-Jordan, partial pivot */
        int piv = col; float best = mx_abs(M[col * W + col]);
        for (int r = col + 1; r < n; r++) if (mx_abs(M[r * W + col]) > best) { best = mx_abs(M[r * W + col]); piv = r; }
        if (piv != col) for (int j = 0; j < W; j++) { float t = M[col * W + j]; M[col * W + j] = M[piv * W + j]; M[piv * W + j] = t; }
        float inv = 1.f / M[col * W + col];
        for (int j = 0; j < W; j++) M[col * W + j] *= inv;
        for (int r = 0; r < n; r++) if (r != col) {
            float f = M[r * W + col];
            if (f != 0.f) for (int j = 0; j < W; j++) M[r * W + j] -= f * M[col * W + j];
        }
    }
    for (int i = 0; i < n; i++) {
        float s = 0.f;
        for (int j = 0; j < n; j++) { c->Minv[i * n + j] = M[i * W + n + j]; s += M[i * W + n + j] * rin[j]; }
        c->win[i] = s;
    }
    /* state-space over capacitor histories */
    int nc = 0;
    for (int k = 0; k < c->ne && nc < MC_MAXC; k++) if (c->el[k].t) c->cap[nc++] = (unsigned char)k;
    c->nc = nc;
    float P[MC_MAXN * MC_MAXC];                 /* x = win*vin + P*J */
    for (int i = 0; i < n; i++) for (int j = 0; j < nc; j++) {
        const McEl *e = &c->el[c->cap[j]];
        float s = 0.f;
        if (e->a >= 2 && e->a != c->op_out) s -= c->Minv[i * n + e->a - 2];
        if (e->b >= 2 && e->b != c->op_out) s += c->Minv[i * n + e->b - 2];
        P[i * MC_MAXC + j] = s;
    }
    #define XW(nd) ((nd) == 0 ? 0.f : (nd) == 1 ? 1.f : c->win[(nd) - 2])
    #define XP(nd, j) ((nd) < 2 ? 0.f : P[((nd) - 2) * MC_MAXC + (j)])
    for (int k = 0; k < nc; k++) {
        const McEl *e = &c->el[c->cap[k]];
        float alpha = XW(e->a) - XW(e->b);
        c->SB[k] = -2.f * e->g * alpha;
        for (int j = 0; j < nc; j++)
            c->SA[k * MC_MAXC + j] = -2.f * e->g * (XP(e->a, j) - XP(e->b, j)) - (k == j ? 1.f : 0.f);
    }
    c->SD = XW(c->out);
    for (int j = 0; j < nc; j++) c->SC[j] = XP(c->out, j);
    #undef XW
    #undef XP
}

/* Per orde N volledig uitgerold (constante lusgrenzen), zelfde optelvolgorde als de
 * generieke lus, dus bit-identieke uitkomst; ~5x minder instructies op RV32. */
#define MC_TICK_N(N)                                                              \
static float mc_tick_##N(McCirc *c, float vin)                                    \
{                                                                                 \
    float J[N], Jn[N], y = c->SD * vin;                                           \
    for (int j = 0; j < N; j++) J[j] = c->J[j];                                   \
    for (int j = 0; j < N; j++) y += c->SC[j] * J[j];                             \
    for (int k = 0; k < N; k++) {                                                 \
        const float *a = &c->SA[k * MC_MAXC];                                     \
        float s = c->SB[k] * vin;                                                 \
        for (int j = 0; j < N; j++) s += a[j] * J[j];                             \
        Jn[k] = s;                                                                \
    }                                                                             \
    for (int k = 0; k < N; k++) c->J[k] = Jn[k];                                  \
    return y;                                                                     \
}
MC_TICK_N(1) MC_TICK_N(2) MC_TICK_N(3) MC_TICK_N(4)
MC_TICK_N(5) MC_TICK_N(6) MC_TICK_N(7) MC_TICK_N(8)
_Static_assert(MC_MAXC == 8, "mc_tick: uitgerolde ordes bijwerken");

float mc_tick(McCirc *c, float vin)
{
    switch (c->nc) {
    case 1: return mc_tick_1(c, vin); case 2: return mc_tick_2(c, vin);
    case 3: return mc_tick_3(c, vin); case 4: return mc_tick_4(c, vin);
    case 5: return mc_tick_5(c, vin); case 6: return mc_tick_6(c, vin);
    case 7: return mc_tick_7(c, vin); case 8: return mc_tick_8(c, vin);
    default: return c->SD * vin;
    }
}
