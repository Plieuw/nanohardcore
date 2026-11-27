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

# device-facing behaviour: boot grace, AUDIO mode, 48 kHz, 12-control bank
o = run([build("device", ["test/device.c", *DSP])])
v = dict((k, float(x)) for k, x in re.findall(r"^(\w+) ([-\d.]+)$", o, re.M))
check("boot grace: silent for 3 s in trigger mode", v["grace_dry_max_err"] < 1e-6, f"{v['grace_dry_max_err']:.2e}")
check("boot grace: processing after the grace", v["after_grace_processed"] > 0.05, f"{v['after_grace_processed']:.3f}")
check("48 kHz: all 7 pulses after the grace detected", v["triggers_48k"] == 7, f"{v['triggers_48k']:.0f}")
check("AUDIO mode: silence in = silence out (909 off)", v["audio_mode_silence"] < 1e-4, f"{v['audio_mode_silence']:.2e}")
check("AUDIO mode: input reaches the output", v["audio_mode_sine_out"] > 0.02, f"{v['audio_mode_sine_out']:.3f}")
check("device bank: ENV>F1 at 0.5 is exactly 0", v["dev_env_zero"] == 0.0, f"{v['dev_env_zero']}")
check("device bank: HARM switch top = x16 (index 11)", v["dev_harm_max"] == 11, f"{v['dev_harm_max']:.0f}")
check("device bank: NaN control falls back to default", abs(v["dev_nan_default"] - 0.333) < 1e-3, f"{v['dev_nan_default']}")
check("device bank: INPUT switch top = AUDIO+909", v["dev_mode_a909"] == 2, f"{v['dev_mode_a909']:.0f}")
check("device bank: Sherman cutoff ceiling 8 kHz (CPU)", v["dev_fmax"] <= 8000, f"{v['dev_fmax']:.0f} Hz")
check("device bank: CC 106 cannot lift the pedal ceiling", v["dev_fmax_after_cc106"] <= 8000, f"{v['dev_fmax_after_cc106']:.0f} Hz")
check("device bank: moving one knob keeps MIDI-set values", abs(v["dev_midi_tune_kept"] - 1.0) < 1e-6, f"{v['dev_midi_tune_kept']}")
check("state fits the 65,536-byte native ring", v["state_bytes"] < 65536, f"{v['state_bytes']:.0f} bytes")

# vector UI (ui/vec.c): bounds, boot screen layout and build-up, font coverage, beam profile
o = run([build("vectest", ["test/vec.c", "ui/vec.c"])])
check("vec: no writes outside the 320x240 window", "guard 1" in o, re.search(r"guard \d", o).group(0))
n, x0, x1, y0, y1 = map(int, re.search(r"title (\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", o).groups())
check("vec boot: title centred (+-3 px)", abs((x0 + x1) / 2 - 160) <= 3, f"centre {(x0 + x1) / 2:.1f}")
check("vec boot: title inside 12 px margins", x0 >= 12 and x1 <= 307, f"x {x0}..{x1}")
bu = [(int(a), int(b)) for a, b in re.findall(r"(\d+):(\d+)", re.search(r"build(.*)", o).group(1))]
pre = max(c for t_, c in bu if t_ <= 500); peak = max(c for _, c in bu)
grow = all(b2 >= b1 - 2 for (t1, b1), (t2, b2) in zip(bu, bu[1:]) if t2 <= 1800)
check("vec boot: title not visible before the beam starts", pre <= 10, f"{pre} px")
check("vec boot: title builds up monotonically until 1.8 s", grow, "ok" if grow else "daalt")
check("vec boot: finished title stays (>= 97% of peak)", bu[-1][1] >= 0.97 * peak, f"{bu[-1][1]}/{peak}")
check("vec boot: deterministic in t", "deterministic 1" in o, "")
check("vec font: all UI characters present, unknown rejected", "glyphs 1 1 0" in o, re.search(r"glyphs.*", o).group(0))
prof = list(map(int, re.search(r"profile (.*)", o).group(1).split()))
check("vec beam: symmetric core with glow, < 4 px wide", prof == prof[::-1] and prof[0] == 0 and max(prof) == 31,
      " ".join(map(str, prof)))
# HARDCORE UI (ui/hcui.c): parameters round-trip through the chain, gestures, drawing bounds
o = run([build("uitest", ["test/ui.c", "ui/hcui.c", "ui/vec.c", *DSP])])
m = re.search(r"params (\d+) unstable (\d+) inexact (\d+) glyphs_missing (\d+)", o)
check("ui: every parameter stable through the chain", m.group(2) == "0", f"{m.group(1)} parameters, {m.group(2)} instabiel")
check("ui: continuous parameters exact for all 128 CC values", m.group(3) == "0", m.group(3))
check("ui: every label and value has vector glyphs", m.group(4) == "0", m.group(4))
g = dict(re.findall(r"(\w+) (-?\d+)", re.search(r"gestures (.*)", o).group(1)))
check("ui: B turn coarse = 4/count, fine = 1", g["coarse"] == "8" and g["fine"] == "1" and g["fine_on"] == "1", f"coarse {g['coarse']} (2 tellen) fine {g['fine']}")
check("ui: B hold returns to the start value", g["reset"] == "1", g["reset"])
check("ui: A turn wraps over the 4 blocks", g["blk"] == "1" and g["wrap"] == "3", f"{g['blk']} / {g['wrap']}")
check("ui: A press opens / next page, A hold = home", g["open"] == "1" and g["page2"] == "1" and g["home"] == "1", f"{g['open']}{g['page2']}{g['home']}")
check("ui: B press toggles a switch (LOW CUT)", g["lowcut"] == "1", g["lowcut"])
check("ui: A+B press = bypass, A+B hold does not toggle", g["bypass"] == "1" and g["bypass_long"] == "1", f"{g['bypass']}/{g['bypass_long']}")
check("ui: B hold on a bipolar control = exactly 0", g["hi_zero"] == "1", g["hi_zero"])
m = re.search(r"draw screens (\d+) outside (\d+)", o)
check("ui: all screens 16/32 bit, landscape/portrait, inside the buffer", m.group(2) == "0", f"{m.group(1)} schermen")

# freestanding for the pedal: RV32 build without libm/libc and without writable statics
import shutil
if shutil.which("clang") and shutil.which("llvm-objdump"):
    for src, allowed in (("ui/vec.c", set()), ("ui/hcui.c", {"chain_midi"})):
        obj = os.path.join(B, os.path.basename(src) + ".rv.o")
        subprocess.run(["clang", "--target=riscv32-unknown-elf", "-march=rv32imafdc", "-mabi=ilp32d", "-O2",
                        "-ffreestanding", "-fno-builtin", "-nostdlib", "-msmall-data-limit=0", "-Werror", "-Wall", "-Wextra", "-c",
                        src, "-o", obj], cwd=ROOT, check=True)
        hdr = run(["llvm-objdump", "-h", obj])
        writable = [l.split()[1] for l in hdr.splitlines() if re.search(r"\.(s?data|s?bss)\b", l) and int(l.split()[2], 16) > 0]
        und = [s for s in run(["llvm-nm", "-u", obj]).split() if s not in allowed and s not in ("U",)]
        und = [s for s in und if not s.startswith("vec_")]
        check(f"{src} RV32: freestanding, no writable statics", not writable and not und,
              f"writable={writable} undefined={und}")

print(f"\n{len(fails)} failure(s)" + (": " + ", ".join(fails) if fails else ""))
sys.exit(1 if fails else 0)
