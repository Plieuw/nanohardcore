/* L-B-H sweep LP -> BP -> HP -> BP -> LP over 12 s via 7-bit CC 27, kick every 250 ms */
#include <stdio.h>
#include <string.h>
#include "../dsp/chain.h"
#define FS 44100
static float in[FS*12], out[FS*12];
int main(int argc,char**argv){ static Chain c; chain_init(&c, FS); c.grace_left = 0; c.wet_g = 1.f;
  chain_midi(&c,0xB0,25,80); chain_midi(&c,0xB0,26,80); chain_midi(&c,0xB0,88,64+25);
  chain_midi(&c,0xB0,29,127); chain_midi(&c,0xB0,30,0); chain_midi(&c,0xB0,87,127);
  int n=FS*12; memset(in,0,sizeof in); for(int t=FS/10;t<n-FS/4;t+=FS/4) for(int i=0;i<220;i++) in[t+i]=0.9f;
  for(int i=0;i<n;i+=64){ double p=(double)i/n; int v=(int)(127*(p<0.5?2*p:2-2*p)); chain_midi(&c,0xB0,27,(unsigned char)v); chain_process(&c,in+i,out+i,0,64); }
  FILE*fp=fopen(argv[1],"wb"); fwrite(out,4,n,fp); fclose(fp); }
