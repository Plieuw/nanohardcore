/* hcui.h - the HARDCORE user interface: two push encoders (A = where, B = how much),
 * all controls of the chain, drawn with the vector engine (vec.h).
 *
 * Values are read from the running Chain (always the truth, also after MIDI) and
 * changed by emitting MIDI CCs, which the audio side applies between two blocks.
 * Freestanding C99 like dsp/ and ui/vec.c.
 */
#ifndef HCUI_H
#define HCUI_H
#include <stdint.h>
#include "vec.h"
#include "../dsp/chain.h"

#define HCUI_HOLD_MS 600u        /* long press */
#define HCUI_MAX_OUT 16

enum { HCUI_HOME = 0, HCUI_BLOCK = 1 };
enum { HCUI_B_TRIG, HCUI_B_909, HCUI_B_SHERMAN, HCUI_B_MACKIE, HCUI_BLOCKS };

typedef struct {
    uint8_t screen, block, page, row, fine, bypass;
    /* buttons: press time, long-press already fired, combo (A+B) in progress */
    uint8_t a_down, b_down, a_long, b_long, combo, combo_long;
    uint32_t a_t0, b_t0, ab_t0;
    uint32_t trig_seen, trig_flash_until, msg_until;
    const char *msg;
    uint8_t have_def;
    uint8_t def[64];             /* value of every parameter at start-up (B hold = back) */
    uint32_t dirty;              /* != 0: screen must be redrawn */
} HcUi;

typedef struct {
    uint8_t n;                   /* CCs to send */
    uint8_t cc[HCUI_MAX_OUT], val[HCUI_MAX_OUT];
} HcUiOut;

void hcui_init(HcUi *u);
/* one input step: encoder counts since the last call, button levels (1 = pressed) */
void hcui_input(HcUi *u, const Chain *c, uint32_t now_ms, int da, int db, int a_down, int b_down,
                HcUiOut *out);
/* draw the current screen; returns 1 if the picture is animated (needs redraws) */
int hcui_render(HcUi *u, VecFb *fb, const Chain *c, uint32_t now_ms);

/* changes whenever the picture would change (state, shown values, trigger lamp,
 * message): redraw only when it differs */
uint32_t hcui_signature(const HcUi *u, const Chain *c, uint32_t now_ms);

/* parameter table (also for tests) */
int hcui_param_count(void);
const char *hcui_param_label(int i);
int hcui_param_cc(int i);
int hcui_param_get(const Chain *c, int i);              /* current value as CC 0..127 */
void hcui_param_text(const Chain *c, int i, char *out); /* value as shown, >= 12 bytes */
int hcui_param_step(const Chain *c, int i, int d, int fine);   /* new CC after d counts */
#endif
