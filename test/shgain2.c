#include <stdio.h>
#include <math.h>
#include "../dsp/sherman.h"
#define FS 44100
static float y[FS];
int main(){ printf("INPUT  gain   THD    >2kHz-energie (helderheid)  H3     H5     H9\n");
 for(int k=0;k<=10;k++){ float kin=k/10.f; static Sherman s; sherman_init(&s,FS); s.k_f1=1; s.k_f2=1; s.k_r1=0; s.k_r2=0; s.k_env1=0; s.k_bleed=0; s.f[0].morph=0; s.f[1].morph=0; s.k_in=kin; sherman_update(&s);
  int n=FS; for(int i=0;i<n;i++) y[i]=sherman_tick(&s,sinf(2*M_PI*200*i/(float)FS));
  double h[40]={0},tot=0,hi=0; for(int m=1;m<40;m++){ double re=0,im=0; for(int i=n/2;i<n;i++){re+=y[i]*cos(2*M_PI*200*m*i/FS); im+=y[i]*sin(2*M_PI*200*m*i/FS);} h[m]=sqrt(re*re+im*im); tot+=h[m]*h[m]; if(m*200>2000) hi+=h[m]*h[m]; }
  double thd=sqrt(tot-h[1]*h[1])/h[1];
  printf("%.1f  %+4.0f dB  %5.1f%%   %6.1f dB                    %5.1f%% %5.1f%% %5.1f%%\n",kin,kin*60-20,100*thd,10*log10(hi/tot),100*h[3]/h[1],100*h[5]/h[1],100*h[9]/h[1]); } }
