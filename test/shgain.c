/* Sherman input-stage behaviour: harmonics vs INPUT, and resonance push-away vs INPUT. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../dsp/sherman.h"
#define FS 44100
static float y[FS*2];
static void open_setup(Sherman*s){ sherman_init(s,FS); s->k_f1=1.0f; s->k_f2=1.0f; s->k_r1=0; s->k_r2=0; s->k_env1=0; s->k_bleed=0; s->f[0].morph=0; s->f[1].morph=0; s->k_ser=0; sherman_update(s);}
int main(int argc,char**argv){
  int lim = argc>1 ? atoi(argv[1]) : 0;
  printf("INPUT  gain   H2      H3      THD   (200 Hz sine, 1 V peak, filters open)\n");
  for(int k=0;k<=10;k++){ float kin=k/10.f; static Sherman s; open_setup(&s); s.k_in=kin; s.limit=lim; sherman_update(&s);
    int n=FS; for(int i=0;i<n;i++) y[i]=sherman_tick(&s, sinf(2*M_PI*200*i/(float)FS));
    double h[8]={0}; for(int m=1;m<8;m++){ double re=0,im=0; for(int i=n/2;i<n;i++){ re+=y[i]*cos(2*M_PI*200*m*i/FS); im+=y[i]*sin(2*M_PI*200*m*i/FS);} h[m]=sqrt(re*re+im*im); }
    double thd=0; for(int m=2;m<8;m++) thd+=h[m]*h[m]; thd=sqrt(thd)/h[1];
    printf("%.1f  %+5.0f dB  %5.1f%%  %5.1f%%  %5.1f%%\n",kin,kin*60-20,100*h[2]/h[1],100*h[3]/h[1],100*thd); }
  printf("\nresonance prominence vs INPUT (LP 1 kHz, reso .9, noise in) %s\n", lim?"LIMIT on":"");
  for(int k=0;k<=10;k+=2){ float kin=k/10.f; static Sherman s; sherman_init(&s,FS); s.k_f1=log2f(1000/2.f)/12.6f; s.k_r1=0.9f; s.k_f2=1; s.k_r2=0; s.k_env1=0; s.k_bleed=0; s.f[0].morph=0; s.f[1].morph=0.5f; s.f[1].corr=-1; s.k_in=kin; s.limit=lim; sherman_update(&s);
    unsigned r=1; int n=FS*2; for(int i=0;i<n;i++){ r=r*1664525u+1013904223u; y[i]=sherman_tick(&s,0.3f*((int)r/2147483648.f)); }
    /* power near fc vs broadband 100-400 Hz (passband) */
    double pr=0,pb=0; int N=n/2;
    for(double f=900; f<=1100; f+=20){ double re=0,im=0; for(int i=N;i<n;i+=2){re+=y[i]*cos(2*M_PI*f*i/FS); im+=y[i]*sin(2*M_PI*f*i/FS);} pr+=re*re+im*im;}
    for(double f=200; f<=400; f+=20){ double re=0,im=0; for(int i=N;i<n;i+=2){re+=y[i]*cos(2*M_PI*f*i/FS); im+=y[i]*sin(2*M_PI*f*i/FS);} pb+=re*re+im*im;}
    printf("INPUT %.1f (%+3.0f dB): resonance peak %+5.1f dB above passband\n",kin,kin*60-20,10*log10(pr/pb)); }
}
