"""xthead.py - T-Head (XuanTie E9xx) custom instructions for Unicorn.

The NanoCore firmware uses the XThead* extensions (indexed loads/stores, bitfield
extract, conditional moves, multiply-accumulate, cache maintenance). Unicorn's RISC-V
core does not know them and raises an illegal-instruction exception. This module
decodes them from the image's own disassembly (llvm-objdump with the XThead features)
and executes them in Python on that exception, then resumes after the instruction.

Use: xthead.install(uc, segments)  with segments = [(load_address, bytes), ...]
Only for offline emulation; says nothing about timing or the real cache behaviour.
"""
import re, struct, subprocess, tempfile
from pathlib import Path
from unicorn import UC_HOOK_INTR
from unicorn import riscv_const as r

MATTR = ('+m,+a,+f,+d,+c,+xtheadba,+xtheadbb,+xtheadbs,+xtheadcmo,+xtheadcondmov,'
         '+xtheadfmemidx,+xtheadmac,+xtheadmemidx,+xtheadmempair,+xtheadsync')
ABI = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1'] + \
      [f'a{i}' for i in range(8)] + [f's{i}' for i in range(2, 12)] + [f't{i}' for i in range(3, 7)]
REG = {n: i for i, n in enumerate(ABI)}
REG['fp'] = 8
NOPS = {'th.sync', 'th.sync.i', 'th.sync.s', 'th.sync.is', 'th.icache.iall', 'th.icache.ialls',
        'th.dcache.call', 'th.dcache.iall', 'th.dcache.ciall', 'th.dcache.cpa', 'th.dcache.ipa',
        'th.dcache.cipa', 'th.dcache.cva', 'th.dcache.iva', 'th.dcache.civa', 'th.dcache.csw',
        'th.dcache.isw', 'th.dcache.cisw', 'th.l2cache.call', 'th.l2cache.iall', 'th.l2cache.ciall'}


def decode(segments):
    """Map pc -> (mnemonic, [operands], size) for every th.* instruction in the segments."""
    table = {}
    with tempfile.TemporaryDirectory() as d:
        for addr, data in segments:
            b = Path(d) / 'seg.bin'; b.write_bytes(data)
            s = Path(d) / 'w.S'; s.write_text('.section .text,"ax"\n.globl _start\n_start:\n.incbin "%s"\n' % b)
            o, e = Path(d) / 'w.o', Path(d) / 'w.elf'
            subprocess.run(['clang', '--target=riscv32-unknown-elf', '-march=rv32imac', '-c', str(s), '-o', str(o)], check=True)
            subprocess.run(['ld.lld', '-Ttext=%#x' % addr, str(o), '-o', str(e)], check=True, capture_output=True)
            dis = subprocess.run(['llvm-objdump', '-d', '--mattr=' + MATTR, str(e)], check=True,
                                 capture_output=True, text=True).stdout
            for line in dis.splitlines():
                m = re.match(r'([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(th\.\S+)\s*(.*)', line)
                if m:
                    ops = [x.strip() for x in re.split(r'[,()]', m.group(4).split('<')[0]) if x.strip()]
                    table[int(m.group(1), 16)] = (m.group(3), ops, len(m.group(2).split()))
    return table


def _imm(s):
    return int(s, 0)


