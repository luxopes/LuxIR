# Kontrakt runtime LuxIR

Aktuální LuxIR vytváří assembler a neobsahuje integrovanou knihovnu luxrt.
Podporovaná deklarace je proto `runtime_abi "none"`; knihovnu pro externí
volání a její kompatibilitu stále zajišťuje frontend a linker.

`luxir --version` vypisuje podporovaný kontrakt. Čtečka přijme explicitní
`ir_version "1"` a `runtime_abi "none"`; neznámé verze odmítne před
zpracováním definic. Vstup bez hlavičky zůstává kompatibilní se stávajícím
frontendem LSL. Hlavičky se kontrolují pro každý vstupní soubor samostatně.

To je základ verzování, nikoli implementace luxrt nebo certifikace ABI
externího runtime. Budoucí číslované luxrt ABI musí popsat layout hodnot a
objektů, GC mapy, safepointy, volání/chyby a OS vrstvu. Teprve poté se přidá
podporovaná revize a kontrola kombinace IR/runtime při linkování.
