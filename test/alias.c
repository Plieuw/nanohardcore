/* Model-aliasing check: Sherman at fs vs at 8*fs (decimated) on identical analytic input.
 * In-band difference = aliasing introduced by the discrete model (should be small);
 * the hardware's own sampled-data aliasing is present in BOTH and cancels. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "../dsp/sherman.h"
#define FS 44100
#define OS 8
#define N (FS*2)
static float in1[N], out1[N], inO[N*OS], outO[N*OS];
static void setup(Sherman *s, float fs, int harm, float f1, float r, float bleed){
  sherman_init(s, fs); s->harm=harm; s->k_f1=f1; s->k_f2=0.45f; s->k_r1=r; s->k_r2=r; s->k_ser=0.5f;
  s->k_env1=0; s->k_bleed=bleed; s->k_in=0.3f; s->f[0].morph=0.5f; s->f[1].morph=0.0f; sherman_update(s);}
int main(int argc,char**argv){
  int harm=atoi(argv[1]); float f1=atof(argv[2]), r=atof(argv[3]), bl=atof(argv[4]);
  for(int i=0;i<N*OS;i++){ double t=(double)i/(FS*OS); inO[i]=0.4*sin(2*M_PI*97*t)+0.25*sin(2*M_PI*1310*t)+0.15*sin(2*M_PI*5210*t);} 
  for(int i=0;i<N;i++) in1[i]=inO[i*OS];
  static Sherman a,b; setup(&a,FS,harm,f1,r,bl); setup(&b,FS*OS,harm,f1,r,bl);
  for(int i=0;i<N;i++) out1[i]=sherman_tick(&a,in1[i]);
  for(int i=0;i<N*OS;i++) outO[i]=sherman_tick(&b,inO[i]);
  FILE*f=fopen(argv[5],"wb"); fwrite(out1,4,N,f); fwrite(outO,4,N*OS,f); fclose(f);
  printf("fclk1 = %.0f Hz\n", 2*pow(2,12.6*f1)*50);
}
