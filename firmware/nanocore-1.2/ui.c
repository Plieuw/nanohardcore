/* ui.c - naam, labels, waardeweergave en defaults voor het scherm van de pedal.
 * De symboolnamen zijn die waar device_ui_120.S (re-nanocore, MIT) naar springt. */
#include <stdint.h>
#include "../../dsp/chain.h"

extern volatile uint32_t nc_octave_fault;

const float nc_octave_defaults_values[11] = {
    0.5f, 0.5f, 0.5f, 0.333f, 0.75f, 0.35f, 0.0f, 0.75f, 0.0f, 0.0f, 0.8f };
_Static_assert(CHAIN_DEV_CONTROLS == 11, "UI-haken verwachten 11 knoppen");

const char nc_octave_name[] = "HARDCORE";

static const char *const LABELS[11] = {
    "SENS", "TUNE", "DECAY", "DRIVE", "FREQ", "RESO", "L-B-H", "ENV>F", "HARM", "INPUT", "LEVEL" };

const char *nc_octave_label(unsigned a, unsigned b, unsigned index, unsigned c)
{
    (void)a; (void)b; (void)c;
    return index < 11 ? LABELS[index] : "";
}

static float bounded(float v, unsigned i)
{
    if (!(v == v) || v > 3.0e38f || v < -3.0e38f) return nc_octave_defaults_values[i];
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

static unsigned put_uint(char *d, unsigned n)
{
    char r[10]; unsigned k = 0, j = 0;
    do { r[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) d[j++] = r[--k];
    return j;
}

int nc_octave_format(unsigned a, unsigned b, unsigned index, char *out, unsigned size,
                     const void *ctx, float value)
{
    (void)a; (void)b; (void)ctx;
    if (!out || !size) return -1;
    static const char *const HARM[4]  = { "FREE", "x2", "x4", "x16" };
    static const char *const INPUT[4] = { "909", "909+DRY", "AUDIO", "AUD+909" };
    static const char *const LBH[3]   = { "LP", "BP", "HP" };
    char digits[16]; const char *text = "";
    if (index < 11) {
        value = bounded(value, index);
        if (nc_octave_fault) text = nc_octave_fault == 2 ? "CHAIN LIMIT" : "CPU LIMIT";
        else if (index == 8) text = HARM[CHAIN_DEV_CHOICE(value)];
        else if (index == 9) text = INPUT[CHAIN_DEV_CHOICE(value)];
        else if (index == 6 && (value == 0.f || value == .5f || value == 1.f))
            text = LBH[(int)(value * 2.f + .5f)];
        else {
            unsigned j = 0;
            if (index == 7) {                               /* ENV>F: -100 .. +100, 0 exact */
                float e = 200.f * value - 100.f;
                if (e < 0) { digits[j++] = '-'; e = -e; } else if (e > 0) digits[j++] = '+';
                j += put_uint(digits + j, (unsigned)(e + .5f));
            } else {
                j += put_uint(digits + j, (unsigned)(value * 100.f + .5f));
                digits[j++] = '%';
            }
            digits[j] = 0; text = digits;
        }
    }
    unsigned i = 0; while (i + 1 < size && text[i]) { out[i] = text[i]; i++; } out[i] = 0;
    /* native UI: 0 = gelukt; anders overschrijft de originele formatter de tekst */
    return index < 11 ? 0 : -1;
}
