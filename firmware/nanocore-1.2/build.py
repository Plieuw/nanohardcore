#!/usr/bin/env python3
"""Bouwt de nanohardcore-patch voor NanoCore-firmware 1.2 (geen USB, schrijft niets naar een pedal).

  python3 firmware/nanocore-1.2/build.py --backup PAD/nanocore-1.2-stock-backup.bin \
      --re-nanocore PAD/re-nanocore [--manifest docs/install/hardcore-1.2.json]

Stappen: clang/ld.lld bouwen de payload op 0x400cb880; de haakplannen en de packager
van re-nanocore (MIT) leggen hem in een kopie van de backup (zelfde haken, ring-
vergroting, reset-shim en FIT-hashes als hun op hardware geteste 1.2-Octaver); hun
opstartcontrole voert de reset-shim uit in Unicorn; daarna exporteert dit script de
web-delta (alleen gewijzigde bytes + onze payload, geen vendorimage).
De backup en de volledige images blijven lokaal in build/fw12 (niet committen).
"""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DSP = ['dsp/bd909.c', 'dsp/sherman.c', 'dsp/mackie.c', 'dsp/mkcirc.c', 'dsp/chain.c']
CODE_BASE = 0x400cb880
FLAGS = ['--target=riscv32-unknown-elf', '-march=rv32imafdc', '-mabi=ilp32d', '-O2',
         '-ffreestanding', '-fno-builtin', '-nostdlib', '-msmall-data-limit=0', '-fno-pic',
         '-mno-relax', '-fno-common', '-Wall', '-Wextra', '-Werror=implicit-function-declaration']
sha = lambda b: hashlib.sha256(b).hexdigest()


def run(*cmd):
    return subprocess.check_output(cmd, text=True)


def build_elf(out):
    objs = []
    for src in DSP + ['firmware/nanocore-1.2/adapter.c', 'firmware/nanocore-1.2/ui.c',
                      'firmware/nanocore-1.2/screen.c', 'ui/vec.c', 'ui/hcui.c']:
        o = out / (Path(src).stem + '.o'); objs.append(str(o))
        run('clang', *FLAGS, '-c', str(ROOT / src), '-o', str(o))
    for asm in ('device_ui_120.S', 'midi_gate.S'):
        o = out / (Path(asm).stem + '.o'); objs.append(str(o))
        run('clang', *FLAGS, '-c', str(HERE / asm), '-o', str(o))
    ld = out / 'device.ld'
    ld.write_text('ENTRY(nc_octave_dispatch)\nSECTIONS { . = %#x; .text : { *(.text*) } '
                  '.rodata : { *(.rodata*) *(.srodata*) } .data : { *(.data*) *(.sdata*) } '
                  '.bss : { *(.bss*) *(.sbss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.note*) '
                  '*(.eh_frame*) *(.riscv.attributes) } }\n' % CODE_BASE)
    elf = out / 'device.elf'
    run('ld.lld', '--no-relax', '-T', str(ld), '-Map', str(out / 'device.map'), *objs, '-o', str(elf))
    undefined = run('llvm-nm', '-u', str(elf)).strip()
    if undefined: raise SystemExit('Onopgeloste symbolen:\n' + undefined)
    symbols = {}
    for line in run('llvm-nm', '-n', str(elf)).splitlines():
        p = line.split()
        if len(p) == 3: symbols[p[2]] = int(p[0], 16)
    for s in ('.init_array', '.fini_array', '.ctors'):
        if s in run('llvm-objdump', '-h', str(elf)): raise SystemExit('Constructors niet toegestaan')
    (out / 'device.asm').write_text(run('llvm-objdump', '-d', '--mattr=+c,+f,+d', str(elf)))
    return elf.read_bytes(), symbols


# Eigen haak (niet in de geteste Octaver-set): native MIDI-berichtfunctie, beide poorten.
MIDI_HOOK = (0x4002db26, 'efe2fd87')            # jal t0,0x4000c3a4 (register-save-helper)

