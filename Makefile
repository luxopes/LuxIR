# LuxIR: C reference, generovaný LSL backend a testy.
#
#   make                 C reference src/luxir
#   make lsl             přegeneruje lsl/luxir.lsl z src/ (Python + pycparser)
#   make native          sestaví build/luxir LSL compilerem
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

check: c native
	$(PYTHON) tests/gcroot.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/invoke.py --backend src/luxir --backend build/luxir
	$(PYTHON) tools/runtime_manifest.py --check
	$(PYTHON) tests/runtime_abi.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/inline.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/atomic_ir.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/numeric.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/versioning.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/qbe2lir_extern.py --backend src/luxir --backend build/luxir
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

check-runtime: c native
	$(PYTHON) tests/build_driver.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/data_sections.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_relocate.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_tls.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/tls_entry.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_platform.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_io.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/entry.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_atomic.py
	$(PYTHON) tests/runtime/run_thread.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_memory.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_integer.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_checked.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_arena.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_managed.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_process.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_process.py --capture --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_extended.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_hash.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_collections.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/run_float64.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/runtime/float64.py --backend src/luxir --backend build/luxir
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

clean:
	$(MAKE) -C src clean
	rm -rf build

.PHONY: all c lsl native check check-dominance check-bitsets check-runtime check-cross clean
