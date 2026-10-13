# Runtime LSL (ABI `lsl-0`)

Runtime jazyka LSL, se kterým `luxir -x` linkuje programy s hlavičkou
`runtime_abi "lsl-0"`: model objektů, alokátor a GC, řetězce, kolekce,
převody čísel, chyby, I/O, procesy, vlákna a start programu. Rozhraní je
popsané v [doc/lsl-runtime.md](../../doc/lsl-runtime.md) a
[doc/runtime.md](../../doc/runtime.md).

## Původ a stav

Zdroje v `linux-x86_64/` jsou převzaté z LSL (`compiler/pure/`, commit
`461dd58` větve `work/runtime-luxir`) beze změny chování. Jsou to emitory
strojového kódu x86-64 napsané v LSL; každý modul má navíc značku
`# lsl: internal-abi`, protože leží mimo strom překladače LSL. Objekt z nich
sestavený je bajtově shodný s `compiler/qbe/runtime.o` z LSL.

Přenositelná náhrada v LuxIR IR (tagované hodnoty, přesné GC, další cíle)
je v [TODO](../../TODO.md). Emitory zůstávají referenčním chováním, dokud ji
neprověří stejné testy a měření.

## Sestavení

```sh
make runtime-lsl LSLC=/cesta/k/lslc      # build/luxir-runtime/lsl-0/amd64_sysv.o
```

1. `runtime_dump.lsl` přeložený LSL compilerem vypíše bajty kódu, štítky
   a fixupy (`.bin`, `.labels`, `.fixups`).
2. `runtime_object.py` z nich napíše assembler (`.s`) s exportovanými
   symboly a můstky `rt_runtime_*` pro SysV volání a sestaví ho `as --64`.

Výsledek se instaluje vedle `luxir` jako `luxir-runtime/lsl-0/amd64_sysv.o`
nebo do `lib/luxir/lsl-0/amd64_sysv.o` prefixu. Test: `make check-link`.
