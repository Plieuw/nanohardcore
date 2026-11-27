/* chain.c - audio/MIDI triggered TR-909 BD -> Sherman filterbank -> Mackie 1202 gain stage.
 *
 * Trigger: the pedal input (guitar jack) is watched by a transient detector that fires
 * on a clock/trigger pulse or on a drum hit. Peak level in a 1.5 ms window after the
 * onset sets the 909 ACCENT (pulse voltage). Retrigger hold-off prevents double hits
 * from ringing drum samples. MIDI note-on also triggers (velocity = accent).
 * The dry input can optionally be mixed into the Sherman ("thru").
 *
 * MIDI CC map (channel ignored; CC 120-127 are channel-mode messages and are not used):
 *   909    : 20 TUNE 21 DECAY 22 ATTACK 23 LEVEL 14 ACCENT 15 accent source (0-63 hit, 64+ fixed)
 *            16 pitch trim +-12 st (64 stock) 17 sweep depth (64 stock)     note-on = trigger
 *   Sherman: 24 INPUT 25 FREQ1 26 RESO1 27 L-B-H 1 28 CORR 1
 *            29 FREQ2 30 RESO2 31 L-B-H 2 85 CORR 2 86 HARMONICS (0 free, 1..11)
 *            87 PAR<>SER 88 ADSR->f1 89 ADSR->f2 (bipolar, 64 = 0) 90 clock bleed
 *            102 A 103 D 104 S 105 R 5 ADSR speed (0 = 4x) 106 clock ratio 50/100
 *            80 HI cut/flat/boost 81 LIMIT 82 minimum gate
 *   Mackie : 107 TRIM 108 LOW CUT 109 LOW 110 MID 111 HI 112 GAIN 18 PAN 19 MUTE 113 MAIN MIX
 *   Input  : 3 mode (0-63 TRIGGER: input fires the 909, 64+ AUDIO: 909 bypassed, input
 *            goes through Sherman + Mackie)
 *   Chain  : 114 trigger sensitivity 115 hold-off 116 thru 117 out level
 *            118 Sherman -> Mackie line level 119 auto level
 */
#include "chain.h"
#include "mathx.h"

#define VOLT_PER_FS 1.0f      /* digital in: 1.0 FS = 1 V (pedal input stage)        */
/* Sherman main out -> Mackie line in: line-level interface. The default (k_line 0.5)
 * puts a full accent kick at ~0.45 V peak (-10 dBV nominal), so the Mackie is clean at
 * TRIM 0 dB and starts to clip around +30 dB TRIM, like a real channel. */
#define LINE_MAX 0.16f
#define OUT_SCALE 1.1f        /* nominal kick (0.45 V at the main out, TRIM 0 dB) -> -6 dBFS */
#define CLIP_ONSET_DB 14.f    /* TRIM where the first stage (U40A, ~11.5 dB hot at GAIN detent) clips at nominal level */
#define WIN_MS 1.5f
#define AUDIO_IN_GAIN 2.0f      /* AUDIO mode: guitar/line level -> ~kick level into Sherman */

void chain_init(Chain *c, float fs)
{
    char *p = (char *)c;
    for (unsigned i = 0; i < sizeof(*c); i++) p[i] = 0;
    c->fs = fs;
    bd909_init(&c->bd, fs);
    sherman_init(&c->sh, fs);
    mackie_init(&c->mk, fs);
    c->sh.k_in = 0.333f;
    sherman_update(&c->sh);
    c->k_sens = 0.5f; c->k_hold = 0.3f; c->k_thru = 0.f; c->k_out = 0.8f; c->k_line = 0.5f;
    c->autolvl = 1; c->autolvl_g = 1.f; c->dirty = 1;
    c->k_gmin = 0.02f;
    c->k_acc_amt = 1.f; c->acc_fixed = 0; c->k_pan = 0.f; c->mute = 0; c->mute_g = 1.f;
    c->mode = 0; c->grace_left = (int)(CHAIN_GRACE_S * fs); c->wet_g = 0.f;
    c->pan_l = c->pan_r = 1.f; c->thr_p = mx_exp2((-20.f - 40.f * c->k_sens) * 0.33219281f);
    c->a_hp = mx_rc_coef(1.f / (2.f * MX_PI * 30.f), 1.f / fs);
    c->a_fast = mx_rc_coef(0.001f, 1.f / fs);
    c->a_slow = mx_rc_coef(0.030f, 1.f / fs);
}

