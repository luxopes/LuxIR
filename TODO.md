# LuxIR TODO

Cíl: frontend (LSL) řeší jen jazyk a zapíše jednotný LuxIR. Vše strojové a
platformní řeší LuxIR: optimalizace, generování kódu, runtime jazyka,
přístup k operačnímu systému, formát binárky a linkování. Jeden `.lir` z LSL
pak dá spustitelný program pro Linux, Windows, macOS i Android.
Navazující plán frontendu: sekce „Plán 0.10“ v `TODO.md` repozitáře
LuxAI/LSL.

Zdrojem pravdy zůstává C reference v `src/`; `lsl/luxir.lsl` se přegeneruje
(`make lsl`). Každá položka platí až s testy v obou verzích (C i LSL dají
stejný výstup) a s měřením: programy LSL přeložené přes LuxIR nesmí být
pomalejší než předchozí vydání LSL, při překladu ani za běhu.

## Rozhraní pro frontendy

- [ ] Verzovat IR a ABI runtime knihovny (`luxir --version`, verze v hlavičce
  `.lir`); nekompatibilní kombinace odmítnout srozumitelnou chybou.
- [ ] Popis layoutu objektů frontendu: ukazatelové mapy typů, kořeny,
  safepointy. Frontend řekne *co* je ukazatel, LuxIR rozhodne *jak* GC
  prochází zásobník a registry (stack mapy).
- [ ] „Volání, které může selhat“: chybová cesta z volání (landing pad nebo
  druhá návratová hodnota), aby frontend negeneroval kontrolu příznaku po
  každém volání.
- [ ] Kontrolovaná aritmetika jako instrukce (přetečení Int, dělení nulou,
  kontrola mezí) s cílem chyby; backend kontroly slučuje a odstraňuje.
- [ ] Dokumentovat celé rozhraní v `doc/runtime.md` vedle `doc/ir.md`.

## Výstup objektů a linkování (bez binutils)

- [ ] Vlastní assembler do objektů: x86_64 ELF64, pak COFF (Windows),
  Mach-O (x86_64 a arm64), arm64 a rv64 ELF.
- [ ] Vlastní linker:
  - statický PIE ELF se stejnými zárukami jako dnešní `ld` v LSL
    (`-static -pie`, `-z text`, `-z noexecstack`, bez build-id, strip);
  - PE32+ s ASLR, DEP a high-entropy VA, importy `kernel32` a `ws2_32`;
  - Mach-O s `LC_MAIN`, `libSystem` a ad-hoc podpisem (na arm64 povinný).
- [ ] `luxir -t <cíl> -o program program.lir runtime.lir …`: od `.lir` po
  spustitelný soubor jedním příkazem, bajtově deterministicky.
- [ ] Odstranění mrtvého kódu celého obrazu včetně runtime (dnes se
  `runtime.o` linkuje celý; tabulka mocnin pro Float zvětšila v LSL 0.9.2
  každou binárku o ~12 KB).
- [ ] Akceptace: linuxové binárky LSL bez `as` a `ld` projdou branou
  hardeningu obrazu v release preflightu LSL a celou sadou testů.

## Runtime knihovna LuxIR (luxrt)

### OS vrstva: jediné místo s kódem pro konkrétní systém
- [ ] Paměť: map, unmap, protect, velikost stránky.
- [ ] Soubory a adresáře: open, read, write, seek, close, stat, list,
  rename, unlink, mkdir, chmod, symlink, cesta k programu.
- [ ] Procesy: spawn s argv, env a pipes; wait, exit, kill; zavírání
  zděděných deskriptorů.
- [ ] Vlákna: spawn a join, čekání na adrese (futex / WaitOnAddress /
  ulock), TLS.
- [ ] Čas, spánek, OS náhoda, proměnné prostředí, argumenty, pracovní adresář.
- [ ] Sockety TCP, UDP a unix; síťové chyby.
- [ ] Terminál: isatty, raw režim, velikost okna, Ctrl-C.
- [ ] Jednotný chybový model (kód a text) pro všechny systémy.

Implementace OS vrstvy:
- [ ] Linux přes systémová volání: x86_64, arm64, rv64.
- [ ] Windows přes Win32: `kernel32`, `ws2_32`, `bcrypt` pro náhodu.
- [ ] macOS přes `libSystem`: systémová volání macOS nejsou stabilní ABI.
- [ ] Android: Linux ARM64 se zohledněním omezení (seccomp, chybějící `/etc`,
  Termux i APK).

### Obecný runtime jazyka: převzít to, co dnes LSL generuje ručně ve strojovém kódu x86_64
- [ ] Alokátor a přesný GC nad stack mapami (viz Rozhraní), safepointy a
  rozpočet alokací; vícevláknový běh.
- [ ] Číselné převody:
  - Int ↔ text;
  - Float ← text (Clinger + Eisel-Lemire + přesné porovnání dlouhých vstupů,
    převzít z LSL 0.9.2);
  - Float → text (nejkratší a nejbližší zápis shodný s `repr` v Pythonu).
