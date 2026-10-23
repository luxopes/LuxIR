# LuxIR 0.4.1

Dynamicky linkované programy pro Linux x86-64: LSL 0.10.4 na cíli
`linux-x86_64-gnu` volá glibc, GLib, GTK 3 a D-Bus.

- `luxir -l` (i v jednom běhu `-a -l -p`) se vstupem `.libs` se jmény
  sdílených knihoven (`libc.so.6`, `libgtk-3.so.0`, …) vytvoří PIE
  s `PT_INTERP` (`/lib64/ld-linux-x86-64.so.2`) a `PT_DYNAMIC`: nedefinované
  symboly jsou importy (`DT_NEEDED` pro každou knihovnu), volání jdou přes
  stub v `.plt` a slot `.got`, který ld.so naváže při startu (`BIND_NOW`,
  `DF_1_NOW | DF_1_PIE`, relokace `GLOB_DAT`); tabulky `.dynsym`, `.dynstr`,
  `.hash`, `.rela.dyn` a `.dynamic` leží za `.data`. Program nic nevyváží,
  absolutní relokace zůstávají chybou, zásobník je nespustitelný a žádný
  segment není RWX.
- Každý import se při linkování ověří proti dynamickým symbolům knihoven
  hostitele (`/lib64`, `/usr/lib64`, `/usr/lib/x86_64-linux-gnu`,
  `/lib/x86_64-linux-gnu`, `/usr/lib`, `/lib`, `/usr/local/lib`): chybějící
  funkce je chyba překladu, ne pád při startu; knihovna, která na hostiteli
  není, se přeskočí.
- Jen x86-64; statické programy, ostatní architektury a generování kódu
  jsou beze změny proti 0.4.0 (výstupy bajtově shodné).

Ověřeno: `make check-ld` (`tests/dyn_check.py`: `tests/run` slinkované
proti glibc hostitele, spuštěné a porovnané s `.out`, smlouva obrazu),
`make check` celé; testy cíle `linux-x86_64-gnu` v LSL (GLib, D-Bus přes
GIO, GTK pod Xvfb) lokálně i v CI.
