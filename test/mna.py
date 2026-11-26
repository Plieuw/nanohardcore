# Linear MNA of the Mackie 1202-VLZ PRO channel EQ (U27B) and 75 Hz HPF (U13A), from sheet 1.
import numpy as np
def solve(elems, nodes, fixed, opamps, out, f):
    """elems: (type,a,b,val); fixed: {node:value}; opamps: [(outnode, p, n)] ideal (V(p)=V(n))."""
    unk=[n for n in nodes if n not in fixed]; idx={n:i for i,n in enumerate(unk)}
    s=2j*np.pi*f; N=len(unk); M=np.zeros((N,N),complex); r=np.zeros(N,complex)
    outs={o for o,_,_ in opamps}
    def stamp(a,b,y):
        for p,q,sg in ((a,b,1),(b,a,1)):
            if p in idx and p not in outs:
                M[idx[p],idx[p]]+=y
                if q in idx: M[idx[p],idx[q]]-=y
                else: r[idx[p]]+=y*fixed[q]
    for t,a,b,v in elems:
        stamp(a,b, 1/v if t=='R' else s*v)
    for o,p,n in opamps:
        row=idx[o]
        for nd,sg in ((p,1),(n,-1)):
            if nd in idx: M[row,idx[nd]]+=sg
            else: r[row]-=sg*fixed[nd]
    x=np.linalg.solve(M,r); return x[idx[out]]
def pot(a,b,w,R,pos):  # pos 0 => wiper at a
    pos=min(max(pos,1e-4),1-1e-4); return [('R',a,w,R*pos),('R',w,b,R*(1-pos))]
def eq(hi,mid,lo,f):
    E=[('C','A','H1',.018e-6)]+pot('H1','H2','WH',50e3,hi)+[('C','H2','O',.018e-6),('R','WH','Y',1.1e3),
       ('R','A','M1',910),('C','M1','M2',.012e-6)]+pot('M1','M2','WM',50e3,mid)+[('R','M2','O',910),('R','WM','Y',1.3e3),
       ('R','A','L1',6.8e3)]+pot('L1','L2','WL',50e3,lo)+[('C','L1','WL',.033e-6),('C','L2','WL',.033e-6),('R','L2','O',6.8e3),
       ('R','WL','X',10e3),('C','Y','X',.012e-6),('R','X','O',1e6),('C','X','O',10e-12)]
    nodes=['A','G','H1','H2','WH','M1','M2','WM','L1','L2','WL','X','Y','O']
    return solve(E,nodes,{'A':1.0,'G':0.0},[('O','G','X')],'O',f)
def hpf(f):
    E=[('C','A','Q1',.22e-6),('R','Q1','G',5.1e3),('C','Q1','Q2',.22e-6),('R','Q2','O',1.3e3),('C','Q2','Q3',.22e-6),('R','Q3','G',62e3)]
    return solve(E,['A','G','Q1','Q2','Q3','O'],{'A':1.0,'G':0.0},[('O','Q3','O')],'O',f)
db=lambda h:20*np.log10(abs(h))
F=[20,50,80,200,500,1000,2500,5000,8000,12000,16000,20000]
print("HPF:", " ".join(f"{f}:{db(hpf(f)):.1f}" for f in F))
for name,args in [("flat",(.5,.5,.5)),("hi 0",(0,.5,.5)),("hi 1",(1,.5,.5)),("mid 0",(.5,0,.5)),("mid 1",(.5,1,.5)),("lo 0",(.5,.5,0)),("lo 1",(.5,.5,1))]:
    print(f"{name:6s}", " ".join(f"{f}:{db(eq(*args,f)):+.1f}" for f in F))
