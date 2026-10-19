# LuxIR: C reference, generovaný LSL backend a testy.
#
#   make                 C reference src/luxir
#   make lsl             přegeneruje lsl/luxir.lsl z src/ (Python + pycparser)
#   make native          sestaví build/luxir LSL compilerem
#   make runtime-lsl     runtime LSL (ABI lsl-0) pro luxir -x:
#                        build/luxir-runtime/lsl-0/amd64_sysv.o
#   make check           testy C reference a native backendu
#
# LSLC je LSL compiler (lslc) s podporou modulů označených
# "# lsl: internal-abi" mimo strom compileru. LSLC_BACKEND je volitelný
# backend předaný compileru jako třetí argument. QBE je referenční
# C QBE 1.3 pro diferenční testy.

LSLC ?= lslc
LSLC_BACKEND ?=
QBE ?= qbe
PYTHON ?= python3

all: c

c:
	$(MAKE) -C src

lsl:
	$(PYTHON) tools/translate.py

native: build/luxir

build/luxir: lsl/luxir.lsl lsl/luxir_memory.lsl lsl/luxir_stdio.lsl \
		lsl/luxir_stdio_raw.lsl lsl/luxir_float.lsl lsl/luxir_libc.lsl
	mkdir -p build
	$(LSLC) lsl/luxir.lsl build/luxir $(LSLC_BACKEND)
	chmod +x build/luxir

# Runtime LSL (ABI lsl-0, Linux x86_64). Emitory strojového kódu v LSL
# vypíšou bajty, štítky a fixupy; runtime_object.py z nich udělá objekt.
RUNTIME_LSL = runtime/lsl/linux-x86_64
RUNTIME_LSL_OBJ = build/luxir-runtime/lsl-0/amd64_sysv.o

runtime-lsl: $(RUNTIME_LSL_OBJ)

$(RUNTIME_LSL_OBJ): $(wildcard $(RUNTIME_LSL)/*.lsl) $(RUNTIME_LSL)/runtime_object.py
	mkdir -p build/runtime-lsl build/luxir-runtime/lsl-0
	$(LSLC) $(RUNTIME_LSL)/runtime_dump.lsl build/runtime-lsl/dump $(LSLC_BACKEND)
	build/runtime-lsl/dump build/runtime-lsl/runtime
	$(PYTHON) $(RUNTIME_LSL)/runtime_object.py build/runtime-lsl/runtime
	cp build/runtime-lsl/runtime.o $@

check: c native runtime-lsl
	$(PYTHON) tests/versioning.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/link.py --backend src/luxir --backend build/luxir
	$(MAKE) check-dominance check-bitsets
	$(PYTHON) tests/run_tests.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/differential.py --reference $(QBE) --backend src/luxir
	$(PYTHON) tests/differential.py --reference $(QBE) --backend build/luxir
	$(PYTHON) tests/fuzz.py --backend build/luxir --compare src/luxir --cases 300

# Independent graph oracle, including a build forcing Lengauer-Tarjan even
# on tiny CFGs. Run on the Linux development host.
check-dominance:
	mkdir -p build
	$(CC) -O2 -std=c99 -ffunction-sections -fdata-sections -Wl,--gc-sections \
		tests/dominance.c src/cfg.c -o build/dominance-test
	build/dominance-test
	$(CC) -O2 -std=c99 -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-DLUXIR_DOM_SMALL_LIMIT=0 tests/dominance.c src/cfg.c -o build/dominance-lt-test
	build/dominance-lt-test

check-bitsets:
	mkdir -p build
	$(CC) -O2 -std=c99 -ffunction-sections -fdata-sections -Wl,--gc-sections \
		tests/bitset_count.c src/util.c -o build/bitset-count-test
	build/bitset-count-test

# Režim -x s runtime LSL (examples/lsl-runtime-hello.lir).
check-link: c native runtime-lsl
	$(PYTHON) tests/link.py --backend src/luxir --backend build/luxir

check-runtime: c native
	$(PYTHON) tests/runtime/run_io.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/entry.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_atomic.py
	$(PYTHON) tests/runtime/run_thread.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_memory.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_integer.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_checked.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_os.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_startup.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/standalone.py --backend src/luxir --backend build/luxir

# Další cíle: Windows pod Wine (mingw-w64), arm64 a rv64 v qemu-user
# (cross gcc), upstream spustitelné testy QBE s C drivery a překlad výstupu
# assemblery všech cílů (clang).
CROSS = amd64_win arm64 rv64

check-cross: c native
	for t in $(CROSS); do \
		$(PYTHON) tests/run_tests.py --target $$t --backend src/luxir --backend build/luxir || exit 1; \
	done
	for t in amd64_sysv $(CROSS); do \
		$(PYTHON) tests/upstream_exec.py --target $$t --backend build/luxir || exit 1; \
	done
	tests/assemble.sh build/luxir tests/run/*.lir examples/*.lir

# C pro 32bitové cíle (c32): clang pro riscv32, arm a i686, qemu-user.
check-c32: c native
	$(PYTHON) tests/run_c32.py --backend src/luxir --backend build/luxir

# 32bitové cíle s assemblerem (arm, i386): GNU as, testovací runtime C
# (clang jen pro testy), i386 nativně, arm v qemu-user; rozdílový fuzzer
# proti amd64.
check-w32: c native
	$(PYTHON) tests/run_w32.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/w32_diff.py --backend src/luxir --cases 60
	$(PYTHON) tests/w32_diff.py --backend build/luxir --cases 20 --seed 2

# Vestavěný assembler (luxir -a): objekty bajtově shodné s GNU as na
# programech LuxIR a sondách relaxace skoků a výplní (tests/as/*.s).
check-as: c native
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --lir tests/run/*.lir
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --arch i386 --lir -t i386 tests/run/*.lir
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --probes tests/as/*64.s
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --arch i386 tests/as/*32.s
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --arch arm64 --lir -t arm64 tests/run/*.lir
	$(PYTHON) tests/as_diff.py --backend src/luxir --backend build/luxir --arch arm64 --probes tests/as/*_a64.s

# Vestavěný linker (luxir -l): programy LuxIR slinkované s testovacím
# runtime (x86-64 bez PIE i PIE, i386), spuštěné a smlouva obrazu.
check-ld: c native
	$(PYTHON) tests/ld_check.py --backend src/luxir --backend build/luxir --arch x86_64 --arch i386 --arch arm64

clean:
	$(MAKE) -C src clean
	rm -rf build

.PHONY: all c lsl native check check-dominance check-bitsets check-runtime check-cross check-c32 check-w32 check-as check-ld runtime-lsl check-link clean
