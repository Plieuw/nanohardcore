/* screen.c - fase B: eigen scherm en knoppen op NanoCore-firmware 1.2.
 *
 * Wat de firmware doet (docs/eigen-firmware.md, fase B):
 *  - LVGL 9 draait in de taak "LVGL": lus van lv_timer_handler (0x4004f4b8) en een
 *    mdelay van 8..16 ms. Het display is aicfb (ST7789), dubbel gebufferd in direct
 *    mode; de flush doet cache schoonmaken, ioctl 0x4643 (pan naar buffer 0/1) en
 *    0x4620 (vsync).
 *  - De taak "enc_poll" scant elke 2 ms de drukknoppen en roept elke 8 ms de
 *    knoppendispatcher 0x40017f72 aan. De encoders tellen in GPIO-interrupts:
 *    enc1 = PB.5/PB.6 (+ knop PB.7, kort en lang), enc2 = PE.9/PE.10 (+ knop PE.11).
 *
 * Twee haken (build.py):
 *  - 0x40017ab2  jal lv_timer_handler -> jal nc_hc_ui_tick
 *  - 0x40018004  jal 0x40017f72       -> jal nc_hc_enc_dispatch
 * De eerste NC_UI_GRACE_MS na de start van de LVGL-taak draait alles native. Daarna
 * (als aicfb een bruikbaar formaat meldt) draait LVGL niet meer: wij tekenen met de
 * vector-engine in de framebuffer en lezen de encoders en knoppen zelf. Meldt aicfb
 * iets onverwachts, dan blijft alles native (en dat wordt in nc_hc_screen gezet).
 */
#include <stdint.h>
#include "../../ui/hcui.h"

#define NATIVE_TICKS      0x4004393cu   /* 64-bit timer, 4 MHz */
#define LV_TIMER_HANDLER  0x4004f4b8u
#define ENC_DISPATCH      0x40017f72u
#define AICFB_HANDLE      0x30118f3cu   /* pointer naar { rt_device *dev; int refs } */
#define AICFB_CTL         0x4005cb68u   /* (handle, cmd, arg) -> rt_device_control */
#define DCACHE_CLEAN      0x40017b48u   /* (adres, lengte) */
#define PIN_READ          0x40045290u   /* rt_pin_read(pin) */
#define ENC1_TAKE         0x40042c7eu   /* int16 delta lezen en wissen (kritieke sectie) */
#define ENC2_TAKE         0x40042ca8u
#define BTN1_SHORT_TAKE   0x40042cd2u
#define BTN2_SHORT_TAKE   0x40042cfcu
#define BTN1_LONG_TAKE    0x40042d26u
#define PIN_BTN1          0x30117f78u   /* pin-id van PB.7 */
#define PIN_BTN2          0x30117f7cu   /* pin-id van PE.11 */
#define FB_GET_SCREENINFO 0x4662
#define FB_PAN_DISPLAY    0x4643
#define FB_WAIT_VSYNC     0x4620
#define MPP_FMT_ARGB8888  0
#define MPP_FMT_XRGB8888  4
#define MPP_FMT_RGB565    14

/* welke encoder A (waar) en B (hoeveel) is, en de draairichting: nog op de pedal te
 * bevestigen (docs/eigen-firmware.md) */
#ifndef NC_UI_A_IS_ENC1
#define NC_UI_A_IS_ENC1 1
#endif
#ifndef NC_UI_DIR_A
#define NC_UI_DIR_A 1
#endif
#ifndef NC_UI_DIR_B
#define NC_UI_DIR_B 1
#endif
#ifndef NC_UI_GRACE_MS
#define NC_UI_GRACE_MS 3000u
#endif

/* uit adapter.c */
extern volatile uint32_t nc_octave_fault;
extern volatile uint32_t nc_hc_bypass;
const Chain *nc_hc_chain(void);
void nc_hc_ui_cc(uint8_t cc, uint8_t value);

typedef struct {                        /* Luban-Lite aicfb_screeninfo (zoals 0x40017c68 hem leest) */
    uint32_t format, bpp, stride, width, height;
    uint8_t *framebuffer;
    uint32_t smem_len;
    uint32_t spare[9];
} FbInfo;

