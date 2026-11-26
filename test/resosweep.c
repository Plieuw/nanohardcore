#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../dsp/chain.h"
#define FS 44100
static float in[FS*12], out[FS*12];
int main(int argc,char**argv){ static Chain c; chain_init(&c,FS);
  chain_midi(&c,0xB0,26,115); chain_midi(&c,0xB0,27,64); chain_midi(&c,0xB0,88,64);
  chain_midi(&c,0xB0,29,127); chain_midi(&c,0xB0,30,0); chain_midi(&c,0xB0,31,0); chain_midi(&c,0xB0,87,127); /* serial, f2 wide open */
  int n=FS*12; memset(in,0,sizeof in); for(int t=FS/10;t<n-FS/4;t+=FS/2) for(int i=0;i<220;i++) in[t+i]=0.9f;
  for(int i=0;i<n;i+=64){ float f=50*powf(200.f,(float)i/n); c.sh.k_f1=log2f(f/2)/12.6f; chain_process(&c,in+i,out+i,0,64); }
  FILE*fp=fopen(argv[1],"wb"); fwrite(out,4,n,fp); fclose(fp); }
