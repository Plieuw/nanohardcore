/* hcui.c - HARDCORE user interface (see hcui.h). */
#include <stddef.h>
#include "hcui.h"

/* ---- parameters ---- */
enum { K_LIN, K_PM, K_BOOL, K_HARM, K_SPEED, K_GMIN, K_RATIO, K_HI3, K_MODE, K_ACCSRC };
enum { F_FLOAT, F_INT };

typedef struct {
    const char *label;
    uint8_t cc, kind, ftype;
    uint16_t off;                    /* offset of the field in Chain */
} Param;

#define FLT(lbl, cc, kind, member) { lbl, cc, kind, F_FLOAT, (uint16_t)offsetof(Chain, member) }
#define INT(lbl, cc, kind, member) { lbl, cc, kind, F_INT, (uint16_t)offsetof(Chain, member) }

/* order = index; the pages below refer to these indices */
static const Param P[] = {
    /* 0  TRIG */
    FLT("SENS", 114, K_LIN, k_sens),     FLT("HOLD", 115, K_LIN, k_hold),
    FLT("GATE", 82, K_GMIN, k_gmin),     FLT("THRU", 116, K_LIN, k_thru),
    INT("INPUT", 3, K_MODE, mode),
    /* 5  909 */
    FLT("TUNE", 20, K_LIN, bd.k_tune),   FLT("DECAY", 21, K_LIN, bd.k_decay),
    FLT("ATTACK", 22, K_LIN, bd.k_attack), FLT("LEVEL", 23, K_LIN, bd.k_level),
    FLT("ACCENT", 14, K_LIN, k_acc_amt), INT("BRON", 15, K_ACCSRC, acc_fixed),
    FLT("PITCH", 16, K_PM, bd.k_pitch),  FLT("DIEPTE", 17, K_PM, bd.k_depth),
    /* 13 SHERMAN F1 */
    FLT("FREQ 1", 25, K_LIN, sh.k_f1),   FLT("RESO 1", 26, K_LIN, sh.k_r1),
    FLT("L-B-H 1", 27, K_LIN, sh.f[0].morph), FLT("CORR 1", 28, K_PM, sh.f[0].corr),
    /* 17 SHERMAN F2 */
    FLT("FREQ 2", 29, K_LIN, sh.k_f2),   FLT("RESO 2", 30, K_LIN, sh.k_r2),
    FLT("L-B-H 2", 31, K_LIN, sh.f[1].morph), FLT("CORR 2", 85, K_PM, sh.f[1].corr),
    INT("HARM", 86, K_HARM, sh.harm),    FLT("PAR-SER", 87, K_LIN, sh.k_ser),
    /* 23 SHERMAN ENV */
    FLT("A", 102, K_LIN, sh.k_a),        FLT("D", 103, K_LIN, sh.k_d),
    FLT("S", 104, K_LIN, sh.k_s),        FLT("R", 105, K_LIN, sh.k_r),
    FLT("ENV>F1", 88, K_PM, sh.k_env1),  FLT("ENV>F2", 89, K_PM, sh.k_env2),
    FLT("SPEED", 5, K_SPEED, sh.k_speed),
    /* 30 SHERMAN IN */
    FLT("INPUT", 24, K_LIN, sh.k_in),    INT("HI", 80, K_HI3, sh.hi_sw),
    INT("LIMIT", 81, K_BOOL, sh.limit),  FLT("BLEED", 90, K_LIN, sh.k_bleed),
    FLT("RATIO", 106, K_RATIO, sh.ratio),
    /* 35 MACKIE */
    FLT("TRIM", 107, K_LIN, mk.k_trim),  INT("LOW CUT", 108, K_BOOL, mk.lowcut),
    FLT("HI", 111, K_PM, mk.k_hi),       FLT("MID", 110, K_PM, mk.k_mid),
    FLT("LOW", 109, K_PM, mk.k_lo),      FLT("PAN", 18, K_PM, k_pan),
    FLT("GAIN", 112, K_LIN, mk.k_gain),  INT("MUTE", 19, K_BOOL, mute),
    /* 43 MACKIE OUT */
    FLT("MAIN", 113, K_LIN, mk.k_main),  FLT("OUT", 117, K_LIN, k_out),
    FLT("LINE", 118, K_LIN, k_line),     INT("AUTO LVL", 119, K_BOOL, autolvl),
};
#define NP ((int)(sizeof P / sizeof P[0]))
_Static_assert(sizeof P / sizeof P[0] <= 64, "def[] te klein");

