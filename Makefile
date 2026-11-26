# nanohardcore - host builds, tests and the RV32 (NanoCore ISA) build + emulation
CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu99 -Wall -Wextra
DSP      = dsp/bd909.c dsp/sherman.c dsp/mackie.c dsp/mkcirc.c dsp/chain.c
RVFLAGS  = --target=riscv32-unknown-elf -march=rv32imafdc -mabi=ilp32d -O2 -ffreestanding \
           -fno-builtin -nostdlib -msmall-data-limit=0 -fno-pic

.PHONY: all render test rv clean

all: render

build:
	mkdir -p build out

# offline renders of all demo scenarios -> out/*.wav
render: build
	$(CC) $(CFLAGS) -o build/render host/render.c $(DSP) -lm
	./build/render out

# regression gate: builds the measurement programs and fails when a documented
# property (909 pitch track, Mackie EQ, Sherman resonance/aliasing, MIDI map) drifts
test:
	python3 test/check.py

# cross-compile for the NanoCore ISA and count instructions in Unicorn
# (needs clang + ld.lld, pip install unicorn pyelftools)
rv: build
	for f in $(DSP) rv/harness.c; do clang $(RVFLAGS) -c $$f -o build/$$(basename $$f .c).rv.o || exit 1; done
	ld.lld -Ttext=0x10000 -e _start build/harness.rv.o build/chain.rv.o build/bd909.rv.o \
	  build/sherman.rv.o build/mackie.rv.o build/mkcirc.rv.o -o rv/test.elf
	llvm-size rv/test.elf
	python3 rv/emu.py

clean:
	rm -rf build rv/test.elf
