/* vec.h - vector display engine for the NanoCore screen (320x240).
 *
 * Everything on screen is built from line segments drawn with a glowing "beam", like
 * an XY vector monitor: no bitmaps, no bitmap font. A segment adds light (saturating)
 * with a soft profile around its centre line, so crossings and polyline joints glow
 * brighter, as on the real tube.
 *
 * Freestanding C99 like dsp/: no libm, no malloc, no writable statics. Pixels are
 * RGB565, RGB888 or XRGB8888 (whatever aicfb reports at run time); layouts use fb->w/fb->h,
 * so a portrait 240x320 panel works as well as 320x240.
 */
#ifndef VEC_H_INCLUDED
#define VEC_H_INCLUDED
#include <stdint.h>

#define VEC_W 320      /* reference size of the design */
#define VEC_H 240

typedef struct {
    void *px;          /* row-major pixels */
    int w, h, stride;  /* stride in pixels */
    int bpp;           /* 16 = RGB565, 24 = RGB888 (bytes B,G,R), 32 = XRGB8888 (0xAARRGGBB); 0 = 16 */
} VecFb;

typedef struct { uint8_t r, g, b; } VecColor;

/* draw budget: how many segments may still be drawn (fractional = the last one is
 * drawn partly, like a beam that is still travelling). NULL = no limit. */
typedef float VecBudget;

void vec_clear(VecFb *fb);
/* one beam segment, coordinates in pixels (float, sub-pixel exact), intensity 0..1+ */
void vec_line(VecFb *fb, float x0, float y0, float x1, float y1, VecColor c, float intensity);
/* same, honouring a draw budget; returns 1 if (part of) it was drawn */
int vec_line_b(VecFb *fb, float x0, float y0, float x1, float y1, VecColor c, float intensity,
               VecBudget *budget);

/* stroke font: caps, digits and . - + : / % < > ' = ( ) ! ? _ (lowercase maps to caps).
 * h = cap height in pixels, slant = x shift per pixel of height (0 = upright). */
float vec_text_width(const char *s, float h);
void vec_text(VecFb *fb, float x, float y, float h, float slant, const char *s,
              VecColor c, float intensity, VecBudget *budget);
/* extruded text: back face at (dx,dy) dimmer, connectors at every vertex, then the front */
void vec_text3d(VecFb *fb, float x, float y, float h, float slant, float dx, float dy,
                const char *s, VecColor front, VecColor back, float intensity, VecBudget *budget);
/* number of segments a string costs (for draw-on timing) */
int vec_text_segments(const char *s);
/* 1 if every character of s has a glyph (space counts as one) */
int vec_has_glyphs(const char *s);

/* boot screen: deterministic function of time since power-up of the UI (ms).
 * Starfield warp, NANOHARDCORE drawn stroke by stroke in extruded vector letters,
 * then BY PLIEUWSKI. Fully built at VEC_BOOT_BUILT_MS, holds (with slow stars) after. */
#define VEC_BOOT_BUILT_MS 2600u
#define VEC_BOOT_MS 3600u
void vec_boot(VecFb *fb, uint32_t t_ms);
#endif
