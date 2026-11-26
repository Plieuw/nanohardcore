#include <stdio.h>
#include <math.h>
#include "../dsp/sherman.h"
#include "../dsp/bd909.h"
#define FS 44100
static float y[FS];
int main(){
 printf("INPUT   grondtoon 200 Hz   octaaf 400 Hz   (sinus 1 V, filters open)   DC uit\n");
 for(int k=6;k<=10;k++){ float kin=k/10.f; static Sherman s; sherman_init(&s,FS); s.k_f1=1; s.k_f2=1; s.k_r1=0; s.k_r2=0; s.k_env1=0; s.k_bleed=0; s.f[0].morph=0; s.f[1].morph=0; s.k_in=kin; sherman_update(&s);
  int n=FS; for(int i=0;i<n;i++) y[i]=sherman_tick(&s,sinf(2*M_PI*200*i/(float)FS));
  double h[3]={0},dc=0; for(int m=1;m<3;m++){ double re=0,im=0; for(int i=n/2;i<n;i++){re+=y[i]*cos(2*M_PI*200*m*i/FS); im+=y[i]*sin(2*M_PI*200*m*i/FS);} h[m]=sqrt(re*re+im*im)/(n/4); }
  for(int i=n/2;i<n;i++) dc+=y[i]; dc/=n/2;
  printf("%.1f     %6.1f dB          %6.1f dB          %+.3f V\n",kin,20*log10(h[1]+1e-9),20*log10(h[2]+1e-9),dc); }
 /* resonance still behaves: LP 800 Hz reso .8 with kick, peak level and no runaway */
 for(int k=8;k<=10;k++){ static BD909 b; static Sherman s; bd909_init(&b,FS); sherman_init(&s,FS); s.k_f1=log2f(400)/12.6f; s.k_r1=0.8f; s.k_f2=1; s.k_r2=0; s.k_bleed=0; s.k_env1=0; s.k_in=k/10.f; sherman_update(&s);
  bd909_trigger(&b,1); float pk=0, tail=0; for(int i=0;i<FS;i++){ float v=sherman_tick(&s,bd909_tick(&b)); if(fabsf(v)>pk)pk=fabsf(v); if(i>FS*0.8) tail=fmaxf(tail,fabsf(v)); }
  printf("kick, LP 800 Hz reso .8, INPUT %.1f: piek %.2f V, rest na 0.8 s %.4f V\n",k/10.f,pk,tail); }
}
