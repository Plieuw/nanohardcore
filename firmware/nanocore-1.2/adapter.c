/* adapter.c - nanohardcore in het FX2/PITCH-slot (subtype 8) van NanoCore-firmware 1.2.
 *
 * Gebaseerd op re-nanocore firmware/custom-fx/octave/device.cpp en overload_guard.h
 * (MIT, (c) 2026 jcuhnoio and contributors), met de 1.2-adressen uit hun
 * tools/custom_fx/octave_120.py. ABI, haakpunten, de vergrote ring van 64 KB en de
 * CPU-bewaking zijn gelijk aan die van de op hardware geteste 1.2-Octaver; alleen de
 * DSP is vervangen door chain.c (909 -> Sherman -> Mackie).
 *
 * Veiligheid:
 *  - opstart-grace in chain.c: de eerste 3 s na pitch_init is de keten droog;
 *  - de CPU-bewaking latcht bypass tot een herstart bij overschrijding van het budget
 *    (eigen tijd, of overruns van de hele audio-callback), net als bij de Octaver;
 *  - geen heap, geen libc, geen externe aanroepen behalve de native 64-bit timer.
 */
#include <stdint.h>
#include <stddef.h>
#include "../../dsp/chain.h"

/* ---- libc-minimum voor door de compiler gegenereerde aanroepen ---- */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ---- native 1.2-adressen (re-nanocore octave_versions.json, profiel '1.2') ---- */
#define NATIVE_TICKS      0x4004393cu   /* 64-bit timer, 4 MHz */
#define AUDIO_SUM         0x3008a9e0u   /* som van callback-ticks */
#define AUDIO_CALLS       0x3008a9f0u
#define AUDIO_DEADLINE    0x3008a9fcu
#define AUDIO_OVERRUNS    0x3008aa00u
#define FAMILY_ENABLE     8             /* family+8: pointer naar enable-byte */
#define FAMILY_PITCH      16            /* family+16: native pitch-object (eerste woord = onze state) */
#define FAMILY_SUBTYPE    0xe48
#define NATIVE_PITCH_BYTES 0xd98
#define RING_BYTES        0x10000
#define TICK_HZ           4000000.f

#ifndef NC_BUDGET_PERCENT
#define NC_BUDGET_PERCENT 65            /* zachte reservering; harde grens max 60% + 1/8 */
#endif
_Static_assert(NC_BUDGET_PERCENT >= 40 && NC_BUDGET_PERCENT <= 65, "ongeldig budget");

static uint64_t ticks(void) { return ((uint64_t (*)(void))NATIVE_TICKS)(); }
static uint32_t word(uintptr_t a) { return *(volatile uint32_t *)a; }

/* ---- CPU-bewaking (overload_guard.h, naar C) ---- */
typedef struct { uint64_t bits; unsigned count; } Recent;
static unsigned recent_push(Recent *r, int miss)
{
    r->count -= (unsigned)(r->bits >> 63);
    r->bits = (r->bits << 1) | (unsigned)(miss != 0);
    r->count += (unsigned)(miss != 0);
    return r->count;
}
typedef struct {
    Recent chain, self;
    uint32_t calls, overruns, sum, last_chain_ticks;
    uint32_t chain_exc, self_exc, ignored_overruns, gaps;
    int initialized;
} Guard;

static unsigned guard_observe(Guard *g, uint32_t now_calls, uint32_t now_overruns,
                              uint32_t now_sum, uint32_t deadline)
{
    unsigned fault = 0;
    if (g->initialized && (uint32_t)(now_calls - g->calls) == 1) {
        g->last_chain_ticks = now_sum - g->sum;
        int late = now_overruns != g->overruns;
        if (late) g->chain_exc++;
        unsigned recent = recent_push(&g->chain, late);
        if ((late && deadline && g->last_chain_ticks > deadline + deadline / 4) || recent >= 3) fault = 2;
    } else if (!g->initialized || now_calls != g->calls) {
        if (g->initialized) { g->ignored_overruns += now_overruns - g->overruns; g->gaps++; }
        g->chain.bits = 0; g->chain.count = 0; g->self.bits = 0; g->self.count = 0;
        g->last_chain_ticks = 0;
    }
    g->calls = now_calls; g->overruns = now_overruns; g->sum = now_sum; g->initialized = 1;
    return fault;
}
static uint32_t hard_limit(uint32_t soft) { return soft + soft / 8; }
static unsigned guard_finish(Guard *g, uint32_t elapsed, uint32_t soft, uint32_t hard)
{
    int late = elapsed > soft;
    if (late) g->self_exc++;
    unsigned recent = recent_push(&g->self, late);
    return elapsed > (hard ? hard : hard_limit(soft)) || recent >= 3 ? 1 : 0;
}

