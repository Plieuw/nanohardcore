#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../dsp/bd909.h"
/* usage: fit909 vth kcv pulse_ms sat tune decay -> prints pitch at checkpoints, hold time */
int main(int c, char**v){
  static BD909 b; bd909_init(&b,44100);
  b.cal_vth=atof(v[1]); b.cal_kcv=atof(v[2]); b.cal_pulse_ms=atof(v[3]); b.cal_sat=atof(v[4]);
  bd909_set(&b,atof(v[5]),atof(v[6]),0,1.0f); bd909_trigger(&b,1.0f);
  int n=44100; static float y[44100]; for(int i=0;i<n;i++) y[i]=bd909_tick(&b);
  double zc[4000]; int nz=0;
  for(int i=1;i<n;i++) if(y[i-1]<0 && y[i]>=0 && nz<4000) zc[nz++]=i-1+(-y[i-1])/(y[i]-y[i-1]);
  double ck[]={5,10,20,40,80,150,300};
  for(int k=0;k<7;k++){ double t=ck[k]*44.1; double f=0; for(int j=1;j<nz;j++) if(zc[j]>t){ f=44100/(zc[j]-zc[j-1]); break;} printf("%.0fms:%.0fHz ",ck[k],f);}
  /* hold: time until per-cycle peak drops 1 dB below max */
  float pk=0; for(int i=0;i<n;i++) if(fabsf(y[i])>pk) pk=fabsf(y[i]);
  int last=0; for(int i=0;i<n;i++) if(fabsf(y[i])>pk*0.891f) last=i;
  int l40=0; for(int i=0;i<n;i++) if(fabsf(y[i])>pk*0.01f) l40=i;
  printf(" hold(-1dB) %.0fms  len(-40dB) %.0fms peak %.2fV\n", last/44.1, l40/44.1, pk);
}
