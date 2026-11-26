#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../dsp/mackie.h"
/* response of mackie_tick at small level, relative to flat settings: eqresp hi mid lo lowcut */
static double resp(float hi,float mid,float lo,int lc,double f){
  static Mackie m; mackie_init(&m,44100); m.k_hi=hi; m.k_mid=mid; m.k_lo=lo; m.lowcut=lc; m.k_trim=0.25f; mackie_update(&m);
  int n=44100; double sr=0,si=0; 
  for(int i=0;i<n;i++){ double x=0.05*sin(2*M_PI*f*i/44100.0); float y=mackie_tick(&m,(float)x);
    if(i>=n/2){ sr+=y*cos(2*M_PI*f*i/44100.0); si+=y*sin(2*M_PI*f*i/44100.0);} }
  return sqrt(sr*sr+si*si)/(n/2)*2/0.05;
}
int main(int c,char**v){
  float hi=atof(v[1]),mid=atof(v[2]),lo=atof(v[3]); int lc=atoi(v[4]);
  double F[]={20,50,80,200,500,1000,2500,5000,8000,12000,16000,20000};
  for(int k=0;k<12;k++){ double r=resp(hi,mid,lo,lc,F[k])/resp(0,0,0,0,F[k]); printf("%g:%+.1f ",F[k],20*log10(r)); }
  printf("\n");
}
