/* resonance behaviour: does it self-oscillate (no input after an impulse) per cutoff/reso/filter */
#include <stdio.h>
#include <math.h>
#include "../dsp/sherman.h"
#define FS 44100
static float level(int filt, float fc, float reso){
  static Sherman s; sherman_init(&s,FS); s.k_env1=0; s.k_env2=0; s.k_bleed=0; s.k_ser=0;
  float k = log2f(fc/2.f)/12.6f;
  if(filt==0){ s.k_f1=k; s.k_r1=reso; s.k_f2=0.0f; s.k_r2=0; s.f[1].morph=0; }
  else       { s.k_f2=k; s.k_r2=reso; s.k_f1=0.0f; s.k_r1=0; }
  s.f[0].morph=0.5f; s.f[1].morph = filt? 0.5f : s.f[1].morph;  /* band-pass */
  sherman_update(&s);
  float pk=0; for(int i=0;i<FS*2;i++){ float x = i<2 ? 1.0f : 0.f; float y=sherman_tick(&s,x); if(i>FS*3/2 && fabsf(y)>pk) pk=fabsf(y); }
  return pk;   /* amplitude still present 1.5 s after a click */
}
int main(){
  float fcs[]={50,150,500,1500,3000,5000,8000};
  for(int f=0;f<2;f++){ printf("filter %d   (piek 1,5 s na een klik; >0.1 V = zelfoscillatie)\n reso  ",f+1);
    for(int i=0;i<7;i++) printf("%7.0fHz",fcs[i]); printf("\n");
    float rs[]={0.3f,0.6f,0.8f,0.9f,1.0f};
    for(int r=0;r<5;r++){ printf(" %.1f  ",rs[r]); for(int i=0;i<7;i++){ float v=level(f,fcs[i],rs[r]); printf("%9s", v>0.1f? "OSC" : (v>1e-3f? "ringt":"-")); } printf("\n"); } }
  printf("\nQ per resostand (zonder klokeffect): ");
  for(int r=0;r<=10;r+=2){ float rr=r/10.f; float d=1.4f*powf(0.05f/1.4f,powf(rr+1e-6f,0.7f)); printf("%.1f:Q%.1f ",rr,1/d);} printf("\n");
}
