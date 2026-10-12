# Kontrakt runtime LuxIR

Aktuální LuxIR vytváří assembler a neobsahuje integrovanou knihovnu luxrt.
Podporovaná deklarace je proto `runtime_abi "none"`; knihovnu pro externí
volání a její kompatibilitu stále zajišťuje frontend a linker.

`luxir --version` vypisuje podporovaný kontrakt. Čtečka přijme explicitní
`ir_version "1"` a `runtime_abi "none"`; neznámé verze odmítne před
zpracováním definic. Vstup bez hlavičky zůstává kompatibilní se stávajícím
frontendem LSL. Hlavičky se kontrolují pro každý vstupní soubor samostatně.

To je základ verzování, nikoli implementace luxrt nebo certifikace ABI
externího runtime. Budoucí číslované luxrt ABI musí popsat obecné služby, volání/chyby, OS vrstvu a způsob předávání
frontendových metadat (layouty, GC mapy a safepointy). Konkrétní layout
hodnot a objektů nesmí být pevně převzat z jednoho jazyka. Teprve poté se přidá
podporovaná revize a kontrola kombinace IR/runtime při linkování.


## Externí platformní služby

TLS a raw thread spawn, register snapshot/caller SP a Linux signal action,
mask a doručení jsou nyní dostupné pro Linux x86_64, arm64 a rv64.
Kontrakt struktur, vlastnictví, omezení statického ELF TLS a signal callbacků
je v [runtime/README.md](../runtime/README.md) a v hlavičkách `runtime/`.
Frontend může používat tyto moduly bez znalosti jejich syscallů či instrukcí;
musí přitom splnit zdokumentované ABI a životnost předávaných bloků.

Tyto moduly nemění `runtime_abi "none"`: stále jde o explicitně připojené
externí služby a sestavení přes assembler/linker. Neimplikují integrovaný
GC, automatické safepointy, libc-kompatibilní raw threads nebo podporu
Windows/macOS. IR `thread(symbol)` již existuje; nová služba poskytuje
inicializované TLS bloky pro hlavní i nová raw vlákna.
