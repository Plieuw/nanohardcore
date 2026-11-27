/* chain.h - NanoCore chain: audio-triggered 909 BD -> Sherman -> Mackie. */
#ifndef CHAIN_H
#define CHAIN_H
#include <stdint.h>

#define CHAIN_GRACE_S 3.0f
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
    /* input mode: 0 = TRIGGER (input fires the 909), 1 = AUDIO (909 bypassed, the input
     * itself goes through Sherman + Mackie; hits still gate the Sherman ADSR),
     * 2 = AUDIO + 909 (input and the 909 it triggers both go into the Sherman) */
    int   mode;
    /* boot safety: after init the chain passes the input dry for CHAIN_GRACE_S seconds,
     * then cross-fades in. A preset that has this effect enabled therefore cannot load
     * the CPU before the stock firmware (USB/MIDI, updater entry) is fully up. */
    int   grace_left; float wet_g;
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

/* the 11 controls exposed through the stock firmware's parameter bank (0..1 each).
 * Index 8 (HARM) and 9 (INPUT) are 4-position switches: value*3 rounded. */
#define CHAIN_DEV_CONTROLS 11
#define CHAIN_DEV_CHOICE(v) ((int)((v) * 3.f + .5f))
/* Sherman cutoff ceiling on the pedal (incl. ENV): keeps the worst knob setting at
 * ~57% of the 48 kHz budget (RV32, 360 instr/us), below the 65% guard (make rv) */
#define CHAIN_DEV_FMAX 8000.f
extern const char *const CHAIN_DEV_NAMES[CHAIN_DEV_CONTROLS];
extern const float CHAIN_DEV_DEFAULTS[CHAIN_DEV_CONTROLS];
void chain_device_controls(Chain *c, const float *p);   /* p: 11 values 0..1 */

/* NanoCore delegated-effect ABI (re-nanocore docs/custom-fx-patch-site.txt) */
void nc_entry(void *state, const float *in_l, const float *in_r, float *out_l, float *out_r,
              uint32_t frames, const float *params, uint32_t input_mode, uint8_t *output_mode);
#endif
