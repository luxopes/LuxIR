# LuxIR 0.4.2

Dynamicky linkované programy (x86-64 Linux) přežijí `strip` a `objcopy`.

- BFD skládá výstupní soubor ze sekcí; interpret a tabulky ld.so dosud
  žádnou sekci neměly (ležely jen v segmentech `.rodata` a `.data`), takže
  `strip` je vynuloval a jádro program odmítlo („exec format error“ –
  makepkg stripuje balíky implicitně). Linker teď zapisuje hlavičky sekcí
  `.interp`, `.got`, `.dynsym` (s `sh_link` na `.dynstr`, `sh_info` 1,
  `sh_entsize` 24), `.dynstr`, `.hash`, `.rela.dyn` a `.dynamic`; `.rodata`
  a `.data` končí před nimi, sekce se nepřekrývají a rozložení segmentů
  v souboru je beze změny.
- `readelf -S/-d/--dyn-syms` tak zobrazí dynamickou část programu, zůstává
  i `e_shstrndx` na `.shstrtab`.
- Statické programy, ostatní formáty (PE, Mach-O, wasm, bare metal) a
  generování kódu jsou beze změny proti 0.4.1 (výstupy bajtově shodné).

Ověřeno: `tests/dyn_check.py` nově každý dynamický program po spuštění
zkopíruje, stripne (`strip`, je-li k dispozici) a spustí znovu se stejným
očekávaným výstupem; `make check` celé.