static float cc01(uint8_t v) { return (float)v * (1.f / 127.f); }
static float ccpm(uint8_t v) { return ((float)v - 64.f) * (1.f / 63.f); }

void chain_midi(Chain *c, uint8_t st, uint8_t d1, uint8_t d2)
{
    uint8_t type = st & 0xF0;
    if (type == 0x90 && d2 > 0) {                  /* note-on: trigger + gate */
        bd909_trigger(&c->bd, (c->acc_fixed ? 1.f : cc01(d2)) * c->k_acc_amt);
        sherman_gate(&c->sh, 1);
        c->gate = 1; c->gate_hold = (int)(c->k_gmin * c->fs); c->notes++;
        c->trig_count++;
        return;
    }
    if (type == 0x80 || (type == 0x90 && d2 == 0)) { if (c->notes > 0) c->notes--; return; }
    if (type != 0xB0) return;
    if (d1 >= 120) return;                         /* channel-mode messages: not parameters */
    float v = cc01(d2);
    BD909 *b = &c->bd; Sherman *s = &c->sh; Mackie *m = &c->mk;
    switch (d1) {
    case 20: b->k_tune = v; break;     case 21: b->k_decay = v; break;
    case 22: b->k_attack = v; break;   case 23: b->k_level = v; break;
    case 24: s->k_in = v; break;       case 25: s->k_f1 = v; break;
    case 26: s->k_r1 = v; break;       case 27: s->f[0].morph = v; break;
    case 28: s->f[0].corr = ccpm(d2); break;
    case 29: s->k_f2 = v; break;       case 30: s->k_r2 = v; break;
    case 31: s->f[1].morph = v; break; case 85: s->f[1].corr = ccpm(d2); break;
    case 86: s->harm = (int)(v * 11.f + 0.5f); break;
    case 87: s->k_ser = v; break;      case 88: s->k_env1 = ccpm(d2); break;     /* ccpm(64) == 0 exactly */
    case 89: s->k_env2 = ccpm(d2); break; case 90: s->k_bleed = v; break;
    case 5:   s->k_speed = 4.f - 3.f * v; break;           /* like the unit's CC5: 0 = 4x faster */
    case 102: s->k_a = v; break;       case 103: s->k_d = v; break;
    case 104: s->k_s = v; break;       case 105: s->k_r = v; break;
    case 106: s->ratio = d2 < 64 ? 50.f : 100.f; s->fmax = 12000.f; break;
    case 107: m->k_trim = v; break;    case 108: m->lowcut = d2 >= 64; break;
    case 109: m->k_lo = ccpm(d2); break; case 110: m->k_mid = ccpm(d2); break;
    case 111: m->k_hi = ccpm(d2); break; case 112: m->k_gain = v; break;
    case 113: m->k_main = v; break;
    case 114: c->k_sens = v; break;    case 115: c->k_hold = v; break;
    case 116: c->k_thru = v; break;    case 117: c->k_out = v; break;
    case 118: c->k_line = v; break;
    case 119: c->autolvl = d2 >= 64; break;
    case 80: s->hi_sw = d2 < 43 ? 0 : (d2 < 86 ? 1 : 2); break;
    case 81: s->limit = d2 >= 64; break;
    case 82: c->k_gmin = 0.005f + 0.5f * v; break;           /* minimum gate 5..505 ms */
    /* 909: panel ACCENT knob + accent source, and two classic service mods */
    case 14: c->k_acc_amt = v; break;                        /* ACCENT level */
    case 15: c->acc_fixed = d2 >= 64; break;                 /* 0-63 hit strength, 64+ fixed */
    case 16: b->k_pitch = ccpm(d2); break;                   /* base pitch trim +-12 st (64 = stock) */
    case 17: b->k_depth = ccpm(d2); break;                   /* sweep depth 0..2x (64 = stock) */
    /* Mackie channel strip: PAN (stereo out) and MUTE */
    case 3:  c->mode = d2 >= 64; break;                      /* input: TRIGGER / AUDIO */
    case 18: c->k_pan = ccpm(d2); break;
    case 19: c->mute = d2 >= 64; break;
    default: return;
    }
    c->dirty = 1;
}

