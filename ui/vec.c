/* vec.c - beam renderer, stroke font and boot screen (see vec.h). */
#include "vec.h"

/* ---- small math (no libm) ---- */
static float vabs(float x) { return x < 0.f ? -x : x; }
static int vfloor(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
static float vsqrt(float x)
{
    if (x <= 0.f) return 0.f;
    union { float f; uint32_t u; } c; c.f = x;
    c.u = 0x1fbd1df5u + (c.u >> 1);                      /* first guess, then Newton */
    float y = c.f;
    y = 0.5f * (y + x / y); y = 0.5f * (y + x / y); y = 0.5f * (y + x / y);
    return y;
}
static float vclamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float smooth(float x) { x = vclamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }

void vec_clear(VecFb *fb)
{
    for (int y = 0; y < fb->h; y++) {
        if (fb->bpp == 32) { uint32_t *r = (uint32_t *)fb->px + y * fb->stride; for (int x = 0; x < fb->w; x++) r[x] = 0xff000000u; }
        else if (fb->bpp == 24) { uint8_t *r = (uint8_t *)fb->px + 3 * y * fb->stride; for (int x = 0; x < 3 * fb->w; x++) r[x] = 0; }
        else { uint16_t *r = (uint16_t *)fb->px + y * fb->stride; for (int x = 0; x < fb->w; x++) r[x] = 0; }
    }
}

void vec_clear_rect(VecFb *fb, int x0, int y0, int w, int h)
{
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->w) x1 = fb->w;
    if (y1 > fb->h) y1 = fb->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (fb->bpp == 32) ((uint32_t *)fb->px)[y * fb->stride + x] = 0xff000000u;
            else if (fb->bpp == 24) { uint8_t *q = (uint8_t *)fb->px + 3 * (y * fb->stride + x); q[0] = q[1] = q[2] = 0; }
            else ((uint16_t *)fb->px)[y * fb->stride + x] = 0;
        }
}

/* ---- the beam ----
 * Light falls off with the perpendicular distance d (pixels) to the segment:
 * core (d < 0.55) full, then a quick drop and a long faint glow up to 3 px. */
#define BEAM_REACH 3.f
static float beam(float d)
{
    if (d < 0.55f) return 1.f;
    if (d < 1.1f)  return 1.f - (d - 0.55f) * (0.55f / 0.55f);            /* 1 -> 0.45 */
    if (d < 2.f)   return 0.45f - (d - 1.1f) * (0.33f / 0.9f);            /* -> 0.12   */
    if (d < BEAM_REACH) return 0.12f - (d - 2.f) * 0.12f;                 /* -> 0      */
    return 0.f;
}

static void add_light(VecFb *fb, int x, int y, VecColor c, float w)
{
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || w <= 0.f) return;
    float r, g, b;
    uint32_t *p32 = 0; uint16_t *p16 = 0; uint8_t *p24 = 0;
    if (fb->bpp == 24) {
        p24 = (uint8_t *)fb->px + 3 * (y * fb->stride + x);
        b = (float)p24[0]; g = (float)p24[1]; r = (float)p24[2];
    } else if (fb->bpp == 32) {
        p32 = (uint32_t *)fb->px + y * fb->stride + x; uint32_t v = *p32;
        r = (float)((v >> 16) & 255); g = (float)((v >> 8) & 255); b = (float)(v & 255);
    } else {
        p16 = (uint16_t *)fb->px + y * fb->stride + x; unsigned v = *p16;
        r = (float)((v >> 11) & 31) * (255.f / 31.f);
        g = (float)((v >> 5) & 63) * (255.f / 63.f);
        b = (float)(v & 31) * (255.f / 31.f);
    }
    float hot = w > 0.8f ? (w - 0.8f) * 160.f : 0.f;     /* overdriven phosphor blooms white */
    r = vclamp(r + c.r * w + hot, 0.f, 255.f);
    g = vclamp(g + c.g * w + hot, 0.f, 255.f);
    b = vclamp(b + c.b * w + hot, 0.f, 255.f);
    if (p24) { p24[0] = (uint8_t)(b + .5f); p24[1] = (uint8_t)(g + .5f); p24[2] = (uint8_t)(r + .5f); return; }
    if (p32) { *p32 = 0xff000000u | ((uint32_t)(r + .5f) << 16) | ((uint32_t)(g + .5f) << 8) | (uint32_t)(b + .5f); return; }
    unsigned R = (unsigned)(r * (31.f / 255.f) + .5f);
    unsigned G = (unsigned)(g * (63.f / 255.f) + .5f);
    unsigned B = (unsigned)(b * (31.f / 255.f) + .5f);
    *p16 = (uint16_t)((R << 11) | (G << 5) | B);
}

