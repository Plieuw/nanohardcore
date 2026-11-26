#include <stdio.h>
#include <math.h>
#include "../dsp/bd909.h"
#include "../dsp/sherman.h"
#define FS 44100
int main(){ printf("INPUT   rms(dB)  >2kHz-aandeel  >5kHz-aandeel  crest\n");
 for(int k=4;k<=10;k++){ float kin=k/10.f; static BD909 b; static Sherman s; bd909_init(&b,FS); sherman_init(&s,FS);
  s.k_f1=1; s.k_f2=1; s.k_r1=0.3f; s.k_r2=0.3f; s.k_env1=0; s.k_bleed=0; s.f[0].morph=0; s.f[1].morph=0; s.k_in=kin; sherman_update(&s);
  static float y[FS/4]; int n=FS/4; bd909_trigger(&b,1.f);
  for(int i=0;i<n;i++) y[i]=sherman_tick(&s,bd909_tick(&b));
  /* spectrum by DFT bins (coarse) */
  double tot=0,h2=0,h5=0,pk=0,rms=0; for(int i=0;i<n;i++){ rms+=y[i]*y[i]; if(fabs(y[i])>pk)pk=fabs(y[i]); }
  for(int f=20; f<20000; f+=20){ double re=0,im=0; for(int i=0;i<n;i+=1){ re+=y[i]*cos(2*M_PI*f*i/FS); im+=y[i]*sin(2*M_PI*f*i/FS);} double p=re*re+im*im; tot+=p; if(f>2000)h2+=p; if(f>5000)h5+=p; }
  printf("%.1f   %6.1f    %6.1f dB      %6.1f dB     %4.1f dB\n",kin,10*log10(rms/n),10*log10(h2/tot),10*log10(h5/tot),20*log10(pk/sqrt(rms/n))); } }