/* ---- toestand die de UI (ui.c) en een eventuele diagnose kunnen lezen ---- */
volatile uint32_t nc_octave_fault = 0;          /* 1 = CPU LIMIT, 2 = CHAIN LIMIT; tot herstart */
volatile uint32_t nc_hc_stats[6];               /* calls, frames, max ticks, last ticks, inits, resets */
static Guard guard;

/* ---- MIDI (USB + BLE) via de haak in midi_gate.S ----
 * De MIDI-taak schrijft in een ringbuffer (één schrijver), de audiotaak leest
 * (één lezer); zo verandert de DSP-toestand alleen tussen twee blokken. */
#define FX2_SUBTYPE_BYTE 0x400b0ec1u     /* runtime subtype-array 0x400b0ec0, slot 1 = FX2 */
#define MIDI_Q 64
typedef struct { uint8_t st, d1, d2, pad; } MidiEv;
static MidiEv midi_q[MIDI_Q];
volatile uint32_t nc_hc_midi_head, nc_hc_midi_tail;
#define midi_head nc_hc_midi_head
#define midi_tail nc_hc_midi_tail
volatile uint32_t nc_hc_midi_count, nc_hc_midi_drop;

#define MAGIC 0x48435231u                       /* "HCR1" */
typedef struct {
    uint32_t magic;
    float    rate;
    float    controls[CHAIN_DEV_CONTROLS];
    uint32_t valid;
    Chain    chain;
} State;
_Static_assert(sizeof(State) <= RING_BYTES, "vergrote ring te klein");

static State *volatile g_state;
int nc_hc_fase_a_own(void);

/* 1 = bericht overgenomen (CC, noot aan/uit terwijl HARDCORE het FX2-effect is),
 * 0 = laat de originele firmware het afhandelen (programmawissel, SysEx, klok, ...) */
int nc_midi_in(uint32_t port, uint32_t status, uint32_t d1, uint32_t d2, uint32_t has_d2)
{
    (void)port;
    uint32_t t = status & 0xf0;
    if (t != 0xb0 && t != 0x90 && t != 0x80) return 0;
    State *s = g_state;
    int fx2 = s && s->magic == MAGIC && *(volatile uint8_t *)FX2_SUBTYPE_BYTE == 8;
    if (!fx2 && !nc_hc_fase_a_own()) return 0;
    uint32_t h = midi_head;
    if (h - midi_tail >= MIDI_Q) { nc_hc_midi_drop++; return 1; }
    MidiEv e = { (uint8_t)status, (uint8_t)(d1 & 0x7f), (uint8_t)(has_d2 ? d2 & 0x7f : 0), 0 };
    midi_q[h % MIDI_Q] = e;
    __asm__ volatile("" ::: "memory");
    midi_head = h + 1;
    nc_hc_midi_count++;
    return 1;
}

static void midi_drain(State *s)
{
    uint32_t t = midi_tail, h = midi_head;
    __asm__ volatile("" ::: "memory");
    for (unsigned n = 0; t != h && n < MIDI_Q; t++, n++) {
        MidiEv e = midi_q[t % MIDI_Q];
        chain_midi(&s->chain, e.st, e.d1, e.d2);
    }
    midi_tail = t;
}

static State *state_for(void *native_pitch)
{
    if (!native_pitch) return 0;
    State *s = *(State **)native_pitch;
    return s && s->magic == MAGIC ? s : 0;
}

