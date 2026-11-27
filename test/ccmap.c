/* every mapped CC must change the output; channel-mode CCs (120-127) must not */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../dsp/chain.h"
#define FS 44100
#define N (FS*3/4)
static float in[N], oL[N], oR[N], rL[N], rR[N];
static void run(int cc, int val, float *L, float *R){
  static Chain c; chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
  /* a context where every control is audible: low clocks for bleed, envelopes on, mild drive */
  chain_midi(&c,0xB0,25,60); chain_midi(&c,0xB0,26,70); chain_midi(&c,0xB0,29,50); chain_midi(&c,0xB0,30,70);
  chain_midi(&c,0xB0,88,90); chain_midi(&c,0xB0,89,40); chain_midi(&c,0xB0,27,30); chain_midi(&c,0xB0,31,30);
  chain_midi(&c,0xB0,104,40); chain_midi(&c,0xB0,90,60); chain_midi(&c,0xB0,107,60);
  if(cc>=0) chain_midi(&c,0xB0,(unsigned char)cc,(unsigned char)val);
  for(int i=0;i<N;i+=64){ int m=N-i<64?N-i:64; if(i/64==(FS/4)/64) chain_midi(&c,0x90,36,90); chain_process(&c,in+i,L+i,R+i,(uint32_t)m); }
}
static double diff(const float*a,const float*b){ double d=0,e=0; for(int i=0;i<N;i++){ d+=(a[i]-b[i])*(a[i]-b[i]); e+=b[i]*b[i]; } return 10*log10(d/(e+1e-20)+1e-20); }
int main(){
  memset(in,0,sizeof in); for(int t=FS/20;t<N-500;t+=FS/2) for(int i=0;i<220;i++) in[t+i]=0.05f;
  for(int i=0;i<220;i++) in[FS/20+FS*6/100+i]=0.05f;   /* second hit 60 ms later: hold-off decides */
  run(-1,0,rL,rR);
  const char* names[]={"909 TUNE","909 DECAY","909 ATTACK","909 LEVEL","909 ACCENT","909 accent bron","909 pitch","909 sweep-diepte",
    "Sh INPUT","Sh FREQ1","Sh RESO1","Sh L-B-H1","Sh CORR1","Sh FREQ2","Sh RESO2","Sh L-B-H2","Sh CORR2","Sh HARMONICS","Sh PAR/SER","Sh ADSR>f1","Sh ADSR>f2","Sh bleed",
    "Sh A","Sh D","Sh S","Sh R","Sh ADSR-snelheid","Sh klokratio","Sh HI","Sh LIMIT","Sh min. gate",
    "Mk TRIM","Mk LOW CUT","Mk LOW","Mk MID","Mk HI","Mk GAIN","Mk PAN","Mk MUTE","Mk MAIN",
    "trig gevoelig","hold-off","thru","out","lijnniveau","auto level","input-modus"};
  int ccs[]={20,21,22,23,14,15,16,17, 24,25,26,27,28,29,30,31,85,86,87,88,89,90, 102,103,104,105,5,106,80,81,82, 107,108,109,110,111,112,18,19,113, 114,115,116,117,118,119,3};
  int vals[]={127,127,127,10,10,127,100,120,110,110,127,127,127,110,127,127,127,64,127,127,127,127,100,100,127,110,0,127,127,127,127,120,127,127,127,127,127,0,127,120,127,0,127,40,127,0,127};
  int n=sizeof(ccs)/sizeof(ccs[0]), bad=0;
  for(int k=0;k<n;k++){ run(ccs[k],vals[k],oL,oR); double dl=diff(oL,rL), dr=diff(oR,rR); double d=dl>dr?dl:dr;
    printf("CC %3d %-18s verschil %6.1f dB %s\n",ccs[k],names[k],d, d<-60?"  <-- GEEN EFFECT":""); if(d<-60) bad++; }
  for(int cc=120;cc<=127;cc++){ run(cc,127,oL,oR); double d=diff(oL,rL); printf("CC %3d (kanaalmodus)      verschil %6.1f dB %s\n",cc,d,d>-100?"  <-- reageert!":"ok, genegeerd"); }
  printf("%d van %d CC's zonder effect\n",bad,n);
}
