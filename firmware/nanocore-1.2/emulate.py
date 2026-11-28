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
import json, math, struct, sys
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
import xthead
XT = xthead.install(u, [(s['load_address'], s['data']) for s in extract_fit(IMG)['segments']])  # T-Head-instructies
u.emu_start(0x40000000, 0x40006428, count=200000)          # reset-shim: payload kopiëren
assert u.reg_read(r.UC_RISCV_REG_PC) == 0x40006428

icount = [0]; ipt = [INSTR_PER_US / 4.0]                    # instructies per tick van 4 MHz
toff = [0]                                                  # extra tijd (ticks) voor UI-tests
def on_code(uc, addr, size, _):
    icount[0] += 1
    if addr == TICKS:                                       # native timer -> instructieklok
        t = int(icount[0] / ipt[0]) + toff[0]; uc.reg_write(r.UC_RISCV_REG_A0, t & 0xffffffff)
        uc.reg_write(r.UC_RISCV_REG_A1, t >> 32); uc.reg_write(r.UC_RISCV_REG_PC, uc.reg_read(r.UC_RISCV_REG_RA))
u.hook_add(UC_HOOK_CODE, on_code)
from unicorn import UC_HOOK_MEM_UNMAPPED
def on_unmapped(uc, access, addr, size, value, _):
    print('niet-gemapt geheugen: %#x (pc %#x, ra %#x)' % (addr, uc.reg_read(r.UC_RISCV_REG_PC), uc.reg_read(r.UC_RISCV_REG_RA)))
    return False
u.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

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

# 4b. MIDI-haak (USB+BLE): native entry 0x4002db26 springt naar nc_midi_gate
HOOK, HELPER, FX2SUB = 0x4002db26, 0x4000c3a4, 0x400b0ec1
u.mem_write(SYM['nc_octave_fault'], b'\0\0\0\0'); ipt[0] = INSTR_PER_US / 4.0
def midi(port, st, d1, d2, has2=1):
    u.reg_write(r.UC_RISCV_REG_T0, 0)
    sp = STR + 0x7000; u.reg_write(r.UC_RISCV_REG_SP, sp); u.reg_write(r.UC_RISCV_REG_RA, STOP)
    for i, v in enumerate((port, st, d1, d2, has2)): u.reg_write(getattr(r, f'UC_RISCV_REG_A{i}'), v)
    try: u.emu_start(HOOK, STOP, count=20000)
    except Exception: pass
    pc = u.reg_read(r.UC_RISCV_REG_PC)
    return 'hardcore' if pc == STOP and u.reg_read(r.UC_RISCV_REG_SP) == sp else ('native' if pc == HELPER and u.reg_read(r.UC_RISCV_REG_T0) == 0x4002db2a and u.reg_read(r.UC_RISCV_REG_A1) == st and u.reg_read(r.UC_RISCV_REG_A3) == d2 and u.reg_read(r.UC_RISCV_REG_SP) == sp else hex(pc))
def stop_at_helper(uc, addr, size, _):
    if addr == HELPER: uc.emu_stop()
hh = u.hook_add(UC_HOOK_CODE, stop_at_helper, begin=HELPER, end=HELPER)
u.mem_write(FX2SUB, b'\x08')
res['midi_cc_active'] = midi(0, 0xB0, 24, 100)
res['midi_note_active'] = midi(1, 0x90, 36, 127)
res['midi_pc_active'] = midi(1, 0xC0, 5, 0, 0)
res['midi_sysex_like'] = midi(1, 0xF2, 1, 2)
head = lambda: struct.unpack('<I', u.mem_read(SYM['nc_hc_midi_head'], 4))[0]
tail = lambda: struct.unpack('<I', u.mem_read(SYM['nc_hc_midi_tail'], 4))[0]
res['midi_queued'] = head() - tail()
u.mem_write(PAR, struct.pack('<11f', 0.5, 0.5, 0.5, 0.333, 0.75, 0.35, 0, 0.75, 0, 0, 0.8))
pk = 0.
for k in range(12): ol, orr, n = block(k, [0.] * N); pk = max(pk, max(abs(x) for x in ol))
res['midi_drained'] = head() == tail()
res['midi_note_kick_peak'] = pk                               # stille input: alleen de MIDI-noot laat de 909 slaan
u.mem_write(FX2SUB, b'\x03')
res['midi_cc_inactive'] = midi(0, 0xB0, 24, 100)
u.hook_del(hh)

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

