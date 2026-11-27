#include "../dsp/chain.h"
void *memset(void *d, int c, unsigned n){ unsigned char*p=d; while(n--) *p++=(unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n){ unsigned char*p=d; const unsigned char*q=s; while(n--) *p++=*q++; return d; }
static Chain C;
static float in[64], outL[64], outR[64];
volatile unsigned mode;            /* set by host: 0 init, 1 block */
volatile unsigned scenario;
volatile float devp[11]; volatile float devfmax;          /* scenario 3: the pedal's 11-control bank, written by the host */
void _start(void) __attribute__((naked, section(".text.start")));
void entry(void);
void _start(void){ __asm__ volatile("li sp, 0x00800000\n call entry\n 1: ebreak\n j 1b"); }
void entry(void){
  if (mode == 0) {
    chain_init(&C, 44100.f); C.grace_left = 0; C.wet_g = 1.f;
    if (scenario == 1) {  /* worst case: both filters at max cutoff (20 clock edges / sample) */
      chain_midi(&C,0xB0,25,127); chain_midi(&C,0xB0,29,127); chain_midi(&C,0xB0,88,127); chain_midi(&C,0xB0,26,100);
    }
    if (scenario == 2) {  /* low clocks with audible bleed, sync 1.5 */
      chain_midi(&C,0xB0,86,23); chain_midi(&C,0xB0,25,40); chain_midi(&C,0xB0,90,127);
    }
    if (scenario == 3 || scenario == 4) { chain_init(&C, 48000.f); C.grace_left = 0; C.wet_g = 1.f; float q[11]; for (int i=0;i<11;i++) q[i]=devp[i]; chain_device_controls(&C, q); if (devfmax > 0.f) C.sh.fmax = devfmax; }
    chain_midi(&C,0x90,36,127);
    return;
  }
  if (scenario == 4) {   /* knob being turned: a new control value every block (FREQ) */
    static unsigned k; float q[11]; for (int i=0;i<11;i++) q[i]=devp[i];
    q[4] = 0.4f + 0.004f * (float)(k++ % 100); chain_device_controls(&C, q);
  }
  for (int i=0;i<64;i++) in[i]=0.f;
  chain_process(&C, in, outL, outR, 64);
}
