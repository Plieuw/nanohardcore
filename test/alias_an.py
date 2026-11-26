import numpy as np, sys, scipy.signal as sg
FS=44100; OS=8; N=FS*2
def spec(x,fs):
    x=x[-fs:]; w=np.hanning(len(x)); X=np.fft.rfft(x*w); f=np.fft.rfftfreq(len(x),1/fs)
    return f, 20*np.log10(np.abs(X)/ (w.sum()/2) +1e-12)
for fn in sys.argv[1:]:
    d=np.fromfile(fn,dtype=np.float32); a=d[:N].astype(float); b=d[N:].astype(float)
    bd=sg.resample_poly(b,1,OS,window=('kaiser',12.0))
    fa,A=spec(a,FS); fb,B=spec(bd,FS)
    band=(fa>20)&(fa<18000)
    top=A[band].max()
    # peaks of A
    pk,_=sg.find_peaks(A,height=top-90,distance=3)
    pk=[p for p in pk if 20<fa[p]<18000]
    worst=-200; wf=0; matched=0
    for p in pk:
        if B[p] >= A[p]-6: matched+=1
        else:
            lvl=A[p]-top
            if lvl>worst: worst,wf=lvl,fa[p]
    # overall: energy of spectrum where |A-B|>6dB
    print(f"{fn}: {len(pk)} peaks within 90 dB, {matched} also in 8x reference; worst model-only component {worst:.1f} dB rel. max at {wf:.0f} Hz")
