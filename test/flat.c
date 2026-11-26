#include <stdio.h>
#include <math.h>
#include "../dsp/mackie.h"
int main(){ double F[]={50,1000,5000,10000,15000,18000,20000};
 for(int k=0;k<7;k++){ static Mackie m; mackie_init(&m,44100); m.k_trim=0.25f; mackie_update(&m);
  int n=44100; double sr=0,si=0,f=F[k];
  for(int i=0;i<n;i++){ float y=mackie_tick(&m,0.05f*(float)sin(2*M_PI*f*i/44100.0)); if(i>=n/2){sr+=y*cos(2*M_PI*f*i/44100.0); si+=y*sin(2*M_PI*f*i/44100.0);} }
  printf("%g Hz: %+.2f dB\n",f,20*log10(sqrt(sr*sr+si*si)/(n/4)/0.05)); } }