# 6. Fase A: de echte native brokverwerker 0x30043800 (ITCM) met een gesimuleerd DMA-blok
#    (64 frames interleaved stereo int32, sample in de bovenste 24 bits). Hij roept via de
#    twee haken nc_hc_amp en nc_hc_dispatch aan; daarna de native uitgangstrap naar DMA-uit.
CHUNK, DIN, DOUT = 0x30043800, 0x30160000, 0x30161000
AMP_ON, AMP_SEL, AMP_TAB = 0x30104879, 0x30104878, 0x30104810
FA = SYM['nc_hc_fase_a']
u.mem_write(SYM['nc_octave_fault'], b'\0\0\0\0'); ipt[0] = INSTR_PER_US / 4.0
u.mem_write(0x3008ab3c, struct.pack('<f', 1.0))            # ingangsgain
u.mem_write(0x3008ab48, struct.pack('<f', 1.0)); u.mem_write(0x3008ab44, struct.pack('<f', 1.0))
u.mem_write(0x3008ab4d, b'\x64')                           # master 100
u.mem_write(0x30104128, struct.pack('<I', 48000))
fa = lambda i: struct.unpack('<I', u.mem_read(FA + 4 * i, 4))[0]
def chunk(samples):
    words = []
    for x in samples:
        v = max(-1.0, min(1.0, x)); w = int(v * 8388607) << 8
        words += [w & 0xffffffff, 0]                       # alleen links is de pedal-ingang
    u.mem_write(DIN, struct.pack('<128I', *words)); u.mem_write(DOUT, bytes(512))
    _, n = call(CHUNK, (DIN, DOUT, 0, 0, 0, 0))
    calls = struct.unpack('<I', u.mem_read(CALLS, 4))[0]; u.mem_write(CALLS, struct.pack('<I', calls + 1))
    s = struct.unpack('<I', u.mem_read(SUM, 4))[0]
    u.mem_write(SUM, struct.pack('<I', (s + int(n / ipt[0]) + 400) & 0xffffffff))
    out = struct.unpack('<128i', u.mem_read(DOUT, 512))
    return [o / 2147483648.0 for o in out[0::2]], [o / 2147483648.0 for o in out[1::2]], n