enum { WAIT = 0, ON = 1, FAULT = 2 };
static struct {
    uint32_t started, t0, state, boot_t0, last_draw, back, deb_a, deb_b, a, b, sig;
    void *handle;
    FbInfo info;
    HcUi ui;
} S;
/* toestand, reden van terugval, frames, formaat, breedte, hoogte, bpp, laatste pan,
 * UI-scherm, gekozen blok, stride, framebuffer, aantal UI-ticks */
volatile uint32_t nc_hc_screen[13];

typedef uint64_t (*TicksFn)(void);
typedef uint32_t (*U32Fn)(void);
typedef int (*TakeFn)(void);
typedef int (*CtlFn)(void *, int, void *);
typedef int (*PinFn)(int);
typedef void (*CleanFn)(void *, int);
typedef void (*VoidFn)(void);

/* ms-achtige teller: eenheden van 1,024 ms (4 MHz >> 12), zonder 64-bit deling */
static uint32_t now_ms(void) { return (uint32_t)(((TicksFn)NATIVE_TICKS)() >> 12); }
static uint32_t native_lvgl(void) { return ((U32Fn)LV_TIMER_HANDLER)(); }

static int ctl(int cmd, void *arg) { return ((CtlFn)AICFB_CTL)(S.handle, cmd, arg); }

static uint32_t fault(uint32_t why)
{
    S.state = FAULT; nc_hc_screen[0] = FAULT; nc_hc_screen[1] = why;
    return native_lvgl();
}

/* lv_color_format per mpp-formaat 4..14, precies de tabel die de native displayinit
 * (0x40017c68) gebruikt; andere formaten worden daar ARGB8888 (0x10) */
#define LV_FMT_TABLE 0x40088f84u
static uint32_t bpp_of(uint32_t mpp)
{
    uint32_t lv = mpp - 4u <= 10u ? *(const volatile uint8_t *)(LV_FMT_TABLE + mpp - 4u) : 0x10u;
    return lv == 0x12u ? 16u : lv == 0x0fu ? 24u : (lv == 0x10u || lv == 0x11u) ? 32u : 0u;
}

/* zo dicht mogelijk bij hoe de native displayinit de scherminfo gebruikt: hij kijkt
 * niet naar de returnwaarde of smem_len en zet buffer 2 op fb + stride * hoogte */
static int takeover(void)
{
    S.handle = *(void *volatile *)AICFB_HANDLE;
    if (!S.handle) return 1;
    uint32_t *w = (uint32_t *)&S.info;
    for (unsigned i = 0; i < sizeof S.info / 4; i++) w[i] = 0;
    (void)ctl(FB_GET_SCREENINFO, &S.info);
    FbInfo *f = &S.info;
    nc_hc_screen[3] = f->format; nc_hc_screen[4] = f->width; nc_hc_screen[5] = f->height; nc_hc_screen[6] = f->bpp;
    nc_hc_screen[10] = f->stride; nc_hc_screen[11] = (uint32_t)(uintptr_t)f->framebuffer;
    uint32_t bpp = bpp_of(f->format);
    if (!bpp) return 3;
    f->bpp = bpp;                         /* het formaat beslist, niet het bpp-veld */
    if (f->width < 64 || f->height < 64 || f->width > 800 || f->height > 800) return 4;
    uint32_t bytes = bpp / 8;
    if (f->stride < f->width * bytes && f->stride >= f->width) f->stride *= bytes;   /* stride in pixels */
    if (!f->framebuffer || f->stride < f->width * bytes || f->stride % bytes) return 5;
    return 0;
}