def midi_hook(stock, symbols):
    from patch_image import extract_fit
    from octave_hooks import jal
    seg = next(s for s in extract_fit(stock)['segments'] if s['load_address'] == 0x40000000)
    address, guard = MIDI_HOOK
    offset = seg['offset'] + address - 0x40000000
    if stock[offset:offset + 4] != bytes.fromhex(guard): raise SystemExit('Onverwachte bytes op de MIDI-haak')
    return [dict(address=hex(address), offset=offset, expected_hex=guard,
                 replacement_hex=jal(address, symbols['nc_midi_gate']).hex(),
                 description='MIDI (USB+BLE): CC/noten naar HARDCORE als dat het FX2-effect is')]


# Fase A (docs/eigen-firmware.md): de native effectketen en de versterker/NAM-trap
# vervangen. Beide zitten in de ITCM-brokverwerker 0x30043800 (seg0), te ver voor een
# `jal` naar de payload: daarom auipc+jalr met ra als hulpregister (ra wordt door de
# originele aanroep toch overschreven).
FASE_A_DISPATCH = (0x3004395e, '17050c001305a5f5efe08fc1')   # auipc a0 / addi a0,-0xa6 / jal 0x30041d7e
FASE_A_AMP = (0x3004392a, '97000310e780a0ac')                # auipc ra / jalr -0x536(ra) -> 0x400733f4


def far_call(pc, target):
    """auipc ra,hi ; jalr ra,lo(ra): 8 bytes, bereik +-2 GiB."""
    delta = (target - pc) & 0xffffffff
    lo = delta & 0xfff
    if lo >= 0x800: lo -= 0x1000
    hi = ((delta - lo) >> 12) & 0xfffff
    auipc = (hi << 12) | (1 << 7) | 0x17
    jalr = ((lo & 0xfff) << 20) | (1 << 15) | (0 << 12) | (1 << 7) | 0x67
    return auipc.to_bytes(4, 'little') + jalr.to_bytes(4, 'little')


NOP = (0x13).to_bytes(4, 'little')                              # addi x0,x0,0


