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

# measurement tests (each prints its own table)
test: build
	$(CC) $(CFLAGS) -o build/ccmap    test/ccmap.c    $(DSP) -lm && ./build/ccmap | tail -3
	$(CC) $(CFLAGS) -o build/fit909   test/fit909.c   dsp/bd909.c -lm && ./build/fit909 5.14 2.0 1.5 0.2 0.5 0.5
	$(CC) $(CFLAGS) -o build/eqresp   test/eqresp.c   dsp/mackie.c dsp/mkcirc.c -lm && ./build/eqresp 0 0 1 0
	$(CC) $(CFLAGS) -o build/reso     test/reso.c     dsp/sherman.c -lm && ./build/reso
	$(CC) $(CFLAGS) -o build/alias    test/alias.c    dsp/sherman.c -lm && \
	  ./build/alias 2 0.30 0.5 0.5 build/al.bin && python3 test/alias_an.py build/al.bin
	python3 test/mna.py

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