typedef struct { const char *name; uint8_t first, count; } Page;
typedef struct { const char *name, *tag; VecColor color; uint8_t npages; Page pages[4]; } Block;

static const Block BLOCKS[HCUI_BLOCKS] = {
    { "TRIG", "TRIG", { 232, 230, 225 }, 1, { { "INGANG", 0, 5 } } },
    { "909", "909", { 255, 106, 43 }, 2, { { "1", 5, 4 }, { "2", 9, 4 } } },
    { "SHERMAN", "SHRMN", { 242, 194, 48 }, 4,
      { { "F1", 13, 4 }, { "F2", 17, 6 }, { "ENV", 23, 7 }, { "IN", 30, 5 } } },
    { "MACKIE", "MACKIE", { 57, 194, 176 }, 2, { { "STRIP", 35, 8 }, { "UIT", 43, 4 } } },
};
#define MACRO 13                     /* FREQ 1: B on the home screen */
static const uint8_t HOME_KEY[HCUI_BLOCKS] = { 0, 6, 13, 35 };   /* value shown per block */

int hcui_param_count(void) { return NP; }
const char *hcui_param_label(int i) { return i >= 0 && i < NP ? P[i].label : ""; }
int hcui_param_cc(int i) { return i >= 0 && i < NP ? P[i].cc : -1; }

static float fget(const Chain *c, int i) { return *(const float *)((const char *)c + P[i].off); }
static int iget(const Chain *c, int i) { return *(const int *)((const char *)c + P[i].off); }
static int r127(float v) { int x = (int)(v * 127.f + .5f); return x < 0 ? 0 : x > 127 ? 127 : x; }
static int clamp127(int x) { return x < 0 ? 0 : x > 127 ? 127 : x; }
/* HARMONICS: chain index = round(v * 11); CC for index k = smallest CC that rounds to k */
static int harm_cc(int k) { return k <= 0 ? 0 : k >= 11 ? 127 : (int)(((float)k - .5f) * 127.f / 11.f) + 1; }

int hcui_param_get(const Chain *c, int i)
{
    if (!c || i < 0 || i >= NP) return 0;
    switch (P[i].kind) {
    case K_LIN:   return r127(fget(c, i));
    case K_PM:    { int x = (int)(fget(c, i) * 63.f + (fget(c, i) < 0 ? -.5f : .5f)) + 64; return clamp127(x); }
    case K_BOOL: case K_ACCSRC: return iget(c, i) ? 127 : 0;
    case K_MODE:  return iget(c, i) ? 127 : 0;
    case K_HARM:  return harm_cc(iget(c, i));
    case K_SPEED: return r127((4.f - fget(c, i)) * (1.f / 3.f));
    case K_GMIN:  return r127((fget(c, i) - 0.005f) * 2.f);
    case K_RATIO: return fget(c, i) > 75.f ? 127 : 0;
    case K_HI3:   { int s = iget(c, i); return s <= 0 ? 0 : s == 1 ? 64 : 127; }
    }
    return 0;
}

int hcui_param_step(const Chain *c, int i, int d, int fine)
{
    int v = hcui_param_get(c, i);
    if (!d) return v;
    switch (P[i].kind) {
    case K_BOOL: case K_ACCSRC: case K_MODE: case K_RATIO: return d > 0 ? 127 : 0;
    case K_HI3: { int s = (v < 43 ? 0 : v < 86 ? 1 : 2) + (d > 0 ? 1 : -1); s = s < 0 ? 0 : s > 2 ? 2 : s; return s == 0 ? 0 : s == 1 ? 64 : 127; }
    case K_HARM: { int k = iget(c, i) + (d > 0 ? 1 : -1); k = k < 0 ? 0 : k > 11 ? 11 : k; return harm_cc(k); }
    default: return clamp127(v + d * (fine ? 1 : 4));
    }
}

static int is_switch(int i)
{
    int k = P[i].kind;
    return k == K_BOOL || k == K_ACCSRC || k == K_MODE || k == K_RATIO || k == K_HI3;
}