grace = int(3.0 * FS / N)
t = 0; native_err = 0.; native_n = []; wet = 0.; own_n = []; amp_skips0 = None
# native uitgangstrap: x * gain(0x3008ab44) * 0.5 (0x400889ac) * master/100 -> lege keten = 0.5 * x
OUT_G = 0.5
for k in range(grace + 200):
    if k == grace:                                         # na de grace: een preset met versterker
        u.mem_write(AMP_ON, b'\1'); u.mem_write(AMP_SEL, b'\0'); u.mem_write(AMP_TAB, b'\1')
        u.mem_write(0x3010487c, struct.pack('<I', 0x30180000))   # versterker-state (native setter schrijft erin)
        amp_skips0 = fa(4)
    inp = [0.6 if (t + i) % (FS // 4) < 240 else 0.02 * math.sin(2 * math.pi * 220 * (t + i) / FS) for i in range(N)]; t += N
    ol, orr, n = chunk(inp)
    if k < grace:
        native_n.append(n)
        if k > 4: native_err = max(native_err, max(abs(a - OUT_G * b) for a, b in zip(ol, inp)))   # lege stock-keten: doorgeven
    elif k > grace + 4:
        wet = max(wet, max(abs(x) for x in ol)); own_n.append(n)
res['xthead_executed'] = dict(XT)
res['fase_a'] = dict(blocks=fa(0), native_blocks=fa(1), own_blocks=fa(2), reason=fa(3), amp_skipped=fa(4) - amp_skips0,
                     grace_passthrough_max_err=native_err, wet_peak=wet,
                     native_chunk_instr_avg=sum(native_n) / len(native_n),
                     own_chunk_instr_avg=sum(own_n) / len(own_n), own_chunk_instr_max=max(own_n),
                     own_load_pct_max=100 * max(own_n) / (4e6 * N / FS * ipt[0]), fault=fault())
# één los zwaar blok (bv. een onderbreking door scherm of BLE) mag geen CPU LIMIT geven
ipt[0] = INSTR_PER_US / 4.0 / 2.0
chunk([0.3] * N)
ipt[0] = INSTR_PER_US / 4.0
for k in range(4): chunk([0.3] * N)
LD = SYM['nc_hc_load']
res['fase_a'].update(single_spike_fault=fault(), load_avg_pm=struct.unpack('<I', u.mem_read(LD, 4))[0],
                     load_peak_pm=struct.unpack('<I', u.mem_read(LD + 4, 4))[0])
# CPU-fout -> terug naar de native keten (latch), geen eigen DSP meer
ipt[0] = INSTR_PER_US / 4.0 / 4.0
for k in range(6): chunk([0.3] * N)
own_before = fa(2); ipt[0] = INSTR_PER_US / 4.0
ol, orr, n = chunk([0.25] * N)
res['fase_a'].update(fault_load_pm=struct.unpack('<I', u.mem_read(LD + 8, 4))[0], fault_slow_clock=fault(), native_after_fault=fa(2) == own_before and fa(3) == 2,
                     native_after_fault_passthrough=abs(ol[10] - OUT_G * 0.25) < 1e-4)
u.mem_write(SYM['nc_octave_fault'], b'\0\0\0\0')

# 7. Fase B: scherm en knoppen. nc_hc_ui_tick (in plaats van lv_timer_handler) en
#    nc_hc_enc_dispatch; aicfb, pinnen en de native LVGL/knop-functies onderschept.
import subprocess
ALL = {}
for line in subprocess.run(['llvm-nm', '-S', str(ROOT / 'build/fw12/device.elf')], capture_output=True, text=True).stdout.splitlines():
    p_ = line.split()
    if len(p_) == 4: ALL[p_[3]] = (int(p_[0], 16), int(p_[1], 16))
FBMEM, HANDLE = 0x301a0000, 0x30190000
# zoals de pedal het waarschijnlijk meldt: portret, bpp-veld 0, ioctl != 0, smem_len = 1 frame
fbinfo = {'format': 14, 'bpp': 0, 'w': 240, 'h': 320, 'rc': 0xffffffff, 'frames': 1}
pans, native = [], {'lvgl': 0, 'enc': 0}
pins = {39: 1, 75: 1}                                     # 1 = losgelaten (actief laag)
u.mem_write(0x30117f78, struct.pack('<I', 39)); u.mem_write(0x30117f7c, struct.pack('<I', 75))
u.mem_write(0x30118f3c, struct.pack('<I', HANDLE))
def ret(uc, v=None):
    if v is not None: uc.reg_write(r.UC_RISCV_REG_A0, v & 0xffffffff)
    uc.reg_write(r.UC_RISCV_REG_PC, uc.reg_read(r.UC_RISCV_REG_RA))
def periph(uc, addr, size, _):
    if addr == 0x4005cb68:                                # aicfb ioctl
        cmd, arg = uc.reg_read(r.UC_RISCV_REG_A1), uc.reg_read(r.UC_RISCV_REG_A2)
        if cmd == 0x4662:
            B = {14: 2, 8: 3, 0: 4, 4: 4}.get(fbinfo['format'], 4); stride = fbinfo['w'] * B
            uc.mem_write(arg, struct.pack('<5III', fbinfo['format'], fbinfo['bpp'], stride, fbinfo['w'], fbinfo['h'], FBMEM, fbinfo['frames'] * stride * fbinfo['h']))
            ret(uc, fbinfo['rc']); return
        elif cmd == 0x4643: pans.append(struct.unpack('<i', uc.mem_read(arg, 4))[0])
        ret(uc, 0)
    elif addr == 0x4004f4b8: native['lvgl'] += 1; ret(uc, 7)
    elif addr == 0x40017f72: native['enc'] += 1; ret(uc)
    elif addr == 0x40045290: ret(uc, pins.get(uc.reg_read(r.UC_RISCV_REG_A0), 1))
hp = [u.hook_add(UC_HOOK_CODE, periph, begin=a, end=a) for a in (0x4005cb68, 0x4004f4b8, 0x40017f72, 0x40045290)]
UNIT = 4096                                               # 1 UI-ms = 4096 ticks
def ui_at(ms):
    toff[0] = ms * UNIT - int(icount[0] / ipt[0])
    v, n = call(SYM['nc_hc_ui_tick']); return v, n
scr = lambda i: struct.unpack('<I', u.mem_read(SYM['nc_hc_screen'] + 4 * i, 4))[0]
uih = lambda: struct.unpack('<I', u.mem_read(SYM['nc_hc_ui_head'], 4))[0]
fb = {}
v0, _ = ui_at(10); v1, _ = ui_at(1500); v2, _ = ui_at(3000)      # grace (start bij 10 ms)
call(SYM['nc_hc_enc_dispatch'])
gr = dict(returns=[v0, v1, v2], native_lvgl=native['lvgl'], native_enc=native['enc'], state=scr(0))
_, n_take = ui_at(3020)                                   # overname + eerste bootframe
_, n_boot = ui_at(3020 + 2000)                            # bootframe op t = 2000
W_, H_ = fbinfo['w'], fbinfo['h']
frame = bytes(u.mem_read(FBMEM + W_ * 2 * H_ * pans[-1], W_ * 2 * H_))
subprocess.run([str(ROOT / 'build/vecshot'), str(ROOT / 'build'), 'p', '2000'], check=True)
ref = (ROOT / 'build/boot_02000.ppm').read_bytes()[-W_ * H_ * 3:]
same = 0
for i in range(W_ * H_):
    v = frame[2 * i] | frame[2 * i + 1] << 8
    rgb = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    same += all(abs(a - b) <= 9 for a, b in zip(rgb, ref[3 * i:3 * i + 3]))
_, n_home = ui_at(3020 + 3700)                            # homescherm
f_before = scr(2); _, n_idle = ui_at(3020 + 3730); idle_drew = scr(2) - f_before   # niets veranderd: niet tekenen
head0 = uih()
u.mem_write(0x30117f76, struct.pack('<h', 2)); ui_at(3020 + 3760)      # enc2 = B: +2
head1 = uih()
pins[39] = 0; ui_at(3020 + 3780); ui_at(3020 + 3800); ui_at(3020 + 3820)
pins[39] = 1; ui_at(3020 + 3840); ui_at(3020 + 3860)                     # A kort: blok open
opened = scr(8)
call(SYM['nc_hc_enc_dispatch'])
res['fase_b'] = dict(grace=gr, state=scr(0), reason=scr(1), frames=scr(2), info=[scr(3), scr(4), scr(5), scr(6)],
                     pans=pans[:6], boot_frame_match_pct=round(100 * same / (W_ * H_), 2),
                     instr_takeover=n_take, instr_boot_frame=n_boot, instr_home_frame=n_home,
                     ms_boot_frame=round(n_boot / INSTR_PER_US / 1000, 1), ms_home_frame=round(n_home / INSTR_PER_US / 1000, 1),
                     cc_queued_by_B=head1 - head0, a_press_opens_block=opened == 1,
                     native_enc_after_takeover=native['enc'] - gr['native_enc'],
                     native_lvgl_after_takeover=native['lvgl'] - gr['native_lvgl'],
                     idle_tick_drew=idle_drew, instr_idle_tick=n_idle)
# 24-bit RGB888 wordt nu ook overgenomen (zoals de native tabel het toelaat)
sa, ss = ALL['S']
u.mem_write(sa, bytes(ss)); fbinfo.update(format=8, w=320, h=240)
nl = native['lvgl']; ui_at(100); ui_at(3200); ui_at(3300)
f24 = bytes(u.mem_read(FBMEM + 320 * 3 * 240 * pans[-1], 320 * 3 * 240))
res['fase_b'].update(rgb888_state=scr(0), rgb888_lit_bytes=sum(1 for x in f24 if x))
# onzinnige resolutie: geen overname, alles native, reden 4
u.mem_write(sa, bytes(ss)); fbinfo.update(format=14, w=0, h=0)
nl = native['lvgl']; ui_at(100); ui_at(3200); v_bad, _ = ui_at(3300)
res['fase_b'].update(bad_format_state=scr(0), bad_format_reason=scr(1), bad_format_native=native['lvgl'] - nl, bad_format_return=v_bad)
# hoorbare diagnose: na 7 s eigen keten zoveel piepjes als de reden (hier 4)
DG = SYM['nc_hc_diag']
call(SYM['nc_hc_ui_cc'], (19, 127))                     # keten op mute: alleen de piepjes zijn hoorbaar
for k in range(40): chunk([0.0] * N)
u.mem_write(DG, struct.pack('<3I', 0, int(7 * FS) - 2 * N, 0))
energy = []
for k in range(int(1.4 * FS / N)):
    ol, orr, n = chunk([0.0] * N)
    energy.append(max(abs(x) for x in ol))
beeps = sum(1 for a_, b_ in zip(energy, energy[1:]) if a_ < 0.05 <= b_)
call(SYM['nc_hc_ui_cc'], (19, 0))
res['fase_b'].update(diag_code=struct.unpack('<I', u.mem_read(DG, 4))[0], diag_beeps_heard=beeps, diag_peak=round(max(energy), 3))
# 8. Fase D1: SPL alleen-lezen uitlezen. Startsein CC 84 'S' 'P' 'L' (USB = poort 0) ->
#    nc_hc_ui_tick leest de partitie blok voor blok en verstuurt via de ECHTE native
#    verzender 0x4001854c (7-bit inpakken + framing). Onderschept: de USB-schrijffunctie
#    en de flash-leesfuncties (die geven een bekend patroon).
import hashlib
u.mem_write(sa, bytes(ss)); fbinfo.update(format=14, w=240, h=320)
ui_at(100); ui_at(3200)
STUB, PART, SPLN = 0x30170000, 0x30171000, 5000
u.mem_write(STUB, bytes.fromhex('67800000'))               # ret
u.mem_write(0x30111680, struct.pack('<I', STUB))           # schrijffunctie van poort 0 (USB)
u.mem_write(PART + 0x38, struct.pack('<I', SPLN))
src = bytes(hashlib.sha256(i.to_bytes(4, 'little')).digest()[0] for i in range(SPLN))
sent, finds = [], []
def d1(uc, addr, size, _):
    if addr == STUB:
        sent.append(bytes(uc.mem_read(uc.reg_read(r.UC_RISCV_REG_A0), uc.reg_read(r.UC_RISCV_REG_A1))))
    elif addr == 0x4000e65e:
        finds.append(bytes(uc.mem_read(uc.reg_read(r.UC_RISCV_REG_A0), 16)).split(b'\0')[0].decode())
        ret(uc, PART)
    elif addr == 0x4000e6ac:
        off, buf, n = (uc.reg_read(getattr(r, 'UC_RISCV_REG_A%d' % k)) for k in (1, 2, 3))
        uc.mem_write(buf, src[off:off + n]); ret(uc, n)
hd = [u.hook_add(UC_HOOK_CODE, d1, begin=a, end=a) for a in (STUB, 0x4000e65e, 0x4000e6ac)]
mi = lambda port, v: call(SYM['nc_midi_in'], (port, 0xB0, 84, v, 1))[0]
REQ = SYM['nc_hc_spl_req']; req = lambda: struct.unpack('<I', u.mem_read(REQ, 4))[0]
r0 = req(); ble = [mi(1, v) for v in (0x53, 0x50, 0x4c)]; ble_req = req() - r0       # BLE: geen dump
usb = [mi(0, v) for v in (0x53, 0x50, 0x4c)]; usb_req = req() - r0
SP = SYM['nc_hc_spl']; spl = lambda i: struct.unpack('<I', u.mem_read(SP + 4 * i, 4))[0]
tms = 3300
for k in range(40):
    tms += 40; ui_at(tms)
    if spl(0) == 2: break
spl_sent = list(sent); sent.clear()
pre = [mi(0, v) for v in (0x50, 0x52, 0x45)]               # P R E = presets
for k in range(40):
    tms += 40; ui_at(tms)
    if spl(0) == 2 and len(sent) >= 7: break
def unpack7(b):
    o = []; i = 0
    while i < len(b):
        h = b[i]; i += 1
        for k in range(7):
            if i >= len(b): break
            o.append(((h >> k) & 1) << 7 | b[i]); i += 1
    return bytes(o)
pre_tag = unpack7(sent[0][5:-1])[:4] if sent else b''
pre_name = finds[-1] if finds else ''
sent = spl_sent
for h in hd: u.hook_del(h)
(ROOT / 'build/spl_msgs.json').write_text(json.dumps([m.hex() for m in sent]))
js = subprocess.run(['node', '-e', """
import('%s').then(async m => {
  const msgs = JSON.parse(require('fs').readFileSync('%s', 'utf8')).map(h => Uint8Array.from(Buffer.from(h, 'hex')));
  const c = new m.Collector(); let done = false;
  for (const x of msgs) { const ch = m.parseChunk(x); if (ch) done = c.add(ch); }
  console.log(JSON.stringify({done, sha: require('crypto').createHash('sha256').update(c.buf).digest('hex')}));
})""" % (ROOT / 'docs/install/spl.js', ROOT / 'build/spl_msgs.json')], capture_output=True, text=True)
jr = json.loads(js.stdout or '{}')
import zlib
res['fase_d1'] = dict(find_name=finds[:1], ble_trigger=ble_req, usb_trigger=usb_req, midi_consumed=ble + usb,
                      state=spl(0), chunks=spl(3), sent_messages=len(sent),
                      sysex_frame_ok=all(m[:5] == bytes([0xf0, 0x7d, 0x4e, 0x43, 0x5c]) and m[-1] == 0xf7 for m in sent),
                      crc_total_ok=spl(4) == zlib.crc32(src), js_complete=jr.get('done'),
                      js_sha_matches_source=jr.get('sha') == hashlib.sha256(src).hexdigest(), js_error=js.stderr[-300:],
                      presets_name=pre_name, presets_tag=pre_tag.decode('latin1'))
for h in hp: u.hook_del(h)
print(json.dumps(res, indent=1))

ok = (res['init_bad_count'] != 0 and res['init_bad_rate'] != 0 and res['init_rc'] == 0 and res['state_in_ring']
      and res['state_bytes'] < 65536 and res['grace_dry_max_err'] == 0 and res['wet_peak'] > 0.05
      and res['fault_after_run'] == 0 and res['fault_with_slow_clock'] in (1, 2) and res['bypass_after_fault']
      and res['name'] == 'HARDCORE' and res['labels'][10] == 'LEVEL' and res['labels'][11] == ''
      and res['format_index11_rc'] != 0 and res['midi_cc_active'] == 'hardcore' and res['midi_note_active'] == 'hardcore'
      and res['midi_pc_active'] == 'native' and res['midi_sysex_like'] == 'native' and res['midi_cc_inactive'] == 'native'
      and res['midi_queued'] == 2 and res['midi_drained'] and res['midi_note_kick_peak'] > 0.05 and res['format']['7:0.5'] == '0' and res['format']['8:1.0'] == 'x16'
      and res['fase_a']['native_blocks'] >= grace and res['fase_a']['own_blocks'] >= 190
      and res['fase_a']['grace_passthrough_max_err'] < 1e-4 and res['fase_a']['wet_peak'] > 0.05
      and res['fase_a']['amp_skipped'] >= 190 and res['fase_a']['fault'] == 0
      and res['fase_a']['fault_slow_clock'] in (1, 2) and res['fase_a']['native_after_fault']
      and res['fase_a']['single_spike_fault'] == 0 and res['fase_a']['fault_load_pm'] > 950
      and res['fase_a']['native_after_fault_passthrough']
      and res['fase_b']['grace']['returns'] == [7, 7, 7] and res['fase_b']['grace']['native_enc'] == 1
      and res['fase_b']['state'] == 1 and res['fase_b']['pans'][:4] == [1, 0, 1, 0]
      and res['fase_b']['boot_frame_match_pct'] >= 99.0 and res['fase_b']['cc_queued_by_B'] == 1
      and res['fase_b']['a_press_opens_block'] and res['fase_b']['native_enc_after_takeover'] == 0
      and res['fase_b']['native_lvgl_after_takeover'] == 0 and res['fase_b']['bad_format_state'] == 2
      and res['fase_b']['bad_format_reason'] == 4 and res['fase_b']['bad_format_return'] == 7
      and res['fase_b']['idle_tick_drew'] == 0 and res['fase_b']['rgb888_state'] == 1 and res['fase_b']['rgb888_lit_bytes'] > 1000
      and res['fase_b']['diag_code'] == 4 and res['fase_b']['diag_beeps_heard'] == 4
      and res['fase_d1']['find_name'] == ['spl'] and res['fase_d1']['ble_trigger'] == 0 and res['fase_d1']['usb_trigger'] == 1
      and res['fase_d1']['state'] == 2 and res['fase_d1']['chunks'] == 7 and res['fase_d1']['sysex_frame_ok']
      and res['fase_d1']['crc_total_ok'] and res['fase_d1']['js_complete'] and res['fase_d1']['js_sha_matches_source']
      and res['fase_d1']['presets_name'] == 'presets' and res['fase_d1']['presets_tag'] == 'NHP1')
print('EMULATIE', 'OK' if ok else 'MISLUKT'); sys.exit(0 if ok else 1)