void vec_line(VecFb *fb, float x0, float y0, float x1, float y1, VecColor c, float intensity)
{
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    int steep = vabs(dy) > vabs(dx);
    /* walk the major axis, light the pixels within BEAM_REACH of the segment */
    float a0 = steep ? y0 : x0, a1 = steep ? y1 : x1;
    if (a0 > a1) { float t = a0; a0 = a1; a1 = t; }
    int m0 = vfloor(a0 - BEAM_REACH), m1 = vfloor(a1 + BEAM_REACH) + 1;
    int lim = steep ? fb->h : fb->w;  /* rows/columns that exist */
    if (m0 < 0) m0 = 0;
    if (m1 > lim - 1) m1 = lim - 1;
    for (int m = m0; m <= m1; m++) {
        /* centre of the beam on this row/column (clamped to the segment) */
        float fm = (float)m, cm;
        if (steep) cm = vabs(dy) > 1e-6f ? x0 + dx * vclamp((fm - y0) / dy, 0.f, 1.f) : x0;
        else       cm = vabs(dx) > 1e-6f ? y0 + dy * vclamp((fm - x0) / dx, 0.f, 1.f) : y0;
        int k0 = vfloor(cm - BEAM_REACH), k1 = vfloor(cm + BEAM_REACH) + 1;
        for (int k = k0; k <= k1; k++) {
            float px = steep ? (float)k : fm, py = steep ? fm : (float)k;
            /* exact distance from the pixel to the segment */
            float t = l2 > 1e-9f ? vclamp(((px - x0) * dx + (py - y0) * dy) / l2, 0.f, 1.f) : 0.f;
            float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
            float d = vsqrt(ex * ex + ey * ey);
            if (d < BEAM_REACH) add_light(fb, steep ? k : m, steep ? m : k, c, intensity * beam(d));
        }
    }
}

int vec_line_b(VecFb *fb, float x0, float y0, float x1, float y1, VecColor c, float intensity,
               VecBudget *budget)
{
    if (budget) {
        if (*budget <= 0.f) return 0;
        if (*budget < 1.f) { x1 = x0 + (x1 - x0) * *budget; y1 = y0 + (y1 - y0) * *budget; }
        *budget -= 1.f;
    }
    vec_line(fb, x0, y0, x1, y1, c, intensity);
    return 1;
}

/* ---- stroke font ----
 * Own design on a 4 x 6 grid (x 0..4 right, y 0..6 down). Each glyph is a list of
 * two-digit points "xy"; consecutive points are joined, '|' lifts the pen. */
static const char *const GLYPHS[64] = {
    /* ' ' */ "",                    /* '!' */ "20 24|25 26",
    /* '"' */ "10 12|30 32",         /* '#' */ "10 16|30 36|02 42|04 44",
    /* '$' */ "40 01 03 43 45 06|20 26", /* '%' */ "06 40|00 01|45 46",
    /* '&' */ "46 01 00 20 21 05 06 26 44", /* ''' */ "20 22",
    /* '(' */ "30 12 14 36",         /* ')' */ "10 32 34 16",
    /* '*' */ "11 35|31 15|03 43",   /* '+' */ "03 43|21 25",
    /* ',' */ "25 16",               /* '-' */ "03 43",
    /* '.' */ "25 26",               /* '/' */ "06 40",
    /* '0' */ "00 40 46 06 00|40 06", /* '1' */ "11 20 26|06 46",
    /* '2' */ "00 40 43 03 06 46",   /* '3' */ "00 40 46 06|03 43",
    /* '4' */ "00 03 43|40 46",      /* '5' */ "40 00 03 43 46 06",
    /* '6' */ "40 00 06 46 43 03",   /* '7' */ "00 40 46",
    /* '8' */ "00 40 46 06 00|03 43", /* '9' */ "43 03 00 40 46 06",
    /* ':' */ "21 22|24 25",         /* ';' */ "21 22|24 16",
    /* '<' */ "40 03 46",            /* '=' */ "02 42|04 44",
    /* '>' */ "00 43 06",            /* '?' */ "01 00 40 42 23 24|25 26",
    /* '@' */ "33 13 14 34 31 41 46 06 00 40", /* 'A' */ "06 02 20 42 46|04 44",
    /* 'B' */ "06 00 30 41 42 33 03|33 44 45 36 06", /* 'C' */ "40 00 06 46",
    /* 'D' */ "06 00 20 42 44 26 06", /* 'E' */ "40 00 06 46|03 33",
    /* 'F' */ "40 00 06|03 33",      /* 'G' */ "40 00 06 46 43 23",
    /* 'H' */ "00 06|40 46|03 43",   /* 'I' */ "00 40|20 26|06 46",
    /* 'J' */ "40 46 06 04",         /* 'K' */ "00 06|40 03 46",
    /* 'L' */ "00 06 46",            /* 'M' */ "06 00 23 40 46",
    /* 'N' */ "06 00 46 40",         /* 'O' */ "00 40 46 06 00",
    /* 'P' */ "06 00 40 43 03",      /* 'Q' */ "00 40 46 06 00|23 47",
    /* 'R' */ "06 00 40 43 03|13 46", /* 'S' */ "40 00 03 43 46 06",
    /* 'T' */ "00 40|20 26",         /* 'U' */ "00 06 46 40",
    /* 'V' */ "00 26 40",            /* 'W' */ "00 06 23 46 40",
    /* 'X' */ "00 46|40 06",         /* 'Y' */ "00 23 40|23 26",
    /* 'Z' */ "00 40 06 46",         /* '[' */ "30 10 16 36",
    /* '\' */ "00 46",               /* ']' */ "10 30 36 16",
    /* '^' */ "12 20 32",            /* '_' */ "06 46",
};
#define ADVANCE 6.f      /* grid units per character (4 glyph + 2 space) */