/* ---- text helpers (no libc) ---- */
static char *put_int(char *d, int n)
{
    char r[12]; int k = 0;
    if (n < 0) { *d++ = '-'; n = -n; }
    do { r[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) *d++ = r[--k];
    *d = 0; return d;
}
static char *put_str(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

void hcui_param_text(const Chain *c, int i, char *o)
{
    static const char *const HARM[12] = { "FREE", "1", "1.5", "2", "3", "4", "5", "6", "7", "8", "9", "16" };
    o[0] = 0;
    if (!c || i < 0 || i >= NP) { put_str(o, "--"); return; }
    int v = hcui_param_get(c, i);
    switch (P[i].kind) {
    case K_LIN:   put_int(o, (v * 100 + 63) / 127); return;
    case K_PM:    { int x = v - 64; if (x > 0) *o++ = '+'; put_int(o, x); return; }
    case K_BOOL:  put_str(o, v ? "AAN" : "UIT"); return;
    case K_ACCSRC: put_str(o, v ? "VAST" : "HIT"); return;
    case K_MODE:  { int m = iget(c, i); put_str(o, m == 0 ? "TRIG" : m == 1 ? "AUDIO" : "AUD+909"); return; }
    case K_HARM:  { int k = iget(c, i); put_str(o, HARM[k < 0 ? 0 : k > 11 ? 11 : k]); return; }
    case K_SPEED: { char *e = put_str(o, "X"); int t = (int)(fget(c, i) * 10.f + .5f); e = put_int(e, t / 10); e = put_str(e, "."); put_int(e, t % 10); return; }
    case K_GMIN:  { char *e = put_int(o, (int)(fget(c, i) * 1000.f + .5f)); put_str(e, "MS"); return; }
    case K_RATIO: put_str(o, v ? "100:1" : "50:1"); return;
    case K_HI3:   put_str(o, v < 43 ? "CUT" : v < 86 ? "FLAT" : "BOOST"); return;
    }
}

/* ---- state machine ---- */
void hcui_init(HcUi *u)
{
    char *p = (char *)u;
    for (unsigned i = 0; i < sizeof *u; i++) p[i] = 0;
    u->dirty = 1;
}

static void emit(HcUiOut *o, int cc, int v)
{
    for (int k = 0; k < o->n; k++) if (o->cc[k] == cc) { o->val[k] = (uint8_t)v; return; }
    if (o->n < HCUI_MAX_OUT) { o->cc[o->n] = (uint8_t)cc; o->val[o->n] = (uint8_t)v; o->n++; }
}

static int sel_param(const HcUi *u)
{
    if (u->screen == HCUI_HOME) return MACRO;
    const Page *pg = &BLOCKS[u->block].pages[u->page];
    return pg->first + (u->row < pg->count ? u->row : 0);
}

static void say(HcUi *u, uint32_t now, const char *m) { u->msg = m; u->msg_until = now + 1200u; }

static void act_a_turn(HcUi *u, int d)
{
    if (u->screen == HCUI_HOME) { int b = ((int)u->block + d) % HCUI_BLOCKS; u->block = (uint8_t)(b < 0 ? b + HCUI_BLOCKS : b); }
    else { int n = BLOCKS[u->block].pages[u->page].count, r = (int)u->row + d; u->row = (uint8_t)(r < 0 ? 0 : r >= n ? n - 1 : r); }
}
static void act_a_press(HcUi *u)
{
    if (u->screen == HCUI_HOME) { u->screen = HCUI_BLOCK; u->page = 0; u->row = 0; }
    else { u->page = (uint8_t)((u->page + 1) % BLOCKS[u->block].npages); u->row = 0; }
}

void hcui_input(HcUi *u, const Chain *c, uint32_t now, int da, int db, int a, int b, HcUiOut *out)
{
    out->n = 0;
    if (c && !u->have_def) {
        for (int i = 0; i < NP; i++) u->def[i] = (uint8_t)hcui_param_get(c, i);
        u->have_def = 1;
    }
    /* turns */
    if (da) { act_a_turn(u, da); u->dirty = 1; }
    if (db && c) {
        int i = sel_param(u);
        int v = hcui_param_step(c, i, db, u->fine);
        emit(out, P[i].cc, v); u->dirty = 1;
    }
    /* buttons: edges, long presses, the A+B combination */
    int was_a = u->a_down, was_b = u->b_down;
    if (a && !was_a) { u->a_down = 1; u->a_t0 = now; u->a_long = 0; }
    if (b && !was_b) { u->b_down = 1; u->b_t0 = now; u->b_long = 0; }
    if (u->a_down && u->b_down && !u->combo) { u->combo = 1; u->combo_long = 0; u->ab_t0 = now; }
    if (u->combo) {
        if (!u->combo_long && a && b && now - u->ab_t0 >= HCUI_HOLD_MS) {
            u->combo_long = 1; say(u, now, "PRESETS: FASE C"); u->dirty = 1;
        }
        if (!a) u->a_down = 0;
        if (!b) u->b_down = 0;
        if (!u->a_down && !u->b_down) {             /* both released: combo ends */
            if (!u->combo_long) { u->bypass ^= 1; say(u, now, u->bypass ? "BYPASS" : "KETEN AAN"); }
            u->combo = 0; u->dirty = 1;
        }
        return;
    }
    if (u->a_down) {
        if (!u->a_long && now - u->a_t0 >= HCUI_HOLD_MS) { u->a_long = 1; u->screen = HCUI_HOME; u->dirty = 1; }
        if (!a) { u->a_down = 0; if (!u->a_long) { act_a_press(u); u->dirty = 1; } }
    }
    if (u->b_down) {
        int i = sel_param(u);
        if (!u->b_long && now - u->b_t0 >= HCUI_HOLD_MS) {
            u->b_long = 1;
            if (c && u->have_def) emit(out, P[i].cc, P[i].kind == K_PM ? 64 : u->def[i]);
            u->dirty = 1;
        }
        if (!b) {
            u->b_down = 0;
            if (!u->b_long) {
                if (c && is_switch(i)) {
                    int v = hcui_param_get(c, i);
                    emit(out, P[i].cc, P[i].kind == K_HI3 ? (v < 43 ? 64 : v < 86 ? 127 : 0) : (v ? 0 : 127));
                } else u->fine ^= 1;
                u->dirty = 1;
            }
        }
    }
}

uint32_t hcui_signature(const HcUi *u, const Chain *c, uint32_t now)
{
    uint32_t h = 2166136261u;
#define MIX(x) (h = (h ^ (uint32_t)(x)) * 16777619u)
    MIX(u->screen); MIX(u->block); MIX(u->page); MIX(u->row); MIX(u->fine); MIX(u->bypass);
    MIX(c != 0); MIX(u->msg && now < u->msg_until); MIX((uintptr_t)u->msg);
    if (c) {
        MIX(c->trig_count != u->trig_seen || now < u->trig_flash_until);
        for (int i = 0; i < NP; i++) MIX(hcui_param_get(c, i));
        MIX(c->mode); MIX(c->sh.harm);
    }
#undef MIX
    return h;
}

/* ---- drawing ---- */
static const VecColor DIM = { 74, 77, 82 }, TXT = { 232, 230, 225 }, MUTED = { 155, 152, 143 };
static const VecColor RED = { 255, 70, 60 };

static void rect(VecFb *fb, float x, float y, float w, float h, VecColor c, float I)
{
    vec_line(fb, x, y, x + w, y, c, I); vec_line(fb, x + w, y, x + w, y + h, c, I);
    vec_line(fb, x + w, y + h, x, y + h, c, I); vec_line(fb, x, y + h, x, y, c, I);
}
static void text_r(VecFb *fb, float xr, float y, float h, const char *s, VecColor c, float I)
{
    vec_text(fb, xr - vec_text_width(s, h), y, h, 0.f, s, c, I, 0);
}
/* value bar: track + level, bipolar from the centre */
static void bar(VecFb *fb, float x, float y, float w, int v, int bipolar, VecColor c, float I)
{
    vec_line(fb, x, y, x + w, y, DIM, .5f);
    float p = x + w * (float)v / 127.f;
    if (bipolar) { float m = x + w * .5f; vec_line(fb, m, y - 2.f, m, y + 2.f, MUTED, .6f); vec_line(fb, m, y, p, y, c, I); }
    else vec_line(fb, x, y, p, y, c, I);
    vec_line(fb, p, y - 3.f, p, y + 3.f, TXT, I);
}

/* small vector pictures for the home screen, in a w x h box */
static void icon(VecFb *fb, int blk, float x, float y, float w, float h, VecColor c, float I)
{
    static const float KICK[] = { 0,.5f, .07f,.1f, .14f,.92f, .22f,.15f, .3f,.85f, .4f,.22f, .52f,.76f, .66f,.32f, .82f,.62f, 1,.5f };
    static const float FILT[] = { 0,.62f, .25f,.6f, .42f,.55f, .5f,.35f, .56f,.12f, .62f,.2f, .7f,.55f, .82f,.8f, 1,.95f };
    if (blk == HCUI_B_909 || blk == HCUI_B_SHERMAN) {
        const float *pt = blk == HCUI_B_909 ? KICK : FILT; int n = blk == HCUI_B_909 ? 10 : 9;
        for (int k = 1; k < n; k++)
            vec_line(fb, x + pt[2 * k - 2] * w, y + pt[2 * k - 1] * h, x + pt[2 * k] * w, y + pt[2 * k + 1] * h, c, I);
    } else if (blk == HCUI_B_MACKIE) {
        static const float LV[5] = { .2f, .55f, .9f, .4f, .7f };
        for (int k = 0; k < 5; k++) { float bx = x + w * (.1f + .2f * (float)k); vec_line(fb, bx, y + h, bx, y + h * (1.f - LV[k]), c, I); }
    } else {
        static const float SP[] = { .02f, .3f, .27f, .55f, .8f };
        vec_line(fb, x, y + h * .75f, x + w, y + h * .75f, c, I * .5f);
        for (int k = 0; k < 5; k++) { float sx = x + w * (SP[k] + .08f); vec_line(fb, sx, y + h * .75f, sx, y + h * (k % 2 ? .3f : .05f), c, I); }
    }
}

int hcui_render(HcUi *u, VecFb *fb, const Chain *c, uint32_t now)
{
    float W = (float)fb->w, H = (float)fb->h;
    int anim = 0;
    vec_clear(fb);

    /* header: name, where we are, trigger lamp, state */
    vec_text(fb, 6.f, 5.f, 9.f, .15f, "NHC", BLOCKS[HCUI_B_909].color, 1.f, 0);
    if (u->screen == HCUI_BLOCK) {
        const Block *bk = &BLOCKS[u->block];
        char t[24]; char *e = put_str(t, bk->name);
        if (bk->npages > 1) { e = put_str(e, " "); put_str(e, bk->pages[u->page].name); }
        vec_text(fb, 40.f, 5.f, 9.f, 0.f, t, bk->color, 1.f, 0);
    }
    if (c && c->trig_count != u->trig_seen) { u->trig_seen = c->trig_count; u->trig_flash_until = now + 120u; }
    float lx = W - 14.f, ly = 9.5f;
    int lit = now < u->trig_flash_until;
    VecColor lc = lit ? BLOCKS[HCUI_B_909].color : DIM;
    vec_line(fb, lx - 4.f, ly, lx, ly - 4.f, lc, 1.f); vec_line(fb, lx, ly - 4.f, lx + 4.f, ly, lc, 1.f);
    vec_line(fb, lx + 4.f, ly, lx, ly + 4.f, lc, 1.f); vec_line(fb, lx, ly + 4.f, lx - 4.f, ly, lc, 1.f);
    if (lit) { vec_line(fb, lx - 2.f, ly, lx + 2.f, ly, TXT, 1.f); anim = 1; }
    if (u->bypass) text_r(fb, lx - 10.f, 5.f, 9.f, "BYPASS", RED, 1.f);
    else if (u->fine) text_r(fb, lx - 10.f, 5.f, 9.f, "FIJN", MUTED, 1.f);
    vec_line(fb, 4.f, 21.f, W - 4.f, 21.f, DIM, .6f);

    char val[16];
    if (u->screen == HCUI_HOME) {
        /* the chain: four blocks (2 x 2 on a portrait panel) */
        int cols = W >= H ? 4 : 2, rows = HCUI_BLOCKS / cols;
        float gap = 6.f, bx0 = 8.f, by0 = 30.f;
        float bw = (W - 2.f * bx0 - gap * (float)(cols - 1)) / (float)cols;
        float bh = (H - by0 - 58.f - gap * (float)(rows - 1)) / (float)rows;
        if (bh > 160.f) bh = 160.f;
        for (int k = 0; k < HCUI_BLOCKS; k++) {
            const Block *bk = &BLOCKS[k];
            float x = bx0 + (bw + gap) * (float)(k % cols), y = by0 + (bh + gap) * (float)(k / cols);
            int f = k == u->block;
            rect(fb, x, y, bw, bh, f ? bk->color : DIM, f ? 1.f : .55f);
            if (f) rect(fb, x + 2.f, y + 2.f, bw - 4.f, bh - 4.f, bk->color, .35f);
            vec_text(fb, x + 5.f, y + 6.f, 8.f, 0.f, bk->tag, bk->color, f ? 1.f : .7f, 0);
            icon(fb, k, x + 6.f, y + 20.f, bw - 12.f, bh * .34f, bk->color, f ? .9f : .5f);
            int pi = HOME_KEY[k];
            vec_text(fb, x + 5.f, y + bh - 30.f, 7.f, 0.f, P[pi].label, MUTED, .8f, 0);
            hcui_param_text(c, pi, val);
            vec_text(fb, x + 5.f, y + bh - 18.f, 10.f, 0.f, val, TXT, 1.f, 0);
            if (k < HCUI_BLOCKS - 1 && cols == 4)
                vec_text(fb, x + bw + 1.f, y + bh * .5f - 3.f, 6.f, 0.f, ">", DIM, .8f, 0);
        }
        /* B = macro (FREQ 1) */
        float my = H - 46.f;
        rect(fb, 8.f, my, W - 16.f, 22.f, DIM, .5f);
        vec_text(fb, 14.f, my + 7.f, 8.f, 0.f, "B", BLOCKS[HCUI_B_SHERMAN].color, 1.f, 0);
        vec_text(fb, 26.f, my + 7.f, 8.f, 0.f, P[MACRO].label, TXT, 1.f, 0);
        hcui_param_text(c, MACRO, val);
        text_r(fb, W - 14.f, my + 7.f, 8.f, val, TXT, 1.f);
        bar(fb, 90.f, my + 11.f, W - 150.f, hcui_param_get(c, MACRO), 0, BLOCKS[HCUI_B_SHERMAN].color, 1.f);
        vec_text(fb, 8.f, H - 15.f, 6.f, 0.f, "A: BLOK  A DRUK: OPEN  A+B: BYPASS", MUTED, .7f, 0);
    } else {
        const Block *bk = &BLOCKS[u->block];
        const Page *pg = &bk->pages[u->page];
        float rh = (H - 30.f - 22.f) / 8.f; if (rh > 30.f) rh = 30.f;
        for (int r = 0; r < pg->count; r++) {
            int pi = pg->first + r, s = r == u->row;
            float y = 30.f + rh * (float)r;
            float I = s ? 1.f : .6f;
            if (s) { vec_text(fb, 4.f, y + 2.f, 8.f, 0.f, ">", bk->color, 1.f, 0); rect(fb, 14.f, y - 2.f, W - 20.f, rh - 3.f, bk->color, .35f); }
            vec_text(fb, 18.f, y + 2.f, 8.f, 0.f, P[pi].label, s ? TXT : MUTED, I, 0);
            hcui_param_text(c, pi, val);
            text_r(fb, W - 12.f, y + 2.f, 8.f, val, s ? TXT : MUTED, I);
            if (!is_switch(pi) && P[pi].kind != K_HARM)
                bar(fb, W * .42f, y + 6.f, W * .32f, hcui_param_get(c, pi), P[pi].kind == K_PM, bk->color, I);
        }
        /* page dots */
        for (int k = 0; k < bk->npages; k++) {
            float dx = W - 12.f - 10.f * (float)(bk->npages - 1 - k), dy = H - 12.f;
            vec_line(fb, dx - 2.f, dy, dx + 2.f, dy, k == u->page ? bk->color : DIM, 1.f);
        }
        char cc[16]; char *e = put_str(cc, "CC "); put_int(e, P[sel_param(u)].cc);
        vec_text(fb, 8.f, H - 15.f, 6.f, 0.f, cc, MUTED, .7f, 0);
        vec_text(fb, 52.f, H - 15.f, 6.f, 0.f, "A: KIES  A DRUK: PAGINA  A LANG: HOME", MUTED, .7f, 0);
    }

    if (c == 0) text_r(fb, W - 8.f, H - 30.f, 7.f, "KETEN START...", MUTED, .8f);
    if (u->msg && now < u->msg_until) {
        float mw = vec_text_width(u->msg, 14.f), mx = (W - mw) * .5f, my = H * .5f - 12.f;
        rect(fb, mx - 10.f, my - 8.f, mw + 20.f, 30.f, TXT, .9f);
        vec_text(fb, mx, my, 14.f, 0.f, u->msg, TXT, 1.f, 0);
        anim = 1;
    }
    u->dirty = 0;
    return anim;
}
