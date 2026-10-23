# LuxIR 0.4.3

Backend v LSL (`lsl/luxir.lsl`, sestavení `build/luxir`, které dodává LSL)
obsahuje opravu z 0.4.2.

- Vydání 0.4.2 změnilo jen C referenci (`src/ld/ld.c`); `lsl/luxir.lsl`
  zůstal vygenerovaný ze zdrojů 0.4.1, takže LSL build luxir stále psal
  dynamické programy bez hlaviček sekcí interpretu a tabulek ld.so a ty po
  `strip` nešly spustit. `lsl/luxir.lsl` je přegenerovaný z `src/`.
- Generování kódu, assembler a ostatní formáty jsou beze změny proti 0.4.2.

Ověřeno: `make check` a `make check-ld` (včetně `tests/dyn_check.py`, který
každý dynamický program spustí i po `strip`) pro C referenci i LSL build.
