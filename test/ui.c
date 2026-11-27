/* ui.c - HARDCORE UI: parameter round trips, gestures and drawing bounds (for check.py) */
#include <stdio.h>
#include <string.h>
#include "../ui/hcui.h"

static Chain C;
static void apply(const HcUiOut *o) { for (int k = 0; k < o->n; k++) chain_midi(&C, 0xB0, o->cc[k], o->val[k]); }
static int find(const char *lbl, int cc) { for (int i = 0; i < hcui_param_count(); i++) if (hcui_param_cc(i) == cc && !strcmp(hcui_param_label(i), lbl)) return i; return -1; }

#define STRIDE 336
static uint16_t B16[STRIDE * 340];
static uint32_t B32[STRIDE * 340];

static int guard_ok(int w, int h, int bpp)
{
    for (int y = 0; y < 340; y++) for (int x = 0; x < STRIDE; x++) {
        int in = y >= 4 && y < 4 + h && x < w;
        if (!in && (bpp == 32 ? B32[y * STRIDE + x] != 0x5a5a5a5au : B16[y * STRIDE + x] != 0x5a5a)) return 0;
    }
    return 1;
}

int main(void)
{
    chain_init(&C, 48000.f); chain_device_controls(&C, CHAIN_DEV_DEFAULTS);
    /* 1. every parameter: value -> CC -> chain -> value */
    int bad = 0, glyph_bad = 0, n = hcui_param_count();
    for (int i = 0; i < n; i++) {
        int cc = hcui_param_cc(i);
        if (!vec_has_glyphs(hcui_param_label(i))) glyph_bad++;
        for (int v = 0; v <= 127; v++) {
            int s = hcui_param_step(&C, i, v > hcui_param_get(&C, i) ? 1 : -1, 1);  /* canonical values */
            (void)s;
            chain_midi(&C, 0xB0, (uint8_t)cc, (uint8_t)v);
            int g = hcui_param_get(&C, i);
            chain_midi(&C, 0xB0, (uint8_t)cc, (uint8_t)g);           /* re-sending what we show */
            if (hcui_param_get(&C, i) != g) { bad++; printf("  instabiel %s v=%d g=%d\n", hcui_param_label(i), v, g); break; }
            char t[16]; hcui_param_text(&C, i, t); if (!vec_has_glyphs(t)) glyph_bad++;
        }
    }
    /* continuous kinds must be exact for every CC */
    int exact_bad = 0;
    const char *cont[] = { "SENS", "TUNE", "FREQ 1", "CORR 1", "PAN", "TRIM", "SPEED", "GATE" };
    for (unsigned k = 0; k < sizeof cont / sizeof *cont; k++) {
        int i = -1; for (int j = 0; j < n; j++) if (!strcmp(hcui_param_label(j), cont[k])) { i = j; break; }
        for (int v = 0; v <= 127; v++) { chain_midi(&C, 0xB0, (uint8_t)hcui_param_cc(i), (uint8_t)v); if (hcui_param_get(&C, i) != v) { exact_bad++; break; } }
    }
    printf("params %d unstable %d inexact %d glyphs_missing %d\n", n, bad, exact_bad, glyph_bad);

    /* 2. gestures */
    chain_init(&C, 48000.f); chain_device_controls(&C, CHAIN_DEV_DEFAULTS);
    HcUi u; HcUiOut o; hcui_init(&u); uint32_t t = 0;
#define STEP(da, db, a, b) do { t += 16; hcui_input(&u, &C, t, da, db, a, b, &o); apply(&o); } while (0)
    STEP(0, 0, 0, 0);
    int f1 = find("FREQ 1", 25), f1_0 = hcui_param_get(&C, f1);
    STEP(0, 2, 0, 0); int f1_coarse = hcui_param_get(&C, f1) - f1_0;
    STEP(0, 0, 0, 1); STEP(0, 0, 0, 0);                     /* B press: fine */
    int fine = u.fine;
    int before = hcui_param_get(&C, f1); STEP(0, 1, 0, 0); int f1_fine = hcui_param_get(&C, f1) - before;
    for (int k = 0; k < 45; k++) STEP(0, 0, 0, 1);          /* B hold ~0.7 s: back to start value */
    STEP(0, 0, 0, 0); int f1_reset = hcui_param_get(&C, f1) == f1_0;
    STEP(1, 0, 0, 0); int blk1 = u.block;                   /* A turn: next block */
    STEP(-2, 0, 0, 0); int blk_wrap = u.block;              /* 1 - 2 -> 3 (wraps) */
    STEP(0, 0, 1, 0); STEP(0, 0, 0, 0); int opened = u.screen == HCUI_BLOCK;   /* A press: open MACKIE */
    STEP(1, 0, 0, 0); STEP(0, 0, 0, 1); STEP(0, 0, 0, 0);   /* row 1 = LOW CUT, B press toggles */
    int lc = C.mk.lowcut;
    STEP(0, 0, 1, 0); STEP(0, 0, 0, 0); int page2 = u.page == 1;  /* A press: next page */
    for (int k = 0; k < 45; k++) STEP(0, 0, 1, 0);          /* A hold: home */
    STEP(0, 0, 0, 0); int home = u.screen == HCUI_HOME;
    STEP(0, 0, 1, 1); STEP(0, 0, 0, 0); int byp = u.bypass; /* A+B press: bypass */
    STEP(0, 0, 1, 1); for (int k = 0; k < 45; k++) STEP(0, 0, 1, 1);
    STEP(0, 0, 0, 0); int byp_after_long = u.bypass;        /* A+B hold: no toggle */
    /* bipolar reset = exact 0: MACKIE strip HI */
    u.screen = HCUI_BLOCK; u.block = HCUI_B_MACKIE; u.page = 0; u.row = 2;
    STEP(0, 9, 0, 0); for (int k = 0; k < 45; k++) STEP(0, 0, 0, 1); STEP(0, 0, 0, 0);
    printf("gestures coarse %d fine %d fine_on %d reset %d blk %d wrap %d open %d lowcut %d page2 %d home %d bypass %d bypass_long %d hi_zero %d\n",
           f1_coarse, f1_fine, fine, f1_reset, blk1, blk_wrap, opened, lc, page2, home, byp, byp_after_long, C.mk.k_hi == 0.f);

    /* 3. drawing: every screen, 16 and 32 bit, landscape and portrait, no writes outside */
    int gbad = 0, screens = 0;
    for (int orient = 0; orient < 2; orient++) for (int bpp = 16; bpp <= 32; bpp += 16) {
        int w = orient ? 240 : 320, h = orient ? 320 : 240;
        for (int i = 0; i < STRIDE * 340; i++) { B16[i] = 0x5a5a; B32[i] = 0x5a5a5a5au; }
        VecFb fb = { bpp == 32 ? (void *)(B32 + 4 * STRIDE) : (void *)(B16 + 4 * STRIDE), w, h, STRIDE, bpp };
        HcUi v; hcui_init(&v); v.msg = "BYPASS"; v.msg_until = 99999;
        hcui_render(&v, &fb, &C, 10); screens++;
        for (int b = 0; b < HCUI_BLOCKS; b++) for (int p = 0; p < 4; p++) {
            v.screen = HCUI_BLOCK; v.block = (uint8_t)b; v.page = (uint8_t)p; v.row = 0;
            if (p >= (b == 2 ? 4 : b == 0 ? 1 : 2)) continue;
            hcui_render(&v, &fb, &C, 10); screens++;
        }
        vec_boot(&fb, 3000); screens++;
        if (!guard_ok(w, h, bpp)) gbad++;
    }
    printf("draw screens %d outside %d\n", screens, gbad);
    return 0;
}