- [ ] Paměťové a řetězcové primitivy: memcpy, memmove, memcmp, hledání bajtů,
  UTF-8, hash; vektorizace tam, kde ji cíl umí.
- [ ] Atomiky a fence jako instrukce pro všechny cíle.
- [ ] Výpis neodchycených chyb a ukončení programu (dnes runtime LSL).

## Výkon runtime (benchmark „autobusy“, 12. 10. 2026)

Program: dosažitelnost v grafu ~100 000 zastávek a ~780 000 hran, 150 dotazů
DFS, zastávky jako řetězcové klíče slovníku (`~/autobus.lsl`, vstup
`04.in`). C se stejným algoritmem 0,33 s, Python 8,8 s, LSL 0.9.2 19 s
(16,4 s s úpravou indexu slovníku níže). Stejný algoritmus v LSL s čísly
zastávek a `IntArray` místo slovníků: 1,48 s. Cíl: řetězcová verze nejvýš 3 s,
číselná nejvýš 0,6 s.

- [ ] **Tagované hodnoty:** hodnoty Any dnes nenesou tag, takže runtime
  každou hodnotu ověřuje dotazem do globální hashovací tabulky objektů
  (`runtime_is_heap_object`). To je výpadek cache při každém hashi klíče,
  porovnání i kontrole typu: jedna lookup ve slovníku ~90–120 ns, z toho
  většina na tuto tabulku. Navrhnout reprezentaci s tagem (tagované ukazatele
  nebo malé Int v tagu), aby test „je objekt / je String“ byl test bitu.
  Patří do kontraktu s frontendem (layout hodnot) a do GC.
  Přijetí: lookup slovníku s řetězcovým klíčem nejvýš 40 ns.
- [ ] **Hash v řetězci:** ukládat hash do hlavičky String (spočítá se jednou),
  porovnávat nejdřív ukazatel, délku a hash, teprve pak bajty.
- [ ] **Slovník v runtime:** kompaktní slovník se zachovaným pořadím vkládání
  (jako CPython). V položce indexu uložený hash a druh klíče; při růstu
  přesun položek bez čtení klíčů; jedno hashování při vložení. LSL 0.9.2 to
  po vydání doplnilo do svého runtime (`heap_runtime_linux_x86_64.lsl`,
  `runtime_dict_probe`, `runtime_dict_index_insert`, `runtime_dict_grow`,
  `runtime_dict_set`) a slouží to jako reference. Malé slovníky bez indexu
  zatím porovnávají lineárně přes `runtime_equal_any`.
- [ ] **Množiny a sety:** stejné principy jako slovník (`set`, `in` nad Set).
- [ ] **split a malé řetězce:** `split` vytváří nový objekt pro každý token.
  Zvážit internování krátkých řetězců nebo řezy bez kopie, aby opakované
  klíče sdílely objekt a porovnání skončilo na shodě ukazatelů.
- [ ] **Benchmarková sada:** přidat „autobusy“ (řetězcovou i číselnou verzi)
  a mikrobenchmarky slovníku (vložení, `in` při zásahu i minutí, čtení,
  vkládání do nového slovníku, append/pop na seznamu) s referencí v C a
  Pythonu; měřit při každé změně runtime.

## Optimalizace (převzít z frontendu a doplnit)

- [ ] Inlinování malých funkcí (listové funkce, tělo jeden výraz). Benchmark
  LSL `function_calls` je v 0.9.2 omezen latencí výpočtu; zbývá ~1 takt volání
  na iteraci.
- [ ] Smyčky: kanonická indukční proměnná a zjednodušení podmínky konce. Má to
  být obecná náhrada podmínky `rang` s literálovým krokem, kterou dnes dělá
  `qbe_emit` v LSL. Dále LICM, strength reduction a odstranění kontrol mezí.
- [ ] Propagace konstant a specializace přes volání (dnes guarded
  specializace ve frontendu LSL), odstranění mrtvých větví.
- [ ] Ke každé optimalizaci: diferenční test proti verzi bez ní a benchmark LSL.

## Cíle a testy

- [ ] `amd64_win`: TLS (dnes se v Wine testech odmítá), velké rámce zásobníku
  (`__chkstk`), SEH tabulky pro výpis chyb.
- [ ] `amd64_apple` a `arm64_apple`: poprvé spustit na skutečném Macu (dnes
  jen ověřený assembler; testy spustí uživatel).
- [ ] CI matice: Linux x86_64 nativně, arm64 a rv64 v qemu-user, Windows ve
  Wine a před vydáním na skutečném Windows, macOS na skutečném Macu.
- [ ] Self-host LuxIR na každém cíli: `luxir` přeložený LSL a luxrt pro daný
  systém, bajtově stejný výstup `.s` jako na Linuxu.
- [ ] Výkonové měření backendu: čas překladu LSL compileru přes LuxIR
  nesmí růst s přidanými optimalizacemi bez vyrovnání jinde.
