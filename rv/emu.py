import sys, os
from unicorn import *
from unicorn.riscv_const import *
from elftools.elf.elffile import ELFFile
elf=ELFFile(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'test.elf'),'rb'))
mu=Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
mu.mem_map(0x0, 0x01000000)
for seg in elf.iter_segments():
    if seg['p_type']=='PT_LOAD':
        mu.mem_write(seg['p_paddr'], seg.data())
sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
start=sym['_start']
try: mu.reg_write(UC_RISCV_REG_MSTATUS, 0x6000)
except Exception as e: print("mstatus",e)
count=[0]
def hook(uc,addr,size,ud): count[0]+=1
mu.hook_add(UC_HOOK_CODE, hook)
ebreak=None
def run(mode,scen):
    mu.mem_write(sym['mode'], mode.to_bytes(4,'little')); mu.mem_write(sym['scenario'], scen.to_bytes(4,'little'))
    count[0]=0
    mu.reg_write(UC_RISCV_REG_PC, start)
    # find ebreak address: run until ebreak (exception)
    try: mu.emu_start(start, 0xFFFFFFF0, count=200_000_000)
    except UcError as e:
        pc=mu.reg_read(UC_RISCV_REG_PC)
        return count[0], str(e), pc
    return count[0], 'end', 0
names={0:'default (free, fc env)',1:'worst: both cutoffs max',2:'sync 1.5 low clocks + bleed'}
for scen in (0,1,2):
    c,e,pc=run(0,scen)
    tot=0; worst=0; nb=60
    for b in range(nb):
        c,e,pc=run(1,scen); tot+=c; worst=max(worst,c)
    print(f"{names[scen]:32s} init {0}  avg {tot/nb/64:7.0f} instr/sample  worst block {worst/64:7.0f}/sample  -> {tot/nb/64*44100/1e6:6.1f} MIPS   ({e[:30]})")
c,e,pc=run(0,0)
print("stop pc", hex(pc))
import struct
outs=[]
for b in range(40):
    run(1,0); outs+=struct.unpack('<64f', mu.mem_read(sym['outL'],256))
import numpy as np
outs=np.array(outs); print("rv out peak", np.abs(outs).max(), "rms", outs.std(), "nan", np.isnan(outs).any())
np.save('/tmp/rv_out.npy', outs)
