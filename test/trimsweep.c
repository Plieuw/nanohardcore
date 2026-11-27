/* kick every 250 ms while TRIM ramps 0 dB -> +45 dB over 12 s; prints per-hit metrics */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../dsp/chain.h"
#define FS 44100
static float in[FS*12], out[FS*12];
int main(int argc,char**argv){
  static Chain c; chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
  int n=FS*12; memset(in,0,sizeof in);
  for(int t=FS/10;t<n-FS/4;t+=FS/4) for(int i=0;i<220;i++) in[t+i]=0.9f;
  for(int i=0;i<n;i+=64){ float tr=0.25f+0.75f*(float)i/n; c.mk.k_trim=tr; c.dirty=1; chain_process(&c,in+i,out+i,0,64); }
  FILE*f=fopen(argv[1],"wb"); fwrite(out,4,n,f); fclose(f);
  /* clean reference: same hit at trim 0 dB, scaled */
  for(int h=0;h<46;h+=5){ int t0=FS/10+h*(FS/4)+80, L=FS/5; double pk=0,rms=0; for(int i=0;i<L;i++){ double v=out[t0+i]; if(fabs(v)>pk)pk=fabs(v); rms+=v*v;} rms=sqrt(rms/L);
    printf("hit %2d trim %+5.1f dB: peak %6.1f dBFS crest %4.1f dB\n",h,-15+60*(0.25+0.75*(t0)/(double)n),20*log10(pk),20*log10(pk/rms)); }
}
