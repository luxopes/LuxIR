# LuxIR

Aktuální vydání: **0.1.1**. Změny a omezení: [RELEASE-0.1.1.md](RELEASE-0.1.1.md).

LuxIR je samostatný backend překladače, podobně jako QBE. Má vlastní IR se
syntaxí ve stylu LSL a v produkci běží jako program napsaný v LSL.
Frontend (například LSL compiler) zapíše program do souboru `.lir` a LuxIR
z něj vytvoří assembler:

```text
luxir -t amd64_sysv -o program.s program.lir
```

Optimalizace a generování kódu pocházejí z QBE 1.3: SSA, alias analýza,
GVN/GCM, if-conversion, alokace registrů a ABI. LuxIR přidává vlastní vstupní
jazyk a zrychlení sdílených algoritmů backendu.
Místo QBE IL (`%x =w add %a, 1`) LuxIR čte:

```lsl
export function add1(Int32(a)) -> Int32:
    Int32(x) = add(a, 1)
    return x
end
```

Úplná specifikace je v [doc/ir.md](doc/ir.md) a příklady v
[examples/](examples/).

## Cíle

| Cíl | Platforma |
|---|---|
| `amd64_sysv` | Linux x86_64 (výchozí) |
| `amd64_apple` | macOS x86_64 |
| `amd64_win` | Windows x86_64 |
| `arm64` | Linux AArch64 |
| `arm64_apple` | macOS Apple Silicon |
| `rv64` | Linux RISC-V 64 |

Typ `Ptr` je v IR připravený pro pozdější 32bitové a 8bitové cíle.

## Struktura

| Cesta | Obsah |
|---|---|
| `src/` | C reference backendu: QBE 1.3 s čtečkou LuxIR v `parse.c` |
| `lsl/luxir.lsl` | backend v LSL, vygenerovaný z `src/` (commitnutý) |
| `lsl/luxir_*.lsl` | LSL runtime pro generovaný kód (paměť, stdio, libc, float) |
| `tools/translate.py` | převod C reference na LSL (pycparser) |
| `tools/qbe2lir.py` | převod QBE IL na LuxIR |
| `tests/` | testy jazyka, diferenční testy proti QBE, fuzzing |
| `doc/ir.md` | specifikace LuxIR |

Zdrojem pravdy je `src/`. Backend v LSL se z něj generuje, takže změny
čtečky, optimalizací i zrychlení překladače se do LSL dostanou
přegenerováním. Za běhu LSL verze nepotřebuje C, Python ani C QBE.

## Sestavení

C reference (rychlý vývojový build a reference pro testy):

```sh
make            # src/luxir
```

Backend v LSL:

```sh
make lsl                                  # přegeneruje lsl/luxir.lsl (Python 3, pycparser 3.0)
make native LSLC=/cesta/k/lslc            # build/luxir
```

`lsl/luxir.lsl` a jeho moduly mají značku `# lsl: internal-abi`. LSL
compiler je musí přeložit s interním ABI, i když leží mimo jeho strom
`compiler/pure/`. Pro reprodukci použijte LSL 0.9.1; jeho frontend i bootstrap
backend jsou součástí stejného balíku. Neupravený compiler 0.8.9 značku mimo
svůj strom nezpracuje správně a výsledný backend za běhu skončí `MemoryError`.

## Testy

```sh
make check LSLC=/cesta/k/lslc QBE=/cesta/k/qbe-1.3
```

- `tests/run_tests.py`: spustitelné programy v LuxIR (aritmetika,
  desetinná čísla, paměť a data, řízení toku, volání a ABI, varargs, env,
  TLS) s nezávisle spočítaným výstupem a 36 chybových vstupů s očekávaným
  hlášením.
- `tests/differential.py`: upstream korpus QBE 1.3 (76 programů × 6 cílů =
  456 dvojic) převedený do LuxIR. Assembler LuxIR se musí bajt po bajtu
  shodovat s C QBE 1.3 na původním IL. Funguje i pro libovolné další
  soubory `.ssa`.
- `tests/run_tests.py --target amd64_win|arm64|rv64`: stejné programy pro
  Windows (mingw-w64 a Wine) a pro arm64 a rv64 (cross gcc a qemu-user).
- `tests/upstream_exec.py`: spustitelné testy QBE 1.3 s C drivery pro ABI.
- `tests/assemble.sh`: výstup pro všech 6 cílů přeložený skutečnými
  assemblery.
