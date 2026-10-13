# LuxIR 0.1.2

- **Runtime LSL patří LuxIR.** Zdroje runtime jazyka LSL (model objektů,
  alokátor a GC, řetězce, kolekce, převody, chyby, I/O, procesy, vlákna,
  start programu) jsou převzaté z LSL do `runtime/lsl/linux-x86_64/`;
  `make runtime-lsl` z nich sestaví `build/luxir-runtime/lsl-0/amd64_sysv.o`,
  bajtově shodný s dosavadním `runtime.o` v LSL.
- **ABI `lsl-0`:** hlavička `runtime_abi "lsl-0"` označí program pro tento
  runtime. Rozhraní je dnešní rozhraní LSL beze změny; verze 0 ještě není
  stabilní. `luxir --version`: `LuxIR 0.1.2 (IR 1; runtime ABI none, lsl-0)`.
- **`luxir -x`:** z `.lir` rovnou spustitelný program. LuxIR přeloží,
  sestaví GNU `as` a slinkuje GNU `ld` (pevné cesty `/usr/bin`, `/bin`) se
  svým runtime jako statický PIE bez loaderu; dočasné soubory uklidí i při
  chybě. Runtime hledá vedle `luxir` (`luxir-runtime/`, `../lib/luxir/`,
  ve vývojovém checkoutu `../build/luxir-runtime/`) nebo v `-L dir`.
  Frontend LSL tak jen zapíše LuxIR.
- Nové testy `tests/link.py` (`make check-link`, součást `make check`),
  rozšířené `tests/versioning.py`, příklad `examples/lsl-runtime-hello.lir`,
  dokumentace `doc/runtime.md`, `doc/lsl-runtime.md`, `runtime/lsl/README.md`.

Ověřeno na LSL (`461dd58` s přepnutím na `luxir -x` ve worktree): kompilátor
v pevném bodu a bajtově shodný s dosavadní cestou, 227 binárek a 117
diagnostik testů a benchmarků shodných, rychlá i plná sada LSL prošla, čas
překladu beze změny. Self-host LuxIR přes starou i novou cestu: stage 1 ==
2 == 3. Podrobnosti ve VALIDATION.md a `validation/release-0.1.2/`.

Omezení: runtime je zatím jen pro Linux x86_64 a jde o převzaté emitory
strojového kódu x86, nikoli přenositelný LuxIR; assembler a linker jsou
stále GNU binutils. Přepis runtime, stabilní ABI a další cíle jsou v TODO.