/* transient detector (power-ratio onset): returns accent 0..1 when a hit fires, else -1.
 * fast = 1 ms power average, slow = 30 ms power average. A steady tone keeps
 * fast/slow below ~2; a new hit pushes it far above RATIO, even over a ringing tail. */
#define ONSET_RATIO 3.0f
static float detect(Chain *c, float x)
{
    float h = c->a_hp * (c->hp_y1 + x - c->hp_x1);    /* 30 Hz DC/rumble block */
    c->hp_x1 = x; c->hp_y1 = h;
    float r = mx_abs(h), p = h * h;
    float slow_prev = c->slow;
    c->fast = p + c->a_fast * (c->fast - p);
    c->slow = p + c->a_slow * (c->slow - p);
    if (c->holdoff > 0) c->holdoff--;

    float thr_db = -20.f - 40.f * c->k_sens;          /* -20..-60 dBFS */
    if (c->window > 0) {                                /* measuring accent */
        if (r > c->peak) c->peak = r;
        if (--c->window == 0) {
            float db = 6.0206f * mx_log2(c->peak + 1e-9f) - thr_db;
            c->holdoff = (int)((0.015f + 0.25f * c->k_hold) * c->fs);
            return mx_clamp(db * (1.f / 30.f), 0.f, 1.f);
        }
        return -1.f;
    }
    float thr_p = mx_exp2(thr_db * 0.33219281f);       /* threshold as power (rms^2) */
    c->thr_p = thr_p;
    if (c->holdoff == 0 && c->fast > thr_p && c->fast > ONSET_RATIO * slow_prev) {
        c->window = (int)(WIN_MS * 1e-3f * c->fs);
        c->peak = r;
    }
    return -1.f;
}