static void read_input(int *da, int *db, int *a, int *b)
{
    int e1 = ((TakeFn)ENC1_TAKE)(), e2 = ((TakeFn)ENC2_TAKE)();
    (void)((TakeFn)BTN1_SHORT_TAKE)(); (void)((TakeFn)BTN2_SHORT_TAKE)(); (void)((TakeFn)BTN1_LONG_TAKE)();
    int p1 = ((PinFn)PIN_READ)(*(volatile int *)PIN_BTN1) == 0;     /* actief laag */
    int p2 = ((PinFn)PIN_READ)(*(volatile int *)PIN_BTN2) == 0;
    int pa = NC_UI_A_IS_ENC1 ? p1 : p2, pb = NC_UI_A_IS_ENC1 ? p2 : p1;
    /* ontdenderen: twee gelijke lezingen achter elkaar (>= 16 ms) */
    if (pa == (int)S.deb_a) S.a = (uint32_t)pa;
    if (pb == (int)S.deb_b) S.b = (uint32_t)pb;
    S.deb_a = (uint32_t)pa; S.deb_b = (uint32_t)pb;
    *da = (NC_UI_A_IS_ENC1 ? e1 : e2) * NC_UI_DIR_A;
    *db = (NC_UI_A_IS_ENC1 ? e2 : e1) * NC_UI_DIR_B;
    *a = (int)S.a; *b = (int)S.b;
}

static void present(uint32_t now, int boot)
{
    FbInfo *f = &S.info;
    uint32_t bytes = f->bpp / 8, size = f->stride * f->height;   /* buffer 2 = fb + size, zoals native */
    uint8_t *buf = f->framebuffer + S.back * size;
    VecFb fb = { buf, (int)f->width, (int)f->height, (int)(f->stride / bytes), (int)f->bpp };
    if (boot) vec_boot(&fb, now - S.boot_t0);
    else hcui_render(&S.ui, &fb, nc_hc_chain(), now);
    ((CleanFn)DCACHE_CLEAN)(buf, (int)size);
    int idx = (int)S.back;
    ctl(FB_PAN_DISPLAY, &idx);
    ctl(FB_WAIT_VSYNC, 0);
    nc_hc_screen[2]++; nc_hc_screen[7] = S.back;
    S.back ^= 1u; S.last_draw = now;
}

/* vervangt lv_timer_handler in de LVGL-taak; geeft de wachttijd in ms terug */
uint32_t nc_hc_ui_tick(void)
{
    uint32_t now = now_ms();
    nc_hc_screen[12]++;
    if (!S.started) { S.started = 1; S.t0 = now; }
    if (S.state == FAULT) return native_lvgl();
    if (S.state == WAIT) {
        if (now - S.t0 < NC_UI_GRACE_MS) return native_lvgl();
        uint32_t why = (uint32_t)takeover();
        if (why) return fault(why);
        S.state = ON; nc_hc_screen[0] = ON; S.boot_t0 = now; S.back = 1;
        hcui_init(&S.ui);
    }
    int da, db, a, b;
    read_input(&da, &db, &a, &b);
    HcUiOut out;
    const Chain *c = nc_hc_chain();
    hcui_input(&S.ui, c, now, da, db, a, b, &out);
    for (int k = 0; k < out.n; k++) nc_hc_ui_cc(out.cc[k], out.val[k]);
    nc_hc_bypass = S.ui.bypass;
    nc_hc_screen[8] = S.ui.screen; nc_hc_screen[9] = S.ui.block;
    if (nc_octave_fault && !S.ui.msg) { S.ui.msg = nc_octave_fault == 2 ? "CHAIN LIMIT" : "CPU LIMIT"; S.ui.msg_until = 0xffffffffu; }

    uint32_t since = now - S.last_draw;
    if (now - S.boot_t0 < VEC_BOOT_MS) {
        if (since >= 33u) present(now, 1);                /* opstartscherm: ~30 fps */
    } else if (since >= 16u) {
        /* alleen tekenen als het beeld verandert (knoppen, MIDI, trigger, melding) */
        uint32_t sig = hcui_signature(&S.ui, c, now);
        if (S.ui.dirty || sig != S.sig || S.last_draw - S.boot_t0 < VEC_BOOT_MS) { S.sig = sig; present(now, 0); }
    }
    return 16;
}

/* vervangt de native knoppendispatcher in de taak enc_poll */
void nc_hc_enc_dispatch(void)
{
    if (S.state == ON) return;                            /* wij lezen de knoppen zelf */
    ((VoidFn)ENC_DISPATCH)();
}
