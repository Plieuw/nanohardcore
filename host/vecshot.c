/* vecshot.c - render vector UI frames on the host to PPM (for review and tests).
 * usage: vecshot OUTDIR [p] t_ms [t_ms ...]   -> OUTDIR/boot_<t>.ppm  (p = portrait 240x320) */
#include <stdio.h>
#include <stdlib.h>
#include "../ui/vec.h"

int main(int argc, char **argv)
{
    static uint16_t px[VEC_W * VEC_H];
    VecFb fb = { px, VEC_W, VEC_H, VEC_W, 16 };
    int a0 = 2;
    if (argc > 2 && argv[2][0] == 'p') { fb.w = VEC_H; fb.h = VEC_W; fb.stride = VEC_H; a0 = 3; }   /* portrait 240x320 */
    for (int a = a0; a < argc; a++) {
        unsigned t = (unsigned)atoi(argv[a]);
        vec_boot(&fb, t);
        char name[512]; snprintf(name, sizeof name, "%s/boot_%05u.ppm", argv[1], t);
        FILE *f = fopen(name, "wb"); if (!f) return 1;
        fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
        for (int i = 0; i < VEC_W * VEC_H; i++) {
            unsigned v = px[i];
            unsigned char c[3] = { (unsigned char)(((v >> 11) & 31) * 255 / 31),
                                   (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                   (unsigned char)((v & 31) * 255 / 31) };
            fwrite(c, 1, 3, f);
        }
        fclose(f);
    }
    return 0;
}
