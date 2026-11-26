import bisect, collections
exec(open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)),'emu.py')).read().split("names=")[0])
funcs=sorted((s['st_value'],s.name) for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_info']['type']=='STT_FUNC')
addrs=[a for a,_ in funcs]
hist=collections.Counter()
def hook2(uc,addr,size,ud):
    i=bisect.bisect_right(addrs,addr)-1; hist[funcs[i][1]]+=1
for scen in (0,1):
    run(0,scen); h=mu.hook_add(UC_HOOK_CODE,hook2); hist.clear()
    for b in range(20): run(1,scen)
    mu.hook_del(h)
    tot=sum(hist.values()); print("scenario",scen)
    for n,c in hist.most_common(8): print(f"   {n:24s} {c/20/64:7.0f} instr/sample {100*c/tot:5.1f}%")
