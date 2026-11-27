#!/usr/bin/env python3
"""Voert de gepatchte 1.2-image functioneel uit in Unicorn (geen USB, geen hardware).

Laadt de FIT-segmenten van build/fw12/nanohardcore-1.2-os.bin, voert de reset-shim uit
tot de native entry (de payload komt zo op 0x400cb880 terecht zoals bij het opstarten),
vervangt alleen de native 64-bit timer door een teller op basis van het aantal
uitgevoerde instructies, en roept dan de gehaakte functies aan met de native ABI:

  1. pitch_init(48000, native_pitch, ring, 2048) en de foutpaden ervan;
  2. 3,5 s audio via nc_octave_dispatch (subtype 8, stereo, 64 frames, stack-args):
     droog tijdens de opstart-grace, daarna verwerkt; 909-hits op de triggerpulsen;
  3. instructies per blok tegen het budget (bij ~360 instructies/us, gemeten op de pedal);
  4. de CPU-bewaking: met een trage klok latcht hij bypass en toont het scherm CPU LIMIT;
  5. de UI-functies: naam, 11 labels, waardeweergave, defaults.
Bewijst niets over caches, randapparatuur of echte deadlines.
"""
import json, struct, sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE
from unicorn import riscv_const as r

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, sys.argv[1] + '/tools/custom_fx')
from patch_image import extract_fit

IMG = (ROOT / 'build/fw12/nanohardcore-1.2-os.bin').read_bytes()
SYM = {k: int(v, 16) for k, v in json.loads((ROOT / 'build/fw12/report.json').read_text())['symbols'].items()}
TICKS, STOP = 0x4004393c, 0x40000004
FAM, PITCH, RING, ENABLE = 0x30140000, 0x30140000 + 16, 0x30150000, 0x30148000
PAR, INL, INR, OUTL, OUTR, OMODE, STR = 0x30149000, 0x3014a000, 0x3014b000, 0x3014c000, 0x3014d000, 0x3014e000, 0x3014f000
CALLS, SUM, DEADLINE, OVERRUNS = 0x3008a9f0, 0x3008a9e0, 0x3008a9fc, 0x3008aa00
FS, N = 48000, 64
INSTR_PER_US = 360.0                      # pedal-meting (octaver: 240k instr in 673 us)

u = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
u.mem_map(0x30040000, 0x200000); u.mem_map(0x40000000, 0x400000)
u.reg_write(r.UC_RISCV_REG_MSTATUS, u.reg_read(r.UC_RISCV_REG_MSTATUS) | 0x6000)   # FPU aan
for s in extract_fit(IMG)['segments']:
    u.mem_write(s['load_address'], s['data'])
u.emu_start(0x40000000, 0x40006428, count=200000)          # reset-shim: payload kopiëren
assert u.reg_read(r.UC_RISCV_REG_PC) == 0x40006428

icount = [0]; ipt = [INSTR_PER_US / 4.0]                    # instructies per tick van 4 MHz
def on_code(uc, addr, size, _):
    icount[0] += 1
    if addr == TICKS:                                       # native timer -> instructieklok
        t = int(icount[0] / ipt[0]); uc.reg_write(r.UC_RISCV_REG_A0, t & 0xffffffff)
        uc.reg_write(r.UC_RISCV_REG_A1, t >> 32); uc.reg_write(r.UC_RISCV_REG_PC, uc.reg_read(r.UC_RISCV_REG_RA))
u.hook_add(UC_HOOK_CODE, on_code)

