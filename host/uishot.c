/* uishot.c - render the HARDCORE UI screens on the host (PPM) for review.
 * usage: uishot OUTDIR   -> home.ppm, blok_<b>_<p>.ppm, portret_home.ppm */
#include <stdio.h>
#include "../ui/hcui.h"

static uint16_t px[320 * 320];
static void save(const char *dir, const char *name, int w, int h)
{
    char f[512]; snprintf(f, sizeof f, "%s/%s.ppm", dir, name);
    FILE *o = fopen(f, "wb"); if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { unsigned v = px[i]; unsigned char c[3] = { (unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31) }; fwrite(c, 1, 3, o); }
    fclose(o);
}
int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    static Chain c; chain_init(&c, 48000.f); chain_device_controls(&c, CHAIN_DEV_DEFAULTS);
    VecFb fb = { px, 320, 240, 320, 16 };
    HcUi u; hcui_init(&u); u.block = HCUI_B_909;
    hcui_render(&u, &fb, &c, 0); save(argv[1], "home", 320, 240);
    static const int np[HCUI_BLOCKS] = { 1, 2, 4, 2 };
    for (int b = 0; b < HCUI_BLOCKS; b++) for (int p = 0; p < np[b]; p++) {
        u.screen = HCUI_BLOCK; u.block = (uint8_t)b; u.page = (uint8_t)p; u.row = 1;
        char n[32]; snprintf(n, sizeof n, "blok_%d_%d", b, p);
        hcui_render(&u, &fb, &c, 0); save(argv[1], n, 320, 240);
    }
    VecFb pf = { px, 240, 320, 240, 16 };
    u.screen = HCUI_HOME; u.block = HCUI_B_SHERMAN; u.bypass = 1;
    hcui_render(&u, &pf, &c, 0); save(argv[1], "portret_home", 240, 320);
    return 0;
}
