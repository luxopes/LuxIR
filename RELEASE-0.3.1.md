# LuxIR 0.3.1

- **`luxir -a -l -t cíl [-p] -o program program.lir runtime.o …`** přeloží
  LIR, sestaví a slinkuje v jednom běhu (vstupy `.lir` se přeloží, ostatní
  jsou objekty pro linker). LSL 0.10.2 tak spouští pro program jediný
  proces backendu místo tří; překlad malých programů je o 10–20 % rychlejší
  než s GNU as a ld v 0.10.1. Výstup je bajtově shodný s oddělenými kroky.
- Rychlejší start LSL buildu: registrace řetězcových literálů C kódu
  (`mem.literal`) nekopíruje text a nevede slovník; mapa pro
  `literal_text` vzniká až při prvním dotazu.
- Generování kódu, assembler a linker jsou beze změny proti 0.3.0.

Ověřeno: `make check` s referenčním QBE 1.3, `make check-as`,
`make check-ld`; program LSL slinkovaný jedním během je bajtově shodný
s výstupem oddělených kroků a překladač LSL se tak přeloží sám bajtově
shodně.