static float bounded(float v, unsigned i)
{
    if (!(v == v) || v > 3.0e38f || v < -3.0e38f) return CHAIN_DEV_DEFAULTS[i];
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

static void controls(State *s, const float *p)
{
    float next[CHAIN_DEV_CONTROLS]; int changed = !s->valid;
    for (unsigned i = 0; i < CHAIN_DEV_CONTROLS; i++) {
        next[i] = p ? bounded(p[i], i) : CHAIN_DEV_DEFAULTS[i];
        changed |= next[i] != s->controls[i];
    }
    if (!changed) return;
    /* alleen gewijzigde knoppen toepassen: waarden die via MIDI zijn gezet blijven staan
     * tot de bijbehorende pedalknop zelf beweegt */
    for (unsigned i = 0; i < CHAIN_DEV_CONTROLS; i++)
        if (!s->valid || next[i] != s->controls[i]) chain_device_set(&s->chain, (int)i, next[i]);
    for (unsigned i = 0; i < CHAIN_DEV_CONTROLS; i++) s->controls[i] = next[i];
    s->valid = 1;
}

/* DSP opnieuw beginnen (effect gekozen, preset gewisseld): knoppen behouden, een
 * lopende opstart-grace behouden, anders een korte fade-in */
static void restart(State *s)
{
    int grace = s->chain.grace_left;
    chain_init(&s->chain, s->rate);
    s->chain.grace_left = grace > 0 ? grace : 0; s->chain.wet_g = 0.f;
    s->valid = 0; controls(s, s->controls);
}

static void bypass(const float *l, const float *r, float *ol, float *or_,
                   uint32_t frames, uint32_t stereo)
{
    for (uint32_t i = 0; i < frames; i++) {
        float a = l[i], b = r ? r[i] : a;
        ol[i] = a; if (stereo && or_) or_[i] = b;
    }
}
/* bypass van ons eigen effect (CPU-limiet): in de triggerstanden is de input een
 * trigger, geen audio -> stilte; in de AUDIO-standen droog */
static void bypass_state(const State *s, const float *l, const float *r, float *ol, float *or_,
                         uint32_t frames, uint32_t stereo)
{
    if (s->chain.mode != 0) { bypass(l, r, ol, or_, frames, stereo); return; }
    for (uint32_t i = 0; i < frames; i++) { ol[i] = 0.f; if (stereo && or_) or_[i] = 0.f; }
}

uint32_t nc_hc_state_bytes(void) { return sizeof(State); }

int nc_octave_pitch_init(float rate, void *native_pitch, void *ring, uint32_t count)
{
    /* Zelfde contract als de Octaver: beide allocaties zijn door de haakplannen
     * vergroot tot RING_BYTES; de native aanroep vraagt 2048. */
    if (!native_pitch || !ring || count != 2048 || ((uintptr_t)ring & 3)) return -1;
    if (rate != 44100.f && rate != 48000.f) return -1;
    memset(native_pitch, 0, NATIVE_PITCH_BYTES);
    State *s = (State *)ring;
    memset(s, 0, sizeof(State));
    s->rate = rate;
    chain_init(&s->chain, rate);                 /* zet de opstart-grace van 3 s */
    for (unsigned i = 0; i < CHAIN_DEV_CONTROLS; i++) s->controls[i] = CHAIN_DEV_DEFAULTS[i];
    controls(s, 0);
    s->magic = MAGIC;                            /* pas geldig na volledige init */
    g_state = s;
    nc_hc_stats[4]++;
    *(State **)native_pitch = s;
    return 0;
}

void nc_octave_pitch_reset(void *native_pitch)
{
    State *s = state_for(native_pitch);
    if (s) { restart(s); nc_hc_stats[5]++; }
}

void nc_octave_pitch_set_shift(float a, float b, float c, void *p)
{
    (void)a; (void)b; (void)c; (void)p;          /* native pitch-instelling niet gebruikt */
}

void nc_octave_process(void *family, const float *left, const float *right,
                       float *out_left, float *out_right, uint32_t frames,
                       const float *p, uint32_t subtype, uint32_t stereo, uint8_t *output_mode)
{
    if (!left || !out_left || !frames) return;
    if (output_mode) *output_mode = stereo != 0;
    unsigned char *bytes = family;
    uint8_t *enabled = family ? *(uint8_t **)(bytes + FAMILY_ENABLE) : 0;
    State *s = family ? state_for(bytes + FAMILY_PITCH) : 0;
    if (subtype != 8 || !enabled || !*enabled || !s) { bypass(left, right, out_left, out_right, frames, stereo); return; }

    uint32_t overruns = word(AUDIO_OVERRUNS), calls = word(AUDIO_CALLS);
    if (!nc_octave_fault && guard_observe(&guard, calls, overruns, word(AUDIO_SUM), word(AUDIO_DEADLINE)))
        nc_octave_fault = 2;
    if (nc_octave_fault) { bypass_state(s, left, right, out_left, out_right, frames, stereo); return; }

    uint64_t start = ticks();
    float block_ticks = TICK_HZ * (float)frames / s->rate;
    uint32_t soft = (uint32_t)(block_ticks * (NC_BUDGET_PERCENT * .01f));
    uint32_t hard = hard_limit((uint32_t)(block_ticks * ((NC_BUDGET_PERCENT > 60 ? 60 : NC_BUDGET_PERCENT) * .01f)));

    if (bytes[FAMILY_SUBTYPE] != 8) { restart(s); nc_hc_stats[5]++; }   /* net gekozen */
    bytes[FAMILY_SUBTYPE] = 8;
    controls(s, p);
    midi_drain(s);

    float mono[32], ol[32], or_[32];
    for (uint32_t off = 0; off < frames; off += 32) {
        uint32_t n = frames - off < 32 ? frames - off : 32;
        for (uint32_t i = 0; i < n; i++) {
            float l = left[off + i];
            mono[i] = stereo && right ? .5f * (l + right[off + i]) : l;
        }
        chain_process(&s->chain, mono, ol, or_, n);
        if (ticks() - start > hard) {            /* harde grens: rest droog, latch */
            nc_octave_fault = 1;
            bypass_state(s, left + off, right ? right + off : 0, out_left + off,
                         out_right ? out_right + off : 0, frames - off, stereo);
            break;
        }
        for (uint32_t i = 0; i < n; i++) {
            out_left[off + i] = ol[i];
            if (stereo && out_right) out_right[off + i] = or_[i];
        }
    }
    uint64_t el = ticks() - start;
    uint32_t last = el > 0xffffffffull ? 0xffffffffu : (uint32_t)el;
    nc_hc_stats[0]++; nc_hc_stats[1] += frames; nc_hc_stats[3] = last;
    if (last > nc_hc_stats[2]) nc_hc_stats[2] = last;
    if (guard_finish(&guard, last, soft, hard)) nc_octave_fault = 1;
}

/* Haak op de native FX-dispatch. Geen stackgebruik vóór de keuze: argument 9/10
 * staan op sp+0/+4. Niet-PITCH speelt de overschreven native proloog exact na
 * (1.2: __riscv_save_12 met hervatting op 0x40069824). */
__attribute__((naked)) void nc_octave_dispatch(void)
{
    __asm__ volatile(
        "li t0,8\n"
        "bne a7,t0,1f\n"
        "j nc_octave_process\n"
        "1: li t0,0x40069824\n"
        "li t1,0x4000c3a4\n"
        "jr t1\n");
}


/* ==== Fase A: de hele native effectketen vervangen (docs/eigen-firmware.md) ====
 *
 * De brokverwerker 0x30043800 (ITCM) zet per 64 frames de mono-ingang om naar float
 * (L = R, 0x301034b8/0x301035b8), laat de versterker/NAM-trap 0x400733f4 erop los en
 * roept dan de ketendispatcher 0x30041d7e aan. Die loopt de 10 slots van het preset af
 * via de procestabel 0x4008ea9c (effecten, cab/IR, tuner-voeding enz.). Na de dispatcher
 * volgen de native uitgangstrap, volume en de int32-conversie naar DMA: die blijven.
 *
 * Twee haken (build.py):
 *   0x3004395e  auipc a0 / addi a0 / jal 0x30041d7e  ->  auipc ra / jalr nc_hc_dispatch / nop
 *   0x3004392a  auipc ra / jalr 0x400733f4           ->  auipc ra / jalr nc_hc_amp
 * Tijdens de opstart-grace (3 s audio) en na een CPU-fout roepen beide exact de
 * native functie aan: het apparaat gedraagt zich dan als stock 1.2. */
#define NATIVE_DISPATCH    0x30041d7eu
#define NATIVE_CHAIN_STATE 0x301038b8u
#define NATIVE_AMP         0x400733f4u
#define NATIVE_RATE_WORD   0x30104128u   /* 0 = 48000 (zoals de native code) */
#define FASE_A_BLOCK       64u
#define FASE_A_GRACE_BLOCKS ((uint32_t)(CHAIN_GRACE_S * 48000.f) / FASE_A_BLOCK)

typedef struct { float *l, *r; uint8_t stereo; } DispatchOut;
typedef void (*NativeDispatch)(void *, float *, float *, float *, float *, uint32_t, void *, DispatchOut *);
typedef void (*NativeAmp)(void *, float *, float *, void *, uint32_t, void *);

#ifndef FASE_A_SOFT_PERCENT
#define FASE_A_SOFT_PERCENT 80           /* blok "zwaar" (telt mee in het venster) */
#endif
#ifndef FASE_A_HARD_PERCENT
#define FASE_A_HARD_PERCENT 95           /* één blok hierboven: echt bijna te laat */
#endif
#define FASE_A_RECENT_MAX 8              /* zware blokken in de laatste 64 */
/* belasting in promille: gemiddelde, piek, bij de fout, native overschrijdingen */
volatile uint32_t nc_hc_load[4];
static unsigned guard_finish_a(Guard *g, uint32_t elapsed, uint32_t soft, uint32_t hard)
{
    int heavy = elapsed > soft;
    if (heavy) g->self_exc++;
    unsigned recent = recent_push(&g->self, heavy);
    return elapsed > hard || recent >= FASE_A_RECENT_MAX ? 1 : 0;
}

static State fase_a;                 /* .bss: zit als nullen in de payload */
static Guard guard_a;

/* ---- fase B: de eigen UI (screen.c) stuurt de keten met CC's via een eigen ring
 * (één schrijver: de LVGL-taak; één lezer: de audiotaak) en zet bypass ---- */
static MidiEv ui_q[MIDI_Q];
volatile uint32_t nc_hc_ui_head, nc_hc_ui_tail, nc_hc_ui_drop;
volatile uint32_t nc_hc_bypass;           /* 1 = hele keten droog (5 ms ramp) */
static float bypass_g;                    /* 0 = keten, 1 = droog */

void nc_hc_ui_cc(uint8_t cc, uint8_t value)
{
    uint32_t h = nc_hc_ui_head;
    if (h - nc_hc_ui_tail >= MIDI_Q) { nc_hc_ui_drop++; return; }
    MidiEv e = { 0xB0, (uint8_t)(cc & 0x7f), (uint8_t)(value & 0x7f), 0 };
    ui_q[h % MIDI_Q] = e;
    __asm__ volatile("" ::: "memory");
    nc_hc_ui_head = h + 1;
}

static void ui_drain(State *s)
{
    uint32_t t = nc_hc_ui_tail, h = nc_hc_ui_head;
    __asm__ volatile("" ::: "memory");
    for (unsigned n = 0; t != h && n < MIDI_Q; t++, n++) {
        MidiEv e = ui_q[t % MIDI_Q];
        chain_midi(&s->chain, e.st, e.d1, e.d2);
    }
    nc_hc_ui_tail = t;
}

/* ---- hoorbare diagnose (beta): ~7 s na de start van de eigen keten piept de pedal
 * zo vaak als de reden waarom het scherm niet is overgenomen (1 = geen aicfb-handle,
 * 3 = onbekend pixelformaat, 4 = vreemde resolutie, 5 = framebuffer/stride,
 * 6 = de UI-haak is nooit aangeroepen). Niets horen = scherm is van HARDCORE. ---- */
#ifndef NC_DIAG_BEEPS
#define NC_DIAG_BEEPS 1
#endif
extern volatile uint32_t nc_hc_screen[13];
volatile uint32_t nc_hc_diag[3];          /* code, samples sinds start, gespeeld */

static void diag_beeps(State *s, float *l, float *r, uint32_t n)
{
#if NC_DIAG_BEEPS
    uint32_t t0 = nc_hc_diag[1], rate = (uint32_t)s->rate, at = 7u * rate;
    nc_hc_diag[1] = t0 + n;
    if (t0 + n <= at) return;
    if (!nc_hc_diag[2]) {                                 /* één keer beslissen, na 7 s */
        nc_hc_diag[2] = 1;
        nc_hc_diag[0] = nc_hc_screen[12] == 0 ? 6u : nc_hc_screen[0] == 2u ? nc_hc_screen[1] : 0u;
    }
    uint32_t code = nc_hc_diag[0];
    if (!code) return;
    uint32_t per = rate * 3u / 10u, len = rate * 8u / 100u, fade = rate / 500u;  /* 80 ms per 0,3 s */
    for (uint32_t i = 0; i < n; i++) {
        if (t0 + i < at) continue;
        uint32_t k = t0 + i - at, idx = k / per, ph = k % per;
        if (idx >= code || ph >= len) continue;
        float frac = (float)((ph * 1000u) % rate) / (float)rate;      /* 1 kHz */
        float tri = 4.f * (frac < .5f ? .5f - frac : frac - .5f) - 1.f;
        float env = ph < fade ? (float)ph / (float)fade : ph > len - fade ? (float)(len - ph) / (float)fade : 1.f;
        l[i] += .2f * tri * env; r[i] += .2f * tri * env;
    }
#else
    (void)s; (void)l; (void)r; (void)n;
#endif
}

const Chain *nc_hc_chain(void)
{
    return fase_a.magic == MAGIC ? &fase_a.chain : 0;
}
/* blokken, native blokken, eigen blokken, laatste reden van terugval
 * (0 = geen, 1 = grace, 2 = CPU-fout, 3 = ongeldige argumenten), aantal overgeslagen versterkers */
volatile uint32_t nc_hc_fase_a[5];

int nc_hc_fase_a_own(void)
{
    return nc_hc_fase_a[0] >= FASE_A_GRACE_BLOCKS && !nc_octave_fault;
}

static void fase_a_start(void)
{
    uint32_t rate = word(NATIVE_RATE_WORD);
    State *s = &fase_a;
    memset(s, 0, sizeof(State));
    s->rate = rate == 44100u ? 44100.f : 48000.f;
    chain_init(&s->chain, s->rate);
    chain_device_controls(&s->chain, CHAIN_DEV_DEFAULTS);   /* zet ook het CPU-plafond */
    for (unsigned i = 0; i < CHAIN_DEV_CONTROLS; i++) s->controls[i] = CHAIN_DEV_DEFAULTS[i];
    s->valid = 1;
    s->chain.grace_left = 0; s->chain.wet_g = 0.f;         /* native grace is al voorbij: 20 ms fade-in */
    s->magic = MAGIC;
}

void nc_hc_amp(void *amp, float *l, float *r, void *params, uint32_t type, void *flag)
{
    if (nc_hc_fase_a_own()) { nc_hc_fase_a[4]++; return; }   /* versterker/NAM draait niet */
    ((NativeAmp)NATIVE_AMP)(amp, l, r, params, type, flag);
}

void nc_hc_dispatch(void *unused, float *in_l, float *in_r, float *out_l, float *out_r,
                    uint32_t frames, void *ctx, DispatchOut *res)
{
    (void)unused;
    uint32_t k = nc_hc_fase_a[0]++;
    if (k < FASE_A_GRACE_BLOCKS || nc_octave_fault || !in_l || !out_l || !out_r || !res
        || frames == 0 || frames > FASE_A_BLOCK) {
        nc_hc_fase_a[3] = k < FASE_A_GRACE_BLOCKS ? 1 : nc_octave_fault ? 2 : 3;
        nc_hc_fase_a[1]++;
        ((NativeDispatch)NATIVE_DISPATCH)((void *)NATIVE_CHAIN_STATE, in_l, in_r, out_l, out_r, frames, ctx, res);
        return;
    }
    State *s = &fase_a;
    if (s->magic != MAGIC) fase_a_start();
    nc_hc_fase_a[2]++; nc_hc_fase_a[3] = 0;

    uint32_t overruns = word(AUDIO_OVERRUNS), calls = word(AUDIO_CALLS);
    if (guard_observe(&guard_a, calls, overruns, word(AUDIO_SUM), word(AUDIO_DEADLINE)) && !nc_octave_fault) {
        nc_octave_fault = 2; nc_hc_load[2] = nc_hc_load[0];
    }
    nc_hc_load[3] = guard_a.chain_exc;

    uint64_t start = ticks();
    float block_ticks = TICK_HZ * (float)frames / s->rate;
    /* fase A vervangt de hele native keten: het blok is van ons. De echte grens is de
     * native deadline (guard_observe hierboven, fout 2). Onze eigen meting telt ook
     * onderbrekingen (scherm, BLE) mee en is daarom alleen een vangnet. */
    uint32_t soft = (uint32_t)(block_ticks * (FASE_A_SOFT_PERCENT * .01f));
    uint32_t hard = (uint32_t)(block_ticks * (FASE_A_HARD_PERCENT * .01f));
    midi_drain(s);
    ui_drain(s);

    for (uint32_t off = 0; off < frames; off += 32) {
        uint32_t n = frames - off < 32 ? frames - off : 32;
        if (nc_octave_fault) {                   /* rest van dit blok: zelfde regel als de FX2-route */
            bypass_state(s, in_l + off, 0, out_l + off, out_r + off, frames - off, 1);
            break;
        }
        chain_process(&s->chain, in_l + off, out_l + off, out_r + off, n);
        if (ticks() - start > hard) nc_octave_fault = 1;
    }
    /* bypass (A+B op de pedal): droog = de ingang, met een ramp van 5 ms */
    float target = nc_hc_bypass ? 1.f : 0.f;
    if (target != bypass_g || target != 0.f) {
        float step = 1.f / (0.005f * s->rate);
        for (uint32_t i = 0; i < frames; i++) {
            bypass_g += bypass_g < target ? step : bypass_g > target ? -step : 0.f;
            if (bypass_g < 0.f) bypass_g = 0.f;
            if (bypass_g > 1.f) bypass_g = 1.f;
            float d = in_l[i];
            out_l[i] += (d - out_l[i]) * bypass_g;
            out_r[i] += (d - out_r[i]) * bypass_g;
        }
    }
    diag_beeps(s, out_l, out_r, frames);
    res->l = out_l; res->r = out_r; res->stereo = 1;

    uint64_t el = ticks() - start;
    uint32_t last = el > 0xffffffffull ? 0xffffffffu : (uint32_t)el;
    nc_hc_stats[0]++; nc_hc_stats[1] += frames; nc_hc_stats[3] = last;
    if (last > nc_hc_stats[2]) nc_hc_stats[2] = last;
    /* belasting in promille van het blok: gemiddelde (EMA ~50 ms) en piek */
    uint32_t pm = (uint32_t)((float)last * 1000.f / block_ticks);
    nc_hc_load[0] = (nc_hc_load[0] * 31u + pm) / 32u;
    if (nc_hc_stats[0] > 750u && pm > nc_hc_load[1]) nc_hc_load[1] = pm;     /* na 1 s */
    if (guard_finish_a(&guard_a, last, soft, hard) && !nc_octave_fault) nc_octave_fault = 1;
    if (nc_octave_fault && !nc_hc_load[2]) nc_hc_load[2] = pm ? pm : 1u;   /* belasting op het moment van de fout */
}
