# LuxIR 0.4.0

Vestavěný assembler a linker pro všechny cíle: LSL 0.10.3 překládá bez
binutils, LLVM, Xcode a wabt.

- **Windows** (`-t amd64_win`, `-t arm64_win`): `luxir -a` zapisuje objekty
  COFF (x86-64 bajtově jako GNU as pro pe-x86-64, arm64 bajtově jako LLVM),
  `luxir -l` linkuje konzolový program PE32+ (ASLR s high-entropy VA, DEP,
  zásobník 8 MiB, bázové relokace, bez tabulky symbolů a časového razítka);
  importy popisují soubory `.def` mezi vstupy. Sekce s více než 65535
  relokacemi používají `IMAGE_SCN_LNK_NRELOC_OVFL`.
- **macOS** (`-t amd64_apple`, `-t arm64_apple`): objekty Mach-O bajtově jako
  LLVM (atomy po symbolech, `@PAGE`/`@PAGEOFF`, ADDEND, `ltmpN`, relaxace
  skoků a nop jako LLVM), spustitelný Mach-O (PIE) ze stubu `.tbd`: dead
  strip, stuby přes GOT navázané dyld, rebase/bind, `LC_MAIN`, `LC_UUID`
  z obsahu a ad-hoc podpis (SHA-256, linker-signed). `-M soubor` zapíše mapu
  rozvržení pro testy.
- **WebAssembly** (`luxir -a -t wasm32`): text WAT na binární modul bajtově
  jako `wat2wasm`; se vstupem `.lir` v jednom běhu (`-a [-g] -t wasm32 -o
  modul.wasm vrstva.wat program.lir`).
- **Bare metal RISC-V** (`luxir -l -b -t rv64`): rozložení desky LSL místo
  linker skriptu, paměťový obraz shodný s GNU ld.
- `-a -l` pro macOS připojí `.subsections_via_symbols`; na arm64 se kód
  v PE zarovnává nejméně na 4 bajty a tabulka importů na 8 bajtů.
- Symboly assembleru i linkeru jsou v hašovací tabulce místo lineárního
  hledání: LSL build sestaví a slinkuje program o 5 % rychleji než 0.3.1.
- Generování kódu je beze změny proti 0.3.1; výstupy pro Linux jsou bajtově
  shodné.

Ověřeno: `make check-as` (COFF a Mach-O proti referenčním assemblerům,
sondy instrukcí a relaxace, `tests/run`, runtime LSL), `make check-ld`
(`pe_check.py` ve wine, `macho_check.py` proti mapě rozvržení,
`ld_bare.py` proti GNU ld), `make check-wasm`; testy cílů LSL na Windows,
macOS, Linuxu, ve wasmtime a qemu v CI LSL.