static const char *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch < 32 || ch >= 32 + 64) return 0;
    return GLYPHS[ch - 32];
}

int vec_has_glyphs(const char *s)
{
    for (; *s; s++) if (!glyph(*s)) return 0;
    return 1;
}

float vec_text_width(const char *s, float h)
{
    int n = 0; while (s[n]) n++;
    return n ? ((float)n * ADVANCE - 2.f) * (h / 6.f) : 0.f;
}

/* walk the strokes of a string; for each segment call the visitor */
typedef struct { float x, y, s, slant; } Pen;
#define GX(p, gx, gy) ((p)->x + ((float)(gx)) * (p)->s + (6.f - (float)(gy)) * (p)->s * (p)->slant)
#define GY(p, gy) ((p)->y + ((float)(gy)) * (p)->s)

static int segs_of(const char *g)
{
    int n = 0, pts = 0;
    for (; *g; g++) {
        if (*g == '|') { pts = 0; continue; }
        if (*g >= '0' && *g <= '9' && g[1] >= '0' && g[1] <= '9') { if (pts++) n++; g++; }
    }
    return n;
}
/* vertices (for connectors of extruded text) */
static int verts_of(const char *g)
{
    int n = 0;
    for (; *g; g++) if (*g >= '0' && *g <= '9' && g[1] >= '0' && g[1] <= '9') { n++; g++; }
    return n;
}

int vec_text_segments(const char *s)
{
    int n = 0;
    for (; *s; s++) { const char *g = glyph(*s); if (g) n += segs_of(g); }
    return n;
}

static void draw_glyph(VecFb *fb, const Pen *p, const char *g, VecColor c, float I, VecBudget *b)
{
    int have = 0; int px = 0, py = 0;
    for (; *g; g++) {
        if (*g == '|') { have = 0; continue; }
        if (!(*g >= '0' && *g <= '9' && g[1] >= '0' && g[1] <= '9')) continue;
        int x = g[0] - '0', y = g[1] - '0'; g++;
        if (have) vec_line_b(fb, GX(p, px, py), GY(p, py), GX(p, x, y), GY(p, y), c, I, b);
        px = x; py = y; have = 1;
    }
}

void vec_text(VecFb *fb, float x, float y, float h, float slant, const char *s,
              VecColor c, float intensity, VecBudget *budget)
{
    Pen p = { x, y, h / 6.f, slant };
    for (; *s; s++, p.x += ADVANCE * p.s) {
        const char *g = glyph(*s);
        if (g) draw_glyph(fb, &p, g, c, intensity, budget);
    }
}

void vec_text3d(VecFb *fb, float x, float y, float h, float slant, float dx, float dy,
                const char *s, VecColor front, VecColor back, float intensity, VecBudget *budget)
{
    /* per character: back face, depth connectors, front face; so the beam builds each
     * letter as a solid before moving on */
    Pen f = { x, y, h / 6.f, slant }, k = { x + dx, y + dy, h / 6.f, slant };
    for (; *s; s++, f.x += ADVANCE * f.s, k.x += ADVANCE * k.s) {
        const char *g = glyph(*s);
        if (!g) continue;
        draw_glyph(fb, &k, g, back, intensity * .42f, budget);
        for (const char *q = g; *q; q++)
            if (*q >= '0' && *q <= '9' && q[1] >= '0' && q[1] <= '9') {
                int gx = q[0] - '0', gy = q[1] - '0'; q++;
                vec_line_b(fb, GX(&f, gx, gy), GY(&f, gy), GX(&k, gx, gy), GY(&k, gy), back, intensity * .22f, budget);
            }
        draw_glyph(fb, &f, g, front, intensity, budget);
    }
}

