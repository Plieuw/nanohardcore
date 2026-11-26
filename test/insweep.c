/* kick every 250 ms through the chain while Sherman INPUT ramps -20 -> +40 dB (12 s).
 * Filter 1 LP ~600 Hz reso .7, Mackie clean, so you hear only the Sherman input stage. */
#include <stdio.h>
#include <string.h>
#include "../dsp/chain.h"
#define FS 44100
static float in[FS*12], out[FS*12];
int main(int argc,char**argv){
  static Chain c; chain_init(&c,FS);
  chain_midi(&c,0xB0,25,(unsigned char)(127*0.66)); chain_midi(&c,0xB0,26,90); chain_midi(&c,0xB0,88,64+30);
  if(argc>2) chain_midi(&c,0xB0,81,127);
  int n=FS*12; memset(in,0,sizeof in);
  for(int t=FS/10;t<n-FS/4;t+=FS/4) for(int i=0;i<220;i++) in[t+i]=0.9f;
  for(int i=0;i<n;i+=64){ c.sh.k_in=(float)i/n; chain_process(&c,in+i,out+i,0,64); }
  FILE*f=fopen(argv[1],"wb"); fwrite(out,4,n,f); fclose(f);
}