def call(fn, args=(), fargs=(), stack=b''):
    sp = STR + 0x8000 - 64
    u.mem_write(sp, stack.ljust(64, b'\0'))
    u.reg_write(r.UC_RISCV_REG_SP, sp); u.reg_write(r.UC_RISCV_REG_RA, STOP)
    for i, a in enumerate(args): u.reg_write(getattr(r, f'UC_RISCV_REG_A{i}'), a & 0xffffffff)
    for i, f in enumerate(fargs):
        u.reg_write(getattr(r, f'UC_RISCV_REG_F{10 + i}'), 0xffffffff00000000 | struct.unpack('<I', struct.pack('<f', f))[0])
    before = icount[0]
    u.emu_start(fn, STOP, count=50_000_000)
    assert u.reg_read(r.UC_RISCV_REG_PC) == STOP, hex(u.reg_read(r.UC_RISCV_REG_PC))
    return u.reg_read(r.UC_RISCV_REG_A0), icount[0] - before

res = {}
# 1. init + foutpaden
res['init_bad_count'] = call(SYM['nc_octave_pitch_init'], (PITCH, RING, 1024), (48000.,))[0]
res['init_bad_rate'] = call(SYM['nc_octave_pitch_init'], (PITCH, RING, 2048), (22050.,))[0]
rc, n_init = call(SYM['nc_octave_pitch_init'], (PITCH, RING, 2048), (48000.,))
res['init_rc'] = rc; res['init_instructions'] = n_init
res['init_ms'] = n_init / INSTR_PER_US / 1000
state_ptr = struct.unpack('<I', u.mem_read(PITCH, 4))[0]
res['state_in_ring'] = state_ptr == RING
res['state_bytes'] = call(SYM['nc_hc_state_bytes'])[0]

# 2. audio via de dispatch-haak
u.mem_write(FAM + 8, struct.pack('<I', ENABLE)); u.mem_write(ENABLE, b'\1')
u.mem_write(PAR, struct.pack('<11f', *json.loads('[0.5,0.5,0.5,0.333,0.75,0.35,0,0.75,0,0,0.8]')))
u.mem_write(DEADLINE, struct.pack('<I', int(4e6 * N / FS)))
def block(k, inp):
    u.mem_write(INL, struct.pack(f'<{N}f', *inp)); u.mem_write(INR, struct.pack(f'<{N}f', *inp))
    _, n = call(SYM['nc_octave_dispatch'], (FAM, INL, INR, OUTL, OUTR, N, PAR, 8),
                stack=struct.pack('<II', 1, OMODE))
    calls = struct.unpack('<I', u.mem_read(CALLS, 4))[0]
    u.mem_write(CALLS, struct.pack('<I', calls + 1))
    s = struct.unpack('<I', u.mem_read(SUM, 4))[0]
    u.mem_write(SUM, struct.pack('<I', (s + int(n / ipt[0]) + 400) & 0xffffffff))   # + rest van de keten
    return list(struct.unpack(f'<{N}f', u.mem_read(OUTL, 4 * N))), list(struct.unpack(f'<{N}f', u.mem_read(OUTR, 4 * N))), n

