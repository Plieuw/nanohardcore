#include <stdio.h>
#include <math.h>
#include "../dsp/sherman.h"
#define FS 44100
static void setup(Sherman*s,float morph,float corr){ sherman_init(s,FS); s->k_f1=log2f(1000/2.f)/12.6f; s->k_r1=0.5f; s->k_f2=1; s->k_r2=0; s->k_ser=1; s->k_env1=0; s->k_bleed=0; s->k_in=0.2f; s->f[0].morph=morph; s->f[0].corr=corr; s->f[1].morph=0; sherman_update(s);}
static double gain(float morph,float corr,double f){ static Sherman s; setup(&s,morph,corr); int n=FS/2; double re=0,im=0;
  for(int i=0;i<n;i++){ float y=sherman_tick(&s,0.05f*(float)sin(2*M_PI*f*i/FS)); if(i>=n/2){re+=y*cos(2*M_PI*f*i/FS); im+=y*sin(2*M_PI*f*i/FS);} }
  return 20*log10(sqrt(re*re+im*im)/(n/4)/0.05/ (0.2*60-20 < 0 ? 1 : 1)); }
int main(){ double F[]={100,300,700,1000,1500,3000,8000};
  printf("respons (dB, fc 1 kHz, reso .5), L-B-H-stand:\n  stand  "); for(int i=0;i<7;i++) printf("%7.0fHz",F[i]); printf("\n");
  float ms[]={0,.25f,.5f,.75f,1}; const char*nm[]={"LP","LP/BP","BP","BP/HP","HP"};
  double ref=gain(0,0,100);
  for(int k=0;k<5;k++){ printf("  %-6s",nm[k]); for(int i=0;i<7;i++) printf("%+8.1f ",gain(ms[k],0,F[i])-ref); printf("\n"); }
  printf("  notch (BP + correctie -B+LH):"); for(int i=0;i<7;i++) printf("%+7.1f ",gain(.5f,1,F[i])-ref); printf("\n");
  /* zipper test: morph swept 0->1->0 in 7-bit CC steps, updated every 64 samples, over 2 s,
   * 220 Hz sine in; energy NOT at 220 Hz harmonics = clicks / zipper */
  static float y[FS*2];
  for(int smooth=-1;smooth<2;smooth++){ static Sherman s; setup(&s,0,0); if(smooth==0) s.a_sm=0;
    int n=FS*2;
    for(int i=0;i<n;i+=64){ int step=(int)(127.0*(i<n/2? (double)i/(n/2) : 2-(double)i/(n/2))); s.f[0].morph=0.5f; s.k_f1 = smooth<0 ? 0.6f : 0.3f+0.6f*step/127.f;
      for(int j=0;j<64 && i+j<n;j++) y[i+j]=sherman_tick(&s,0.5f*sinf(2*M_PI*220*(i+j)/(float)FS)); }
    double tot=0,har=0;
    for(int f=20; f<20000; f+=5){ double re=0,im=0; for(int i=0;i<n;i+=2){ re+=y[i]*cos(2*M_PI*f*i/FS); im+=y[i]*sin(2*M_PI*f*i/FS);} double p=re*re+im*im; tot+=p; if(f<3000) har+=p; }
    printf("%s: energie boven 3 kHz (klikken) %.1f dB onder het signaal\n", smooth<0?"cutoff vast (referentie)":(smooth?"cutoff-sweep met smoothing  ":"cutoff-sweep zonder smoothing"), -10*log10((tot-har)/har)); }
}
