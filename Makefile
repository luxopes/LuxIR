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

check: c native
	$(PYTHON) tests/run_tests.py --backend src/luxir --backend build/luxir
	$(PYTHON) tests/differential.py --reference $(QBE) --backend src/luxir
	$(PYTHON) tests/differential.py --reference $(QBE) --backend build/luxir
	$(PYTHON) tests/fuzz.py --backend build/luxir --compare src/luxir --cases 300

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

.PHONY: all c lsl native check check-cross clean
