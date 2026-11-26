#!/usr/bin/env python3
"""Regression gate for nanohardcore: builds the measurement programs, runs them and
fails (exit 1) when a measured property leaves its tolerance. Every limit below is
a claim made in README.md / docs/ - change the code, not the limit, unless the claim
itself is deliberately revised (and then update the docs in the same PR).

Run: python3 test/check.py   (needs gcc, numpy, scipy)"""
import math, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
os.makedirs(B, exist_ok=True)
CC = os.environ.get("CC", "gcc")
FLAGS = ["-O2", "-std=gnu99"]
DSP = ["dsp/bd909.c", "dsp/sherman.c", "dsp/mackie.c", "dsp/mkcirc.c", "dsp/chain.c"]
fails = []


def build(name, srcs):
    out = os.path.join(B, name)
    subprocess.run([CC, *FLAGS, "-o", out, *srcs, "-lm"], cwd=ROOT, check=True)
    return out


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True).stdout


def check(name, ok, detail):
    print(f"[{'ok ' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        fails.append(name)


def st(a, b):
    return 12 * math.log2(max(a, 1e-9) / b)


# 909 pitch track against measured units (TUNE mid / max), body hold
fit = build("fit909", ["test/fit909.c", "dsp/bd909.c"])
p = lambda o: {int(a): float(b) for a, b in re.findall(r"(\d+)ms:(\d+)Hz", o)}
mid = run([fit, "0.5", "0.5"])
hi = run([fit, "1.0", "0.5"])
m, h = p(mid), p(hi)
check("909 230 Hz @5 ms", abs(st(m[5], 230)) < 1.0, f"{m[5]:.0f} Hz")
check("909 160 Hz @10 ms", abs(st(m[10], 160)) < 1.0, f"{m[10]:.0f} Hz")
check("909 rest ~47 Hz @80 ms", abs(st(m[80], 47)) < 0.7, f"{m[80]:.0f} Hz")
check("909 TUNE max 111 Hz @40 ms", abs(st(h[40], 111)) < 1.5, f"{h[40]:.0f} Hz")
hold = float(re.search(r"hold\(-1dB\) (\d+)ms", mid).group(1))
check("909 body hold (decay .5)", 30 <= hold <= 80, f"{hold:.0f} ms")

# Mackie EQ: circuit simulation vs published ranges
eq = build("eqresp", ["test/eqresp.c", "dsp/mackie.c", "dsp/mkcirc.c"])
r = lambda o: {int(a): float(b) for a, b in re.findall(r"(\d+):([+-]\d+\.\d)", o)}
lo, mi, hs, lc = (r(run([eq, *a])) for a in (["0", "0", "1", "0"], ["0", "1", "0", "0"], ["1", "0", "0", "0"], ["0", "0", "0", "1"]))
check("Mackie LOW +14 dB @80 Hz", abs(lo[80] - 14.2) < 1.0, f"{lo[80]:+.1f} dB")
check("Mackie MID +13 dB @2.5 kHz", abs(mi[2500] - 13.2) < 1.0, f"{mi[2500]:+.1f} dB")
check("Mackie HI +18 dB @12 kHz", abs(hs[12000] - 18.2) < 1.0, f"{hs[12000]:+.1f} dB")
check("Mackie LOW CUT -16.5 dB @50 Hz", abs(lc[50] + 16.5) < 1.0, f"{lc[50]:+.1f} dB")

# Sherman resonance: no self-oscillation at or below 1.5 kHz, oscillation high up
rs = run([build("reso", ["test/reso.c", "dsp/sherman.c"])])
rows = re.findall(r"^\s*(\d\.\d)\s+((?:\S+\s*){7})$", rs, re.M)
low_osc = any("OSC" in row[1].split()[:4] for row in rows)
high_osc = any(row[1].split()[6] == "OSC" for row in rows)
check("Sherman: no sub/low self-oscillation (<=1.5 kHz)", not low_osc, "zelfoscillatie laag" if low_osc else "geen")
check("Sherman: self-oscillates at 8 kHz", high_osc, "ja" if high_osc else "nee")

# Sherman model aliasing vs 8x reference
al = build("alias", ["test/alias.c", "dsp/sherman.c"])
worst_all = -200.0
for cfg in (["11", "0.40", "0.5", "0.5"], ["2", "0.30", "0.5", "0.5"], ["0", "0.75", "0.6", "0.5"]):
    binf = os.path.join(B, "al_" + "_".join(cfg) + ".bin")
    run([al, *cfg, binf])
    o = run([sys.executable, "test/alias_an.py", binf])
    worst_all = max(worst_all, float(re.search(r"worst model-only component (-?\d+\.\d)", o).group(1)))
check("Sherman model-only aliasing", worst_all < -45.0, f"{worst_all:.1f} dB")

# every mapped CC audible, channel-mode CCs ignored
o = run([build("ccmap", ["test/ccmap.c", *DSP])])
dead = int(re.search(r"(\d+) van \d+ CC's zonder effect", o).group(1))
react = o.count("reageert!")
check("MIDI: every mapped CC has an effect", dead == 0, f"{dead} zonder effect")
check("MIDI: CC 120-127 ignored", react == 0, f"{react} reageren")

# offline render smoke test: triggers detected, no NaN
os.makedirs(os.path.join(B, "out"), exist_ok=True)
o = run([build("render", ["host/render.c", *DSP]), os.path.join(B, "out")])
mp = re.search(r"pulses: sent (\d+) detected (\d+)", o)
check("trigger: all pulses detected", mp and mp.group(1) == mp.group(2), mp.group(0) if mp else "geen output")

print(f"\n{len(fails)} failure(s)" + (": " + ", ".join(fails) if fails else ""))
sys.exit(1 if fails else 0)
