# LuxIR 0.3.0

- **Vestavěný assembler a linker** (`luxir -a`, `luxir -l`) pro x86-64,
  i386, AArch64, RISC-V 64 a ARM (A32): objekty ELF bajtově shodné
  s GNU as na všem, co LuxIR a runtime LSL vypisují, statické spustitelné
  soubory (PIE bez absolutních relokací, nespustitelný zásobník, bez
  tabulky symbolů, gc-sections). LSL 0.10.2 tak na Linuxu překládá bez
  binutils na všech pěti architekturách. Podrobnosti v README (Vestavěný
  assembler, Vestavěný linker).
- **32bitové cíle `arm` a `i386`** generují assembler přímo (bez C):
  64bitové hodnoty se rozkládají na dvojice slov (src/w32.c), pomocné
  funkce dělení a převodů jsou v runtime/w32/helpers.lir.
- Linker zarovnává na arm64 segmenty na 64 KiB (jádra se stránkou 16 nebo
  64 KiB) bez výplně v souboru.
- Omezení assembleru: bez TLS, GOT, Intel syntaxe, Thumb a literal poolů
  `ldr =`; RISC-V jen rv64g bez relaxace. Linker bez archivů, sdílených
  knihoven, TLS a linker skriptů (bare metal zůstává na GNU ld).

Ověřeno: `make check` s referenčním QBE 1.3, `make check-w32`,
`make check-as` (shoda s GNU as včetně generovaných sond relaxace a
konstant) a `make check-ld` (slinkované testovací programy na x86-64,
i386, arm64, rv64 a arm, poslední tři v qemu-user). V LSL 0.10.2 celé
sady testů na všech pěti Linuxových cílech a bajtově shodný self-host.
