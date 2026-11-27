/* vec.c - measurements of the vector engine and the boot screen for check.py */
#include <stdio.h>
#include "../ui/vec.h"

#define STRIDE (VEC_W + 16)
#define ROWS (VEC_H + 8)
static uint16_t buf[STRIDE * ROWS];

static int lit(unsigned v) { return v != 0; }
static int orange(unsigned v) { unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; return r > 14 && r * 2 > g && b < r / 2 + 2; }

int main(void)
{
    VecFb fb = { buf + 4 * STRIDE, VEC_W, VEC_H, STRIDE };
    /* guard: everything outside the 320x240 window must stay 0x5a5a */
    for (int i = 0; i < STRIDE * ROWS; i++) buf[i] = 0x5a5a;
    vec_boot(&fb, 3600);
    int guard = 1;
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < STRIDE; x++) {
        int in = y >= 4 && y < 4 + VEC_H && x < VEC_W;
        if (!in && buf[y * STRIDE + x] != 0x5a5a) guard = 0;
    }
    /* a line far outside the screen must not write anything */
    vec_line(&fb, -500, -500, -400, -450, (VecColor){255,255,255}, 1.f);
    vec_line(&fb, 900, 100, 2000, 300, (VecColor){255,255,255}, 1.f);
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < STRIDE; x++) {
        int in = y >= 4 && y < 4 + VEC_H && x < VEC_W;
        if (!in && buf[y * STRIDE + x] != 0x5a5a) guard = 0;
    }
    printf("guard %d\n", guard);

    /* title box (orange pixels) on the finished screen */
    vec_boot(&fb, 3600);
    int x0 = VEC_W, x1 = -1, y0 = VEC_H, y1 = -1, n = 0;
    for (int y = 0; y < VEC_H; y++) for (int x = 0; x < VEC_W; x++)
        if (orange(fb.px[y * STRIDE + x])) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    printf("title %d %d %d %d %d\n", n, x0, x1, y0, y1);

    /* build-up: title pixels per time step, and nothing orange before 450 ms */
    printf("build");
    for (unsigned t = 0; t <= 3600; t += 100) {
        vec_boot(&fb, t); int c = 0;
        for (int y = 60; y < 120; y++) for (int x = 0; x < VEC_W; x++) if (orange(fb.px[y * STRIDE + x])) c++;
        printf(" %u:%d", t, c);
    }
    printf("\n");
    /* determinism */
    vec_boot(&fb, 2345); unsigned long h1 = 0; for (int i = 0; i < STRIDE * VEC_H; i++) h1 = h1 * 31 + fb.px[i];
    vec_boot(&fb, 2345); unsigned long h2 = 0; for (int i = 0; i < STRIDE * VEC_H; i++) h2 = h2 * 31 + fb.px[i];
    printf("deterministic %d\n", h1 == h2);
    /* font coverage */
    printf("glyphs %d %d %d\n", vec_has_glyphs("NANOHARDCORE BY PLIEUWSKI"),
           vec_has_glyphs("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-+:/%<>'=()!?_abcxyz"),
           vec_has_glyphs("\x7f"));
    /* a single horizontal line: peak in the centre, soft glow, nothing beyond 3 px */
    vec_clear(&fb); vec_line(&fb, 100, 100.5f, 200, 100.5f, (VecColor){0,0,255}, 1.f);
    unsigned b[10]; for (int k = 0; k < 10; k++) b[k] = fb.px[(96 + k) * STRIDE + 150] & 31;
    printf("profile"); for (int k = 0; k < 10; k++) printf(" %u", b[k]); printf("\n");
    int any = 0; (void)lit; for (int i = 0; i < STRIDE * VEC_H; i++) if (lit(fb.px[i])) any++;
    printf("lit %d\n", any);
    return 0;
}