- `tests/fuzz.py`: mutační fuzzing čtečky. Backend nesmí spadnout a C
  i LSL verze musí dát stejný výsledek i stejné chybové hlášení.
- `make check-dominance`: nezávislé ověření dominátorů odstraněním vrcholu
  a testem dosažitelnosti; běžný i vynucený Lengauerův–Tarjanův algoritmus.
- `tests/dominance_pipeline.py`: velké náhodné CFG přes celý překladač,
  porovnání assembleru s předchozím backendem a spuštění na hostiteli.

Aktuální úpravy algoritmů, měření a regresní ověření jsou popsány v
[benchmarks/algorithms/README.md](benchmarks/algorithms/README.md).
Následující přehled zachycuje ověření výchozího stavu před vydáním 0.1:

| Kontrola | C reference | LSL backend |
|---|---|---|
| upstream korpus, 6 cílů, shoda s C QBE | 456/456 | 456/456 |
| IL generované LSL frontendem (compiler 3,6 MB, QBE port 8,2 MB), 6 cílů | 12/12 | 12/12 |
| programy LuxIR | 6/6 | 6/6 |
| chybová hlášení | 36/36 | 36/36 |
| fuzzing, 3 000 vstupů pod ASan/UBSan, 1 500 vstupů C proti LSL | bez pádu | shoda |

Ověření podle platforem:

| Cíl | Assembler shodný s C QBE | Přeložený assemblerem | Spuštěný |
|---|---|---|---|
| `amd64_sysv` | ano | GNU as | ano: Linux x86_64, 6 programů LuxIR, 59 testů QBE |
| `amd64_win` | ano | mingw-w64 as | ano: Wine, 5 programů LuxIR (TLS se odmítne), 56 testů QBE |
| `amd64_apple` | ano | clang (x86_64-apple-macos) | ne, chybí Mac |
| `arm64` | ano | clang (aarch64-linux-gnu) | ano: qemu-user, 6 programů LuxIR, 58 testů QBE |
| `arm64_apple` | ano | clang (arm64-apple-macos) | ne, chybí Mac |
| `rv64` | ano | clang (riscv64-linux-gnu, rv64gc) | ano: qemu-user, 6 programů LuxIR, 58 testů QBE |

Testy QBE jsou upstream spustitelné testy (`tests/upstream_exec.py`)
převedené do LuxIR. Většina z nich linkuje C driver a ověřuje volací
konvenci proti skutečnému C compileru cíle. Na rv64 QBE generuje kód bez
PIC, proto se linkuje s `-no-pie`.

Překlad assemblerem zahrnuje testovací programy, příklady, celý převedený
korpus QBE a IL z LSL frontendu (`tests/assemble.sh`, `make check-cross`).
Thread-local data na `amd64_win` backend odmítne chybou, protože je QBE pro
Windows nepodporuje. Samotný backend jako program běží jen na
Linuxu x86_64, protože tam dnes existuje LSL runtime. Kód pro ostatní
platformy generuje, ale na nich se zatím nespustí.

Výchozí C reference prošla také MemorySanitizerem nad celým korpusem. IL z LSL
frontendu na rv64 končí v obou implementacích stejnou kontrolou QBE, takže
jde o existující omezení cíle, nikoli o rozdíl.

## Rychlost

Proti vydání 0.1 klesl čas překladu IR frontendu o **12,6 %** a IR backendu
o **23,7 %**, špičková RSS o **27–28 %**. Jde o sedm střídavých dvojic se
stejným vstupem a shodným výstupním assemblerem; podrobnosti a všechny vzorky
jsou v [měření algoritmů](benchmarks/algorithms/README.md).

Historické srovnání původního portu QBE a LuxIR, jiné vstupy a sestavení:
medián ze tří běhů, Linux x86_64, výstup do `/dev/null`.

| Vstup | QBE port v LSL (QBE IL) | LuxIR v LSL |
|---|---:|---:|
| IL LSL compileru (3,6 MB / 4,2 MB LuxIR) | 1,107 s | 1,093 s |
| IL QBE portu (8,2 MB / 9,5 MB LuxIR) | 3,686 s | 3,550 s |

Text LuxIR je asi o 15 % delší, přesto ho LSL backend zpracuje rychleji
než QBE port v LSL svůj vstup. Lexer čte celý soubor najednou, jméno
hashuje jednou při čtení a lokální hodnoty, bloky i symboly hledá přes
pole indexovaná číslem jména.

