/* Device-facing behaviour: boot grace, input modes, 48 kHz, the 12-control bank. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../dsp/chain.h"
#define FS 48000
#define N (FS*5)
static float in[N], oL[N], oR[N];
int main(void){
  static Chain c;
  /* 1: boot grace - dry for CHAIN_GRACE_S, processed after */
  chain_init(&c, FS);
  memset(in,0,sizeof in);
  for(int t=FS/10;t<N-FS/4;t+=FS/4) for(int i=0;i<240;i++) in[t+i]=0.6f;
  for(int i=0;i<N;i+=64){ int m=N-i<64?N-i:64; chain_process(&c,in+i,oL+i,oR+i,(uint32_t)m); }
  int grace=(int)(CHAIN_GRACE_S*FS); double dryerr=0, wet=0;
  for(int i=0;i<grace-64;i++) dryerr=fmax(dryerr,fabs(oL[i]-in[i]));
  for(int i=grace+FS/2;i<N;i++) wet=fmax(wet,fabs(oL[i]-in[i]));
  printf("grace_dry_max_err %.6f\n", dryerr);
  printf("after_grace_processed %.3f\n", wet);
  printf("triggers_48k %u\n", c.trig_count);
  /* 2: AUDIO mode: silence in -> silence out (no 909), sine in -> processed out */
  chain_init(&c, FS); c.grace_left=0; c.wet_g=1.f; chain_midi(&c,0xB0,3,127);
  float z[64]={0}, a[64], b[64]; double sil=0;
  for(int k=0;k<FS/64;k++){ chain_process(&c,z,a,b,64); for(int i=0;i<64;i++) sil=fmax(sil,fabs(a[i])); }
  double pk=0; float s[64]; int nn=0;
  for(int k=0;k<FS/64;k++){ for(int i=0;i<64;i++,nn++) s[i]=0.2f*sinf(2*M_PI*110*nn/FS); chain_process(&c,s,a,b,64); for(int i=0;i<64;i++) pk=fmax(pk,fabs(a[i])); }
  printf("audio_mode_silence %.6f\n", sil);
  printf("audio_mode_sine_out %.3f\n", pk);
  /* 3: device bank: defaults and exact zeros at 0.5 */
  chain_init(&c, FS); chain_device_controls(&c, CHAIN_DEV_DEFAULTS);
  float p[CHAIN_DEV_CONTROLS]; memcpy(p,CHAIN_DEV_DEFAULTS,sizeof p); p[7]=0.5f; p[10]=0.5f;
  chain_device_controls(&c,p);
  printf("dev_env_zero %.9f\n", c.sh.k_env1);
  printf("dev_mid_zero %.9f\n", c.mk.k_mid);
  p[8]=1.f; chain_device_controls(&c,p); printf("dev_harm_max %d\n", c.sh.harm);
  printf("state_bytes %u\n", (unsigned)sizeof(Chain));
  return 0;
}
