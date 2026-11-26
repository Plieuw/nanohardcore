#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../dsp/chain.h"
#define FS 44100
static float in[FS*3], out[FS*3];
static void cc(Chain*c,int n,int v){ chain_midi(c,0xB0,n,v); }
int main(){
  static Chain c;
  /* 1: gate follows the input: 300 ms tone bursts, sustain .6 */
  chain_init(&c,FS); cc(&c,104,76); cc(&c,103,40); cc(&c,105,50); cc(&c,23,0);
  memset(in,0,sizeof in); for(int b=0;b<3;b++) for(int i=0;i<FS*3/10;i++) in[FS/10+b*FS*8/10+i]=0.4f*sinf(2*M_PI*220*i/(float)FS);
  printf("1) gate volgt de input (bursts van 300 ms, S=0.6):\n");
  for(int i=0;i<FS*2;i+=64){ chain_process(&c,in+i,out+i,0,64); if(i%(FS/20)<64) printf("   t=%4.0f ms  input %s  gate %d  env %.2f  stage %d\n",i*1000.0/FS, fabsf(in[i])>0||fabsf(in[i+10])>0?"aan":"uit",c.gate,c.sh.env,c.sh.env_stage); }
  /* 2: attack lock: triggers every 125 ms, attack 200 ms */
  printf("2) attack-lock: triggers elke 125 ms, attack ~200 ms\n");
  for(int lock=1;lock>=0;lock--){ chain_init(&c,FS); cc(&c,102,(int)(127*log2f(0.2f/0.002f)/13.87f)); cc(&c,123,lock?127:0); cc(&c,23,0);
    memset(in,0,sizeof in); for(int t=FS/10;t<FS*2;t+=FS/8) for(int i=0;i<220;i++) in[t+i]=0.9f;
    int restarts=0, prev=0;
    for(int i=0;i<FS*2;i+=64){ chain_process(&c,in+i,out+i,0,64); int st=c.sh.env_stage; if(st==1 && prev!=1) restarts++; prev=st; }
    printf("   lock %s: %u triggers, ADSR-attack gestart %d keer\n", lock?"aan (hardware)":"uit", c.trig_count, restarts); }
  /* 3: bipolar + true zero */
  printf("3) diepte per filter (env = 1.0, free mode):\n");
  int vals[]={0,32,63,64,65,96,127};
  for(int k=0;k<7;k++){ float d=((float)vals[k]-64.f)/63.f; if(d<-1)d=-1; float oct=7.f*d*fabsf(d); printf("   CC %3d -> %+6.3f oct\n",vals[k],oct); }
  return 0;
}
