/* chain.h - NanoCore chain: audio-triggered 909 BD -> Sherman -> Mackie. */
#ifndef CHAIN_H
#define CHAIN_H
#include <stdint.h>
#include "bd909.h"
#include "sherman.h"
#include "mackie.h"

typedef struct {
    float fs;
    BD909 bd; Sherman sh; Mackie mk;
    /* trigger detector */
    float k_sens, k_hold, k_thru, k_out, k_line;
    int   autolvl; float autolvl_g;
    /* gate (Sherman ADSR): high from a detected hit while the input stays above the
     * trigger level, at least k_gmin long; or while a MIDI note is held */
    int   gate, gate_hold, notes;
    float k_gmin, thr_p;
    float k_acc_amt; int acc_fixed;
    float k_pan, pan_l, pan_r; int mute; float mute_g;
    float hp_x1, hp_y1, a_hp;
    float fast, slow, a_fast, a_slow;
    int   window, holdoff, armed;
    float peak;
    int   dirty;
    uint32_t trig_count;
} Chain;

void chain_init(Chain *c, float fs);
void chain_midi(Chain *c, uint8_t status, uint8_t d1, uint8_t d2);  /* CC + note-on */
void chain_process(Chain *c, const float *in, float *outL, float *outR, uint32_t n);

/* NanoCore delegated-effect ABI (re-nanocore docs/custom-fx-patch-site.txt) */
void nc_entry(void *state, const float *in_l, const float *in_r, float *out_l, float *out_r,
              uint32_t frames, const float *params, uint32_t input_mode, uint8_t *output_mode);
#endif
