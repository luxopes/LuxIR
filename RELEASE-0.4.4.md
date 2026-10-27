# LuxIR 0.4.4

Dynamický program x86-64 (vstup `.libs`, cíl `linux-x86_64-gnu` v LSL) má
verze importovaných symbolů jako u GNU ld.

- Linker čte z knihoven hostitele `.gnu.version` a `.gnu.version_d` a ke
  každému importu zapíše výchozí verzi knihovny (`.gnu.version`,
  `.gnu.version_r`, `DT_VERSYM`, `DT_VERNEED`, `DT_VERNEEDNUM`). Dřív byly
  importy bez verze a ld.so je navázal na nejstarší verzi symbolu: např.
  `realpath(cesta, NULL)` volalo kompatibilní `realpath@GLIBC_2.2.5`, které
  NULL nepřijímá (EINVAL). Skryté (starší) verze se při výběru přeskočí.
- Program bez verzovaných importů (knihovny bez verzí, křížový překlad bez
  knihoven na hostiteli) se zapisuje beze změny proti 0.4.3. Program
  s verzemi potřebuje knihovnu alespoň té verze, se kterou byl slinkovaný.
- `lsl/luxir.lsl` je přegenerovaný z `src/` (`tools/translate.py`,
  pycparser 3.0); statické ELF, PE a Mach-O výstupy jsou bajtově shodné
  s 0.4.3.

Ověřeno: `tests/dyn_check.py` pro C referenci i LSL build (každý dynamický
program spuštěný i po `strip`), `readelf -V` a `realpath(cesta, NULL)` z LSL,
bajtová shoda výstupu C reference a LSL buildu a statických programů
proti 0.4.3. Sady `ld_check`, `pe_check` a `macho_check` potřebují clang
a llvm-objdump, které na stroji vydání nebyly.