## Původ

Backend vychází z native LSL portu QBE 1.3 v
`LuxAI/LSL-Frontend-Backend-Test` (C zdroje, překladač a runtime moduly
z commitu `d6c9152`; větev `coordinator/qbe-declaration-scope-commit-20261010`
s commitem `e0951ad` se od něj liší jen ve frontendu). Licence:
MIT pro LuxIR a původní MIT licence QBE pro `src/`, viz [LICENSE](LICENSE).

## Externí luxrt

Experimentální runtime moduly a jejich C reference jsou v `runtime/`.
`make check-runtime` ověřuje linuxové primitivy a skutečný program bez libc.
Cross testy arm64/rv64 vyžadují cross GCC a qemu-user; viz `runtime/README.md`.
Managed ABI, stack mapy a GC zůstávají otevřené a nejsou označené jako hotové.

Self-host a limit celé kompilace: `python3 tests/selfhost.py --compiler
/cesta/lslc --compiler-cwd /cesta/k/resource-root --backend build/luxir
--limit 60`. Test ověřuje dvě další generace, identitu binárek a čas každé.


Vývojová větev nyní hlásí `0.1.2-dev`; vydaná 0.1.1 zůstává samostatný tag.
Nové matematické a SC atomické IR instrukce jsou popsány v `doc/ir.md`.

Pro raw Linux program lze už použít sestavovací driver:

```sh
python3 tools/luxir_build.py --backend build/luxir -t amd64_sysv -o program program.lir
./program
```

Frontend dodá `main(argc,argv,envp) -> Int32` (nepoužité argumenty lze vynechat).
Driver připojí raw luxrt, kernel entry, PIE relokace, RELRO a main TLS, sestaví
statický PIE bez libc/loaderu a odstraní nepoužívané funkce, data, TLS a
numerické literály. Explicitní sekce frontendu zůstanou společné. Podporuje
Linux amd64_sysv, arm64 a rv64; cross GNU binutils musejí být na PATH.
Používá externí GNU as/ld a Python při sestavení. Hotová binárka je nepotřebuje.
Nejde ještě o standardní managed runtime, libovolné libc/dlopen volání nebo
vlastní assembler/linker. Testovaný příklad je `examples/build-demo.lir`.

### Volitelné inlinování

`luxir -i input.lir` může vložit těla již definovaných malých funkcí do
volajícího. Použijte jen tehdy, když definice mají lokální vazbu: žádná
dynamická interpozice ani nahrazení symbolu jinou knihovnou. Export samo
o sobě není překážka. Bez `-i` zůstává původní překlad.

Streaming cache platí pro jeden vstupní soubor, pojme 128 funkcí; tělo
obsahuje nejvýše 8 operací, 6 scalar parametrů a jeden blok. Store,
atomiky, další call, stack allocation, aggregate/env/variadic ABI a CFG
se nevkládají. Celkové rozšíření jedné funkce je omezené na 1024 operací.
Forward calls a volání mezi moduly zůstávají běžná volání.

`tools/luxir_build.py --inline ...` zapne tuto optimalizaci pro raw static
PIE, kde linker zajišťuje lokální vazbu definic. Nevytváří automatické
GC stackmaps ani neřeší managed safepoints.

### Runtime ABI 1

`runtime_abi "1"` vybírá Linux64 explicit-root managed profil a při
sestavení připojí standardní luxrt automaticky. Missing/wrong runtime
revision je link error, včetně `--gc-sections`. Popis Value, root frames,
layoutů, statusů a size queries: [doc/abi-v1.md](doc/abi-v1.md).
Manifest scalar IR signatur: [runtime/abi-v1.json](runtime/abi-v1.json).
Frontend rootuje živé Values před alokačními calls; automatické stackmaps
a shared-heap managed threads zatím nejsou součástí tohoto profilu.

Managed lokály lze označit `Value`, funkci dát entry `gc_heap(context)`: backend
vloží precise shadow frames, aktualizuje je po definicích/Phi a uklidí při
normálním i chybovém návratu. Native PC stackmaps/STW a last-use clearing
zůstávají otevřené. `luxir --features` vypíše dostupné compiler capabilities
v JSON; podrobnosti v [doc/ir.md](doc/ir.md).