def install(uc, segments):
    table = decode(segments)
    stats = {}

    def rg(n): return 0 if n == 'zero' else uc.reg_read(r.UC_RISCV_REG_X0 + REG[n])
    def wg(n, v):
        if n != 'zero': uc.reg_write(r.UC_RISCV_REG_X0 + REG[n], v & 0xffffffff)
    def sx(v, bits):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v >> (bits - 1) else v
    def ld(a, n, signed):
        v = int.from_bytes(uc.mem_read(a & 0xffffffff, n), 'little')
        return sx(v, 8 * n) & 0xffffffff if signed else v
    def st(a, n, v): uc.mem_write(a & 0xffffffff, (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little'))
    SIZES = {'b': (1, True), 'bu': (1, False), 'h': (2, True), 'hu': (2, False), 'w': (4, True), 'wu': (4, False)}

    def run(mn, o):
        if mn in NOPS: return
        if mn == 'th.addsl': wg(o[0], rg(o[1]) + (rg(o[2]) << _imm(o[3]))); return
        if mn in ('th.ext', 'th.extu'):
            msb, lsb = _imm(o[2]), _imm(o[3]); v = (rg(o[1]) >> lsb) & ((1 << (msb - lsb + 1)) - 1)
            wg(o[0], sx(v, msb - lsb + 1) if mn == 'th.ext' else v); return
        if mn == 'th.mveqz':
            if rg(o[2]) == 0: wg(o[0], rg(o[1]))
            return
        if mn == 'th.mvnez':
            if rg(o[2]) != 0: wg(o[0], rg(o[1]))
            return
        if mn == 'th.mula': wg(o[0], rg(o[0]) + rg(o[1]) * rg(o[2])); return
        if mn == 'th.muls': wg(o[0], rg(o[0]) - rg(o[1]) * rg(o[2])); return
        if mn == 'th.mulah': wg(o[0], rg(o[0]) + sx(rg(o[1]), 16) * sx(rg(o[2]), 16)); return
        if mn == 'th.mulsh': wg(o[0], rg(o[0]) - sx(rg(o[1]), 16) * sx(rg(o[2]), 16)); return
        if mn == 'th.rev': wg(o[0], int.from_bytes(rg(o[1]).to_bytes(4, 'little'), 'big')); return
        if mn == 'th.ff1': v = rg(o[1]); wg(o[0], 32 - v.bit_length()); return
        if mn == 'th.ff0': v = (~rg(o[1])) & 0xffffffff; wg(o[0], 32 - v.bit_length()); return
        if mn == 'th.tstnbz':
            v = rg(o[1]); wg(o[0], sum((0xff << 8 * i) for i in range(4) if (v >> 8 * i) & 0xff == 0)); return
        if mn == 'th.srri': n = _imm(o[2]) & 31; v = rg(o[1]); wg(o[0], (v >> n) | (v << (32 - n))); return
        m = re.match(r'th\.(l|s)r(bu|hu|wu|b|h|w)$', mn)               # indexed: rd, rs1, rs2, imm2
        if m:
            n, signed = SIZES[m.group(2)]; a = rg(o[1]) + (rg(o[2]) << _imm(o[3]))
            if m.group(1) == 'l': wg(o[0], ld(a, n, signed))
            else: st(a, n, rg(o[0]))
            return
        m = re.match(r'th\.(l|s)(bu|hu|b|h|w)(ia|ib)$', mn)            # rd, (rs1), imm5, imm2
        if m:
            n, signed = SIZES[m.group(2)]; base = rg(o[1]); inc = _imm(o[2]) << _imm(o[3])
            a = base + inc if m.group(3) == 'ib' else base
            if m.group(1) == 'l': v = ld(a, n, signed)
            else: st(a, n, rg(o[0]))
            wg(o[1], base + inc)
            if m.group(1) == 'l': wg(o[0], v)
            return
        if mn in ('th.lwd', 'th.lwud', 'th.swd'):                      # rd1, rd2, (rs1), imm2, 3
            a = rg(o[2]) + (_imm(o[3]) << 3)
            if mn == 'th.swd': st(a, 4, rg(o[0])); st(a + 4, 4, rg(o[1]))
            else: wg(o[0], ld(a, 4, False)); wg(o[1], ld(a + 4, 4, False))
            return
        raise NotImplementedError(mn)

    def pext(word):
        """P-extension (packed SIMD, opcode 0x77): only what the firmware uses."""
        rd, f3, rs1, rs2, f7 = (word >> 7) & 31, (word >> 12) & 7, (word >> 15) & 31, (word >> 20) & 31, word >> 25
        X = lambda i: 0 if i == 0 else uc.reg_read(r.UC_RISCV_REG_X0 + i)
        W = lambda i, v: i and uc.reg_write(r.UC_RISCV_REG_X0 + i, v & 0xffffffff)
        pair = lambda i: X(i) | (X(i + 1) << 32)
        if f3 == 1 and f7 in (0x60, 0x61):                        # ADD64 / SUB64 (register pairs)
            v = pair(rs1) + pair(rs2) if f7 == 0x60 else pair(rs1) - pair(rs2)
            W(rd, v); W(rd + 1, v >> 32); return 'p.add64' if f7 == 0x60 else 'p.sub64'
        raise RuntimeError('onbekende P-instructie %#010x (funct7 %#x funct3 %d)' % (word, f7, f3))

    def on_intr(u, intno, _):
        # Unicorn meldt bij een ongeldige 4-byte-instructie PC = instructie + 4
        nxt = u.reg_read(r.UC_RISCV_REG_PC); pc = nxt - 4
        ins = table.get(pc)
        if ins is not None:
            mn, ops, size = ins
            run(mn, ops)
        else:
            word = int.from_bytes(u.mem_read(pc, 4), 'little')
            if word & 0x7f != 0x77:
                raise RuntimeError('onbekende instructie %#010x op %#x (intno %d)' % (word, pc, intno))
            mn = pext(word)
        stats[mn] = stats.get(mn, 0) + 1
        u.reg_write(r.UC_RISCV_REG_PC, nxt)

    uc.hook_add(UC_HOOK_INTR, on_intr)
    return stats