static int text3d_segments(const char *s)
{
    int n = 0;
    for (; *s; s++) { const char *g = glyph(*s); if (g) n += 2 * segs_of(g) + verts_of(g); }
    return n;
}

/* ---- boot screen ---- */
static const VecColor ORANGE = { 255, 106, 43 }, EMBER = { 150, 46, 14 };
static const VecColor TEAL = { 57, 194, 176 }, STAR = { 190, 210, 255 };
#define TITLE "NANOHARDCORE"
#define SUB "BY PLIEUWSKI"

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float u01(uint32_t h) { return (float)(h >> 8) * (1.f / 16777216.f); }

/* warp starfield: each star flies out from the centre; speed is high at power-up
 * (warp), then settles to a slow drift. Position is a pure function of t. */
static void stars(VecFb *fb, float t)
{
    const float cx = fb->w * .5f, cy = fb->h * .46f, k = (float)(fb->w < fb->h ? fb->w : fb->h) / 240.f;
    /* travelled distance: slow drift + a warp burst that ends around 1.1 s */
    float S = 0.00006f * t + 1.6f * smooth(t / 1100.f);
    float w = vclamp(t / 1100.f, 0.f, 1.f);
    float speed = 0.00006f + 1.6f * 6.f * w * (1.f - w) / 1100.f;   /* dS/dt */
    float trail = vclamp(speed * 90.f, 0.004f, 0.16f);   /* streak length in depth units */
    for (uint32_t i = 0; i < 72; i++) {
        uint32_t h = hash(i * 0x9e3779b9u + 0x1234567u);
        float ux = u01(h) * 2.f - 1.f, uy = u01(hash(h)) * 2.f - 1.f;
        float n = vsqrt(ux * ux + uy * uy);
        if (n < 0.08f) continue;
        ux /= n; uy /= n;
        float ph = u01(hash(h ^ 0xabcdefu)) + S;
        ph -= (float)vfloor(ph);
        float z = 1.f - ph;                               /* 1 far ... 0 at the glass */
        float z2 = z + trail; if (z2 > 1.f) z2 = 1.f;
        float r1 = 16.f * k / (z + 0.05f), r2 = 16.f * k / (z2 + 0.05f);
        /* far stars fade in (no bright knot in the centre), near ones are brightest */
        float I = smooth((1.f - z) / 0.45f) * (0.3f + 0.7f * u01(hash(h + 7u)));
        vec_line(fb, cx + ux * r2, cy + uy * r2 * .8f, cx + ux * r1, cy + uy * r1 * .8f, STAR, I);
    }
}

void vec_boot(VecFb *fb, uint32_t t_ms)
{
    float t = (float)t_ms, W = (float)fb->w, H = (float)fb->h;
    vec_clear(fb);
    stars(fb, t);

    /* title: extruded, slanted, built by the beam from 0.45 s to 1.75 s; sized to the
     * screen width (24 px on 320, smaller on a portrait panel) */
    const float slant = .2f, depth_x = 2.5f, depth_y = -3.f;
    float th = 24.f, fit = (W - 30.f) / (vec_text_width(TITLE, 1.f) + slant);
    if (th > fit) th = fit;
    float tw = vec_text_width(TITLE, th) + slant * th;
    float cy = H * .5f;
    float tx = (W - tw) * .5f, ty = cy - 36.f * th / 24.f;
    VecBudget b = (float)text3d_segments(TITLE) * smooth((t - 450.f) / 1300.f) * 1.0001f;
    if (t >= 450.f)
        vec_text3d(fb, tx, ty, th, slant, depth_x, depth_y, TITLE, ORANGE, EMBER, 1.f, &b);

    /* two horizon rules that open out from the centre (1.6 s .. 2.0 s) */
    float o = smooth((t - 1600.f) / 400.f) * (W * .5f - 28.f);
    if (o > 0.f) {
        vec_line(fb, W * .5f - o, cy + 2.f, W * .5f + o, cy + 2.f, TEAL, .8f);
        vec_line(fb, W * .5f - o * .62f, cy + 6.f, W * .5f + o * .62f, cy + 6.f, TEAL, .45f);
    }

    /* subtitle, drawn on from 1.9 s to 2.6 s */
    const float sh = 10.f;
    float sw = vec_text_width(SUB, sh);
    VecBudget sb = (float)vec_text_segments(SUB) * smooth((t - 1900.f) / 700.f) * 1.0001f;
    if (t >= 1900.f) vec_text(fb, (W - sw) * .5f, cy + 16.f, sh, 0.f, SUB, TEAL, 1.f, &sb);
}