def fase_a_hooks(stock, symbols):
    from patch_image import extract_fit
    seg = next(s for s in extract_fit(stock)['segments'] if s['load_address'] == 0x30040000)
    out = []
    for (address, guard), repl, what in (
            (FASE_A_DISPATCH, far_call(FASE_A_DISPATCH[0], symbols['nc_hc_dispatch']) + NOP,
             'Fase A: ketendispatcher 0x30041d7e -> nc_hc_dispatch (native tijdens grace/fout)'),
            (FASE_A_AMP, far_call(FASE_A_AMP[0], symbols['nc_hc_amp']),
             'Fase A: versterker/NAM 0x400733f4 -> nc_hc_amp (native tijdens grace/fout)')):
        offset = seg['offset'] + address - 0x30040000
        if stock[offset:offset + len(guard) // 2] != bytes.fromhex(guard):
            raise SystemExit('Onverwachte bytes op fase-A-haak %#x' % address)
        assert len(repl) == len(guard) // 2
        out.append(dict(address=hex(address), offset=offset, expected_hex=guard,
                        replacement_hex=repl.hex(), description=what))
    return out


# Fase B: eigen scherm en knoppen. Beide haken zijn een `jal ra` in seg3 (binnen het
# bereik van +-1 MiB tot de payload): alleen het doel verandert.
FASE_B_UI = (0x40017ab2, 'ef707320')          # jal 0x4004f4b8 (lv_timer_handler) in de LVGL-taak
FASE_B_ENC = (0x40018004, 'eff0fff6')         # jal 0x40017f72 (knoppendispatcher) in enc_poll


def jal_ra(pc, target):
    off = target - pc
    if not -(1 << 20) <= off < (1 << 20) or off & 1: raise SystemExit('jal buiten bereik: %#x -> %#x' % (pc, target))
    o = off & 0x1fffff
    insn = (((o >> 20) & 1) << 31) | (((o >> 1) & 0x3ff) << 21) | (((o >> 11) & 1) << 20) | (((o >> 12) & 0xff) << 12) | (1 << 7) | 0x6f
    return insn.to_bytes(4, 'little')


def fase_b_hooks(stock, symbols):
    from patch_image import extract_fit
    seg = next(s for s in extract_fit(stock)['segments'] if s['load_address'] == 0x40000000)
    out = []
    for (address, guard), sym, what in (
            (FASE_B_UI, 'nc_hc_ui_tick', 'Fase B: lv_timer_handler -> nc_hc_ui_tick (native tijdens grace/fout)'),
            (FASE_B_ENC, 'nc_hc_enc_dispatch', 'Fase B: knoppendispatcher -> nc_hc_enc_dispatch (native zolang de UI niet over is)')):
        offset = seg['offset'] + address - 0x40000000
        if stock[offset:offset + 4] != bytes.fromhex(guard):
            raise SystemExit('Onverwachte bytes op fase-B-haak %#x' % address)
        out.append(dict(address=hex(address), offset=offset, expected_hex=guard,
                        replacement_hex=jal_ra(address, symbols[sym]).hex(), description=what))
    return out


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--backup', required=True, type=Path)
    a.add_argument('--re-nanocore', required=True, type=Path)
    a.add_argument('--out', type=Path, default=ROOT / 'build' / 'fw12')
    a.add_argument('--manifest', type=Path)
    args = a.parse_args()
    sys.path.insert(0, str(args.re_nanocore / 'tools' / 'custom_fx'))
    from firmware_identity import identify_firmware
    from patch_image import extract_fit
    import octave_120, octave_ui_120, package_octave_120, check_octave_120_boot, export_octave_web_patch

    stock = args.backup.read_bytes()
    if identify_firmware(stock) != '1.2': raise SystemExit('Backup is geen firmware 1.2')
    args.out.mkdir(parents=True, exist_ok=True)
    elf, symbols = build_elf(args.out)

    hooks = octave_120.patches(stock, symbols) + octave_ui_120.patches(stock, symbols) + midi_hook(stock, symbols) + fase_a_hooks(stock, symbols) + fase_b_hooks(stock, symbols)
    plan = {'schema_version': 1, 'firmware_version': '1.2', 'elf_sha256': sha(elf), 'patches': hooks}
    image, receipt = package_octave_120.package(stock, elf, plan)
    boot = check_octave_120_boot.check(image, elf)
    (args.out / 'nanohardcore-1.2-os.bin').write_bytes(image)

    # FIT tot op 2048 bytes uitgelijnd, net als het geteste re-nanocore-manifest: de
    # opvulling moet nul zijn en valt zo binnen de genormaliseerde stock-hash
    fit_end = max(s['offset'] + s['size'] for s in extract_fit(stock)['segments'])
    pad_end = (fit_end + 2047) & ~2047
    if any(stock[fit_end:pad_end]): raise SystemExit('FIT-opvulling is niet leeg')
    manifest = export_octave_web_patch.export(stock[:pad_end], image)
    manifest.update({'effect': 'HARDCORE', 'replaces': ['OCTAVER (FX2/PITCH)'],
                     'workInProgress': True, 'hardwareTested': False,
                     'payloadSha256': boot['payload_sha256'], 'elfSha256': sha(elf)})
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=1) + '\n')

    report = {'stock_sha256': sha(stock), 'image_sha256': sha(image), 'elf_sha256': sha(elf),
              'payload_bytes': boot['payload_bytes'], 'new_heap_start': receipt['new_heap_start'],
              'remaining_heap_span': receipt['remaining_heap_span'],
              'changed_ranges': len(receipt['changed_ranges']),
              'hooks': [h['description'] for h in hooks],
              'boot_check': {k: v for k, v in boot.items() if isinstance(v, bool)},
              'symbols': {k: hex(v) for k, v in symbols.items() if k.startswith('nc_')}}
    (args.out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