void chain_process(Chain *c, const float *in, float *outL, float *outR, uint32_t n)
{
    if (c->dirty) {
        /* PAN R486 (dual 10k): constant-power law, normalised so centre = unity per side */
        float th = (mx_clamp(c->k_pan, -1.f, 1.f) + 1.f) * 0.25f * MX_PI;
        c->pan_l = 1.41421356f * mx_cos(th); c->pan_r = 1.41421356f * mx_sin(th);
        /* auto level: undo the TRIM gain up to clip onset (what you'd do with MAIN on a
         * real desk), so past that point TRIM adds overdrive, not loudness */
        float tdb = mx_clamp(-15.f + 60.f * c->mk.k_trim, 0.f, CLIP_ONSET_DB);
        c->autolvl_g = c->autolvl ? mx_exp2(-tdb * 0.16609640f) : 1.f;
        bd909_set(&c->bd, c->bd.k_tune, c->bd.k_decay, c->bd.k_attack, c->bd.k_level);
        sherman_update(&c->sh);
        mackie_update(&c->mk);
        c->dirty = 0;
    }
    if (c->grace_left > 0) {                            /* boot grace: dry, no DSP load */
        for (uint32_t i = 0; i < n; i++) {
            float x = in ? in[i] : 0.f;
            outL[i] = x; if (outR) outR[i] = x;
        }
        c->grace_left -= (int)n;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        float x = in ? in[i] : 0.f;
        float acc = detect(c, x);
        if (acc >= 0.f) {                               /* every hit: 909 + gate on */
            if (c->mode == 0) bd909_trigger(&c->bd, (c->acc_fixed ? 1.f : acc) * c->k_acc_amt);
            sherman_gate(&c->sh, 1);
            c->gate = 1; c->gate_hold = (int)(c->k_gmin * c->fs);
            c->trig_count++;
        } else if (c->gate) {                           /* gate falls when the input does */
            if (c->gate_hold > 0) c->gate_hold--;
            else if (c->notes == 0 && c->fast < c->thr_p * 0.25f) { c->gate = 0; sherman_gate(&c->sh, 0); }
        }
        float v = c->mode == 0 ? bd909_tick(&c->bd) + c->k_thru * x * VOLT_PER_FS
                               : x * VOLT_PER_FS * AUDIO_IN_GAIN;     /* AUDIO: 909 bypassed */
        v = sherman_tick(&c->sh, v);
        v = mackie_tick(&c->mk, v * LINE_MAX * 2.f * c->k_line);
        float o = mx_clamp(v * OUT_SCALE * c->k_out * c->autolvl_g, -3.f, 3.f);
        o = o * (27.f + o * o) / (27.f + 9.f * o * o);    /* output soft limiter (Pade tanh) */
        c->mute_g += (c->mute ? -1.f : 1.f) * (1.f / 220.f);          /* 5 ms mute ramp */
        c->mute_g = mx_clamp(c->mute_g, 0.f, 1.f);
        o *= c->mute_g;
        if (c->wet_g < 1.f) {                           /* 20 ms fade-in after the grace */
            c->wet_g += 1.f / (0.02f * c->fs); if (c->wet_g > 1.f) c->wet_g = 1.f;
            o = x + (o - x) * c->wet_g;
        }
        if (outR) { outL[i] = o * c->pan_l; outR[i] = o * c->pan_r; }
        else outL[i] = o;
    }
}

void nc_entry(void *state, const float *in_l, const float *in_r, float *out_l, float *out_r,
              uint32_t frames, const float *params, uint32_t input_mode, uint8_t *output_mode)
{
    (void)in_r; (void)params; (void)input_mode;
    chain_process((Chain *)state, in_l, out_l, out_r, frames);
    if (output_mode) *output_mode = out_r ? 1 : 0;
}

/* ---- stock-firmware parameter bank (12 controls, 0..1) ---- */
const char *const CHAIN_DEV_NAMES[CHAIN_DEV_CONTROLS] = {
    "SENS", "TUNE", "DECAY", "DRIVE", "FREQ1", "RESO1", "L-B-H1", "ENV>F1", "HARM", "TRIM", "MID", "LEVEL" };
const float CHAIN_DEV_DEFAULTS[CHAIN_DEV_CONTROLS] = {
    0.5f, 0.5f, 0.5f, 0.333f, 0.75f, 0.35f, 0.0f, 0.75f, 0.0f, 0.25f, 0.5f, 0.8f };

void chain_device_controls(Chain *c, const float *p)
{
    float q[CHAIN_DEV_CONTROLS];
    for (int i = 0; i < CHAIN_DEV_CONTROLS; i++) q[i] = mx_clamp(p ? p[i] : CHAIN_DEV_DEFAULTS[i], 0.f, 1.f);
    c->k_sens = q[0];
    c->bd.k_tune = q[1]; c->bd.k_decay = q[2];
    c->sh.k_in = q[3]; c->sh.k_f1 = q[4]; c->sh.k_r1 = q[5]; c->sh.f[0].morph = q[6];
    c->sh.k_env1 = 2.f * q[7] - 1.f;                       /* 0.5 = exactly 0 */
    c->sh.harm = (int)(q[8] * 11.f + 0.5f);
    c->mk.k_trim = q[9]; c->mk.k_mid = 2.f * q[10] - 1.f;  /* 0.5 = flat */
    c->k_out = q[11];
    c->dirty = 1;
}
