# Kontrakt runtime LuxIR

Hlavička `runtime_abi` říká, nad jakým runtime program běží:

| Hodnota | Význam |
|---|---|
| `none` (výchozí) | LuxIR nedodává runtime. Výstupem je assembler, knihovny a linkování zajistí volající. |
| `lsl-0` | Runtime jazyka LSL dodávaný s LuxIR. `luxir -x` z programu udělá spustitelný soubor. |

`luxir --version` vypisuje podporované hodnoty. Čtečka neznámé hodnoty
odmítne před zpracováním definic; vstup bez hlavičky zůstává kompatibilní.

## Spustitelný program: `luxir -x`

```text
luxir -x [-t amd64_sysv] [-L adresář] -o program program.lir
```

LuxIR přeloží vstup, assembler zapíše do `program.s`, sestaví ho GNU `as`
a slinkuje GNU `ld` s objektem runtime jako statický PIE bez dynamického
loaderu (`-z text`, `-z noexecstack`, bez build-id, strip). Dočasné
`program.s` a `program.o` smaže; při chybě nezůstane ani `program`.
Assembler a linker se hledají jen v `/usr/bin` a `/bin`, nikdy v `PATH`.
Výsledek je bajtově deterministický.

Objekt runtime je `<adresář>/lsl-0/amd64_sysv.o`. Bez `-L` se hledá vedle
spuštěného `luxir`:

1. `<dir luxir>/luxir-runtime/`,
2. `<dir luxir>/../lib/luxir/` (instalace do prefixu, `bin/luxir`),
3. `<dir luxir>/../build/luxir-runtime/` (C reference `src/luxir` ve
   vývojovém checkoutu po `make runtime-lsl`).

Runtime `lsl-0` je zatím jen pro `amd64_sysv` (Linux x86_64). Vlastní
assembler a linker a další cíle jsou v [TODO](../TODO.md).

## ABI `lsl-0`

Rozhraní převzaté beze změny z LSL (stav LSL `461dd58`): model hodnot a
objektů, chyby v TLS, kořeny GC, 145 funkcí `rt_runtime_*` a symboly, které
musí definovat program (`lsl_entry`, `lsl_globals`, `lsl_global_bytes`,
`lsl_global_flags`). Popis: [lsl-runtime.md](lsl-runtime.md), nejmenší
program: [examples/lsl-runtime-hello.lir](../examples/lsl-runtime-hello.lir).
Verze 0 znamená, že rozhraní ještě není stabilní: LuxIR ho bude měnit
(tagované hodnoty, přesné GC) a frontend se přizpůsobí nové verzi ABI.