grace_blocks = int(3.0 * FS / N); total = int(3.6 * FS / N)
dry_err = 0.; wet_peak = 0.; per_block = []; t = 0
for k in range(total):
    inp = [0.6 if (t + i) % (FS // 4) < 240 else 0. for i in range(N)]; t += N
    ol, orr, n = block(k, inp)
    if k < grace_blocks - 1:
        f32 = [struct.unpack('<f', struct.pack('<f', x))[0] for x in inp]
        dry_err = max(dry_err, max(abs(a) for a in ol), max(abs(a) for a in orr))   # triggerstand: stil
    elif k > grace_blocks + 4:
        wet_peak = max(wet_peak, max(abs(x) for x in ol)); per_block.append(n)
fault = lambda: struct.unpack('<I', u.mem_read(SYM['nc_octave_fault'], 4))[0]
budget = 4e6 * N / FS * 0.65 * ipt[0]
res.update(grace_dry_max_err=dry_err, wet_peak=wet_peak, fault_after_run=fault(),
           output_mode=u.mem_read(OMODE, 1)[0],
           instr_per_block_avg=sum(per_block) / len(per_block), instr_per_block_max=max(per_block),
           budget_65pct_instr=budget,
           max_load_pct=100 * max(per_block) / (4e6 * N / FS * ipt[0]))

# zwaarste stand met de pedalknoppen: FREQ/RESO/DRIVE max, HARM vrij, AUDIO+909, ruis
import random; random.seed(1)
u.mem_write(PAR, struct.pack('<11f', 0.5, 0.5, 0.5, 1.0, 1.0, 1.0, 0.5, 1.0, 0.0, 1.0, 0.8))
heavy = []; ipt[0] = 1e9          # klok stilzetten: alleen instructies tellen, bewaking grijpt niet in
for k in range(60):
    ol, orr, n = block(k, [0.3 * (random.random() * 2 - 1) if k % 15 < 2 else 0.05 * (random.random() * 2 - 1) for _ in range(N)])
    heavy.append(n)
res['heavy_max_load_pct'] = 100 * max(heavy[5:]) / (4e6 * N / FS * INSTR_PER_US / 4.0)
res['heavy_avg_load_pct'] = 100 * sum(heavy[5:]) / len(heavy[5:]) / (4e6 * N / FS * INSTR_PER_US / 4.0)
res['fault_after_heavy'] = fault(); ipt[0] = INSTR_PER_US / 4.0

# 3. subtype != 8 komt nooit in onze verwerking: alleen via de native proloog (niet emuleerbaar)
# 4. CPU-bewaking: trage klok (x4) -> latch bypass, scherm toont CPU LIMIT
u.mem_write(PAR, struct.pack('<11f', 0.5, 0.5, 0.5, 0.333, 0.75, 0.35, 0, 0.75, 0, 0, 0.8))   # 909-stand
ipt[0] = INSTR_PER_US / 4.0 / 4.0
for k in range(6): ol, orr, n = block(k, [0.3] * N)
res['fault_with_slow_clock'] = fault()
ol, orr, n = block(0, [0.25] * N)
res['bypass_after_fault'] = max(abs(x) for x in ol) == 0.0 and n < 3000   # triggerstand: stil

# 5. UI
def cstr(a):
    b = bytes(u.mem_read(a, 32)); return b[:b.index(0)].decode()
res['name'] = cstr(SYM['nc_octave_name'])
res['labels'] = [cstr(call(SYM['nc_octave_label'], (0, 0, i, 0))[0]) for i in range(12)]
BUF = STR + 0x100
def fmt(i, v):
    rc = call(SYM['nc_octave_format'], (0, 0, i, BUF, 16, 0), (v,))[0]; return cstr(BUF), rc
u.mem_write(SYM['nc_octave_fault'], b'\0\0\0\0')
res['format'] = {f'{i}:{v}': fmt(i, v)[0] for i, v in [(0, .5), (6, 0.), (6, .5), (6, .25), (7, .5), (7, 1.), (7, 0.), (8, 0.), (8, 1.), (9, 1 / 3), (9, 1.), (10, .8)]}
res['format_index11_rc'] = fmt(11, .5)[1]
res['format_nan'] = fmt(3, float('nan'))[0]
res['defaults'] = [round(x, 3) for x in struct.unpack('<11f', u.mem_read(SYM['nc_octave_defaults_values'], 44))]
print(json.dumps(res, indent=1))

ok = (res['init_bad_count'] != 0 and res['init_bad_rate'] != 0 and res['init_rc'] == 0 and res['state_in_ring']
      and res['state_bytes'] < 65536 and res['grace_dry_max_err'] == 0 and res['wet_peak'] > 0.05
      and res['fault_after_run'] == 0 and res['fault_with_slow_clock'] in (1, 2) and res['bypass_after_fault']
      and res['name'] == 'HARDCORE' and res['labels'][10] == 'LEVEL' and res['labels'][11] == ''
      and res['format_index11_rc'] != 0 and res['format']['7:0.5'] == '0' and res['format']['8:1.0'] == 'x16')
print('EMULATIE', 'OK' if ok else 'MISLUKT'); sys.exit(0 if ok else 1)
