# LuxIR 0.2.0

- **Nové cíle.** `arm64_win` (Windows on ARM64: x18 je registr platformy
  a nikdy se nepoužije jako pomocný registr, sekce PE s explicitními
  příznaky) a `wasm32` (WebAssembly v textovém formátu pro `wat2wasm`).
  Jednotné ABI wasm: parametry i výsledek i64, nepřímá volání přes tabulku
  podle počtu argumentů. Každá hodnota `Int64` se ukládá také do rámce na
  stínovém zásobníku v lineární paměti, kde ji vidí konzervativní GC.
  Prolog s rámcem kontroluje mez stínového zásobníku (`$__stack_overflow`).
  Volba `-g` je pro vlákna wasi-threads: sdílená importovaná paměť, pasivní
  data (`$__wasm_init_memory`) a bezpečné body pro GC na vstupu funkcí a na
  zpětných skocích (`$__safepoint`, slovo `$__safepoint_flag`).
- **Opravy cílů.** `rv64`: slovo použité jako dlouhý operand se rozšiřuje
  nulami. `arm64_apple`: posun symbolu za mezí addendu Mach-O se načte
  zvlášť. Apple: jména sekcí bez uvozovek.
- **Konstantní skok.** `if` podle adresy symbolu (vždy nenulové) se složí
  na nepodmíněný skok; dřív výběr instrukcí končil asercí (nalezeno
  fuzzerem).
- **LuxIR v LSL na každém Linuxu.** Paměťová vrstva `lsl/luxir_memory.lsl`
  bere čísla systémových volání z platformy cíle, takže backend přeložený
  pro Linux arm64 nebo riscv64 běží nativně (LSL 0.10 ho dodává pro
  linux-arm64 a linux-riscv64).

Ověřeno: `make check` s referenčním QBE 1.3 (protokol 144 kontrol na 6
cílech, 31 kontrol linkování, dominance a bitsety, 36 chybových případů,
diferenciální test 456 dvojic vstup/cíl bez rozdílu proti C QBE pro C i LSL
build, fuzzer 300 případů bez problému), `make check-cross` (arm64 a rv64
v qemu 58 ok, assembler všech cílů bez chyby). Výstup LSL buildu je bajtově
shodný s C referencí na runtime LSL pro všech 9 kombinací cílů a voleb.
LSL 0.10 s tímto backendem prochází testy na Linuxu x86_64/arm64/riscv64,
Windows x86_64/arm64, macOS x86_64/arm64, WebAssembly a bare metal RISC-V.

Omezení: runtime `runtime/lsl/` pro `luxir -x` zůstává jen pro Linux
x86_64; přenositelný runtime LSL pro ostatní cíle je v repozitáři LSL.
Assembler a linker zůstávají systémové (binutils, LLVM), wasm vyžaduje WABT.
