# Release 0.1.1 — finální binárka

`luxir --version`: `LuxIR 0.1.1 (IR 1; runtime ABI none)`.
Finální bootstrap 49.766 s, self-host stage 2 52.864 s,
stage 3 52.057 s; oba self-host překlady pod limitem
60 s a všechny tři binárky bajtově stejné. SHA-256:
`0b64ac50e0650858e344b9261d80b9ef46a446c1975f625afd0ec2a18eb34d02`.
Finální `make check` a `make check-runtime` prošly; runtime execution
arm64/rv64 v QEMU znovu prošel v C i LSL variantě.
Použit existující frontend LSL 0.9.1 bez změn. Raw záznamy:
`validation/release-0.1.1/`. Starší časy a hashe níže popisují dřívější checkpointy.

# Aktuální checkpoint LuxIR — Linux runtime, rv64 PIE a self-host

Stav 12. 10. 2026, pracovní větev `work/luxir-todo`. Navazuje na remote main
`ade2519` a větev `perf/algorithms-after-0.1` (`8b9bca5`). Změny z původního
archivu byly přeneseny do Git checkoutu a LSL znovu přegenerováno z C.
**Celé TODO není dokončeno**; úzké implementované části jsou v TODO explicitní.
Zdroj LSL frontendu nebyl měněn.

- 462/462 C funkcí přegenerováno, žádná blokovaná.
- `make check`: 108 protokolových kontrol, 6 host programů + 36 chybových
  vstupů na každý backend, 456/456 QBE dvojic pro C i LSL, 300 fuzz případů
  bez internal error. 6 435 072 nezávislých kontrol dominance a 12 384 bitset
  kontrol. Nový/old CFG pipeline: 216 shod assembleru, 36 skutečných spuštění.
- `make check-runtime`: všechna host runtime provedení úspěšná. Nově 138 OS
  kontrol, 10 009 startup/environment kontrol, 33 full I/O kontrol včetně
  skutečného EINTR, 2 032 atomic kontrol a 256 thread kontrol (40 vláken,
  4 000 000 inkrementů, guarded stacky).
- Cross GCC + qemu-user 8.2.2: paměť/UTF-8 (235 296), Int64 (3 800 329),
  checked aritmetika (800 512), startup (10 009), OS (27), full I/O (33) a
  atomiky (2 032) prošly na **arm64 i rv64**. C reference a LSL-generovaný
  runtime mají stejný výstup; assembler obou backendů shodný. 6 původních
  programů a 36 error vstupů prošlo také pro oba cross cíle a oba backendy.
- Skutečné kernel entry, argv/env/auxv: libc-free static PIE bez interpreteru
  běží na všech třech Linux cílech, obě backendové verze dají stejné bajty.
- rv64 `-p`: regresní test lokálních adres + offset a externích adres,
  statický PIE bez runtime relokací běží v QEMU. Volba nenahrazuje datový
  relokátor pro pointerové konstanty ani hotový dynamický/shared linker.
- Clang 18/GNU as skutečně assemblovaly 59 povolených kombinací demo/test
  a target, 0 chyb. Windows TLS je očekávaně odmítnuto. Windows/macOS nebyly
  spuštěny a jejich runtime zde není implementovaný.

## Self-host a výkon

Stage 2: **48,269 s**; stage 3: **46,155 s**. Každý zahrnuje frontend,
backend, GNU assembler a linker. Obě generace splnily explicitní limit 60 s
v tomto prostředí. Stage 1 == stage 2 == stage 3, SHA-256:
`b8dd50c149d1592fc49260f33303b05802570bed04dfdd5199f649e89e5e3c53`.
Test `tests/selfhost.py` při timeoutu ukončí celou procesovou skupinu a nechá
logy. Jde o Linux x86_64 a použitý LSL 0.9.1 compiler, nikoli cross self-host
ani časový slib pro jiné stroje. Kompilátor je nástrojový vstup, nebyl upraven.

15 střídavých připnutých párů nad 1000 × 40 instrukcemi, stejné bajty
assembleru: baseline 93,377 ms, kombinovaný backend 93,575 ms (rozdíl 0,21 %,
v mezích šumu). Tento malý vstup nepotvrzuje nové zrychlení ani celkovou
výkonovou bránu LSL. Dřívější velká měření větve perf jsou zachovaná v
`benchmarks/algorithms/`, nebyla vydávána za nové měření zdejšího hostu.

Runtime mikrobenchmark, 9 střídavých párů × 1 000 000 volání, C reference
vs native: hash 52,179/53,743 ms, UTF-8 41,170/24,039 ms, Int64 formatter
32,843/42,217 ms. Formatter je nadále pomalejší než C; výkonové TODO není
odškrtnuto. Autobus/Any/Dict benchmark ani výkonový kontrakt managed LSL se
z těchto měření nedokazuje.

Raw JSON: `validation/2026-10-12-linux/`. Skript `tools/check_linux.sh`
sjednocuje místní Linux matici. Nový Forgejo C-reference workflow má validní
YAML, **remote CI job zatím neběžel**, nepředstírá C/LSL self-host v CI.

Zůstává: managed ABI a GC se stack mapami, tagované Any a kontejnery, Float
převody, checked/atomic IR instrukce a landing pads, nový interprocedurální
inliner/optimalizace, vlastní assembler/linker, plná OS/TLS/signálová vrstva,
Windows/macOS/Android runtime a akceptační testy. Tyto části nejsou hotové.

---

## Historie předchozího archivového checkpointu

# Ověření verzování LuxIR — 12. 10. 2026

Rozsah: samostatný `luxir --version` (také -V), volitelné hlavičky
`ir_version "1"` a `runtime_abi "none"`. C reference změněna v src/, LSL
backend přegenerován `make lsl` (459/459 funkcí, žádná blokovaná).
Starší vstupy bez hlaviček zůstávají kompatibilní. Header-only vstup má
stejný výstup jako prázdné legacy IR (včetně GNU-stack metadata).
Čtečka zpracovává assembler průběžně: chyba pozdní hlavičky může mít na
stdout již emitované předchozí definice. Návratový kód je neúspěšný;
spotřebitel musí odmítnout výstup při neúspěšném překladu.

## Provedené kontroly

- 108 protokolových kontrol v C i LSL, přes šest cílových backendů:
  nezměněný assembler, neznámé verze, duplicitní/pozdní hlavičky, malformed
  syntax, stdin a reset stavu mezi více soubory; diagnostika bajtově shodná.
- Původní testy: šest nativně spuštěných programů a 36 chybových vstupů
  v každém backendu; všechny prošly.
- Upstream QBE 1.3: 456 kombinací vstup/cíl pro C a 456 pro LSL,
  žádný rozdíl proti C QBE. Viz differential-*.json.
- 300 mutačních fuzz případů: C/LSL shoda, žádný internal error nebo pád.
- Protokolové testy zopakovány i na selfhostované binárce LuxIR.
- LSL frontend přeložen novým backendem za 5.778 s: byte-identický
  s dříve ověřenou frontend generací, SHA256
  dfe09af872905d37b970770924f2442645ccf902fca889611935979fe1766019.
- LuxIR bootstrap 45.521 s, vlastní selfhost 45.369 s; obě binárky
  byte-identické, SHA256
  30c5e435de0c6a11220423bea83dac9f5f6dc66c3726a82bc7b169a5565fd83d.

Použit frontend LSL 0.9.1; protože checkoutový compiler hledá některé
resources podle cwd, kompilace proběhla z jeho kořene s absolutní cestou
ke zdroji LuxIR a backendu. Paměťový limit 4 GiB, timeout 120 s.
Selfhost LuxIR peak RSS přibližně 1.01 GiB. Novější LSL 0.9.2 v této
izolované práci nebylo dostupné.

## Výkon

Deterministický test 1000 funkcí po 40 instrukcích, vstup 1 235 890 bajtů;
proces připnut na CPU 0. Výstup je byte-identický proti baseline.
Benchmark měří celou invokaci včetně spuštění procesu a zachycení assembleru.
LSL baseline je dodaná binárka LSL 0.9.1/LuxIR; původní generovaný zdroj
archivu je byte-identický s dříve ověřeným LuxIR commitem 7480a0f.
C baseline je původní src/ z přiloženého archivu, sestavené stejným make.

| Varianta | Baseline medián | Nový medián | Počet měření každé |
|---|---:|---:|---:|
| LSL backend | 103.928 ms | 100.472 ms | 11 |
| C reference, kontrolní běh | 37.720 ms | 37.136 ms | 51 |

První 11běhový C test ukázal +3.5 %, proto byl proveden delší kontrolní
běh, v němž se zpomalení nepotvrdilo. Host je sdílený/virtualizovaný;
malé rozdíly nejsou příslibem obecného zrychlení. Raw výsledky obou běhů
jsou zachovány. Runtime testovaných programů se nemění: assembler je
bajtově shodný. To nenahrazuje budoucí benchmark autobusy s LSL 0.9.2.

## Reprodukce

```sh
make lsl
make
make native LSLC=/cesta/k/lslc LSLC_BACKEND=/cesta/k/luxir
python3 tests/versioning.py --backend src/luxir --backend build/luxir
python3 tests/run_tests.py --backend src/luxir --backend build/luxir
python3 tests/differential.py --reference /cesta/k/qbe --backend build/luxir
python3 tests/fuzz.py --backend build/luxir --compare src/luxir --cases 300
```

Benchmark: `tests/version_benchmark.py --baseline OLD --backend NEW --report RESULT`.
Přiložené `bin/luxir-linux-x86_64` je ověřená native LSL binárka (SHA výše);
po rozbalení použijte `chmod +x bin/luxir-linux-x86_64`, pokud nástroj
nezachová executable bit. `changes.patch` obsahuje změny zdrojů a dokumentace.

## Zbývající rozsah

Číslované luxrt ABI, layout/GC kontrakt a ověřování kompatibility externího
runtime při linkování nejsou implementovány. `none` to výslovně uvádí.
TODO proto rozděluje dokončené IR verzování a zbývající runtime práci.
Pro šest cílů byla ověřena shoda generovaného assembleru; programy byly
spuštěny pouze na Linux x86_64. Změny nebyly pushnuty na remote.

# Pokračování runtime práce — 12. 10. 2026

Celé TODO není dokončeno. Přibyly skutečně spustitelné externí runtime moduly,
C reference a společná brána `make check-runtime`; managed ABI/GC, frontendová
integrace, vlastní assembler/linker a ostatní OS stále zůstávají otevřené.

- Paměť/UTF-8/hash: 235 296 kontrol v každém provedení.
- Int64 decimal parse/format: 3 800 329 kontrol v každém provedení.
- Checked add/sub/mul/floor-divide: 800 512 kontrol proti 128bitové referenci.
- Linux x86_64 syscalls: 27 kontrol v každém provedení.
- Memory/integer/checked assembler se bajtově shoduje mezi C a LSL backendem
  pro šest cílů. Execution byl ověřen na Linux x86_64.
- C reference memory/integer/checked prošly ASan/UBSan; LeakSanitizer
  je v hostu nedostupný. Native memory testy mají canaries a guard pages.
- Standalone program bez libc a ELF loaderu: statický PIE s nonexecutable
  stackem; C/LSL backendy vytvoří identickou binárku. Použito GNU as/ld.

## Mikrobenchmark runtime

Mediány devíti běhů po milionu volání, CPU affinity 0, GCC C reference
-O2 bez LTO versus skutečný LuxIR-generovaný assembler. Vstupy 64 ASCII bajtů
pro hash/UTF-8; Int64.MIN + i pro format. Časy níže zahrnují volání helperu,
nikoli vytváření LSL String objektů nebo GC. Kontrolován shodný checksum.

| Helper | C reference | Native LuxIR |
|---|---:|---:|
| hash | 57.474 ms | 55.031 ms |
| utf8 | 42.927 ms | 23.762 ms |
| format | 33.676 ms | 41.864 ms |

Formatter byl opraven z dvou dělení na jednu číslici na párové zpracování
číslic. První test měl ještě dispatch uvnitř smyčky a slouží pouze jako
diagnostika; kontrolní a konečný běh mají dispatch vně smyčky. Finální
formatter je stále pomalejší než optimalizovaná C reference. Tyto výsledky
netvrdí splnění výkonové brány proti LSL 0.9.2 ani benchmarku autobusů: chybí
aktuální frontend, původní autobus.lsl a 04.in. Moduly proto nejsou
automaticky nasazeny jako produkční runtime a široké TODO položky nebyly
zaškrtnuty.

## Konkrétní neověřené závislosti

Aktuální Git server znovu vrací 502 i pro LSL. K dispozici je pouze starší
frontend 0.9.1; požadovaný runtime/layout z 0.9.2 a plán 0.10 nebyly dodány.
Chybí také autobus.lsl/04.in. Host neposkytuje clang, qemu-aarch64,
qemu-riscv64 ani Wine; skutečný macOS/Windows není dostupný. To je blokér
konkrétní platformní/integrační akceptace, nikoli tvrzení, že zbývající
implementace už existují. Přesný GC, kontejnery/tagované hodnoty, Float
převody, assembler/linker a optimalizační průchody stále vyžadují vývoj.

## Další soutěžní benchmark — opery

Viz `benchmarks/contest/README.md` a `results/final.json`: 10 milionů Int64
prvků, C/LSL assembler shodný pro šest targetů, 14 correctness kontrol.
Numeric 4,622 ms C -O2 / 6,409 ms LuxIR, text 47,753 / 65,903 ms.
Experimentální loop layout byl vyřazen pro neprokázaný přínos. Ověřený
backend a self-host hash se nezměnily; tento commit přidává benchmark a TODO.

## Oddělené ELF sekce — 12. 10. 2026

Backend `-s` a driver vyřazují nepoužitá data, BSS, TLS a numerické
literály podle relokací GNU linkeru. `tests/data_sections.py`: 36 kontrol
pro C a LSL na každý Linux cíl, včetně callbacku uchovaného pointerem,
transitivních dat, mrtvého pointerového cyklu, offsetů, běžného BSS a TLS
inicializace/BSS. Explicitní sekce sdílí životnost a zůstávají společné.
Kontrolní x86-64 obraz: 120 120 → 13 576 bajtů. ARM64 a RV64 skutečně
spuštěny v QEMU; C/LSL binárky shodné. Dosavadních 30 build-driver kontrol
na každý cíl také prošlo. Bez `-s` výstup dosavadního demo obrazu nezměněn.

Obnovený toolchain: LSL 0.9.1 release compiler, 465/465 generovaných funkcí.
Bootstrap 65,60 s. Povinný selfhost test s limitem 60 s skončil timeoutem;
diagnosticky s limitem 90 s dokončeny dvě generace za 64,42 a 69,51 s,
obě shodné se stage 1 (SHA256
`1e5308aca51dfaea18c76636799a0513e1bd741ee392f814157dc666a4fdb3df`).
Minutová brána v tomto prostředí tedy **neprošla**; výsledky staršího
prostředí nejsou důkazem aktuálního výkonu. Protokol 108 kontrol a všechny
základní programy/72 negativních případů C+LSL prošly.

## Raw arena allocator — 12. 10. 2026

`runtime/arena.c/.h/.lir`: 24bajtový caller-owned descriptor, init s
kontrolou rozsahu, bump allocation se zarovnáním a kontrolou kapacity,
rewind. Chyby zachovají descriptor i output; payload se neinicializuje.
Nejde o managed allocator, GC, mapování paměti ani thread-safe arénu.

Nezávislý 128bitový C oracle: 1 213 384 kontrol pro C referenci, C-emitted
a LSL-emitted IR, plus C ASan/UBSan. Assembler C/LSL shodný pro šest
cílů. 256 dalších scénářů s Pythonem vypočtenými offsety spuštěno jako
raw static PIE na třech Linux cílech (ARM64/RV64 QEMU), binárky C/LSL
shodné. Driver připojuje arena modul automaticky; nepoužitý modul je
vyřazen a dosavadní demo obraz zůstává bajtově stejný.

Izolovaný x86-64 benchmark: 16 milionů alokací po 16 bajtech na trial,
7 měřených trialů a warm-up; obsahuje call, kontroly a pointer checksum,
nezahrnuje OS mapování, inicializaci objektů, GC nebo synchronizaci.
- C reference: medián 2.165 ns/alokaci.
- src/luxir: medián 3.264 ns/alokaci.
- build/luxir: medián 3.074 ns/alokaci.

Čísla nejsou měřením managed objektů ani end-to-end soutěžního programu.
C/LSL emitory generují stejný kód; rozdíl jejich samostatně měřených
trialů proto není rozdíl optimalizace backendu.

Kontrola staršího commitu `12d3bb3` v obnoveném prostředí: znovu
vytvořená binárka má původní SHA256 `2f77ea40…41f368d`; bootstrap
77,20 s, selfhost stage 2 timeout 90 s. To nepotvrzuje relativní
zrychlení nové změny, ale ukazuje, že starší minutový výsledek nelze
přenést do současného prostředí.

Profilovaný aktuální překlad: 63,7588 s celkem, samotný native LuxIR
backend 3,6696 s (včetně spuštění wrapperu), úspěšná kompilace. Zbytek
je frontend, generování IR, as/ld a driver; jednotlivé složky zbytku
tímto měřením nejsou rozdělené. Výstup ověřen jako shodný s výchozí
binárkou. Aktuální limit 60 s nesplněn.

## Managed core a extern call — 12. 10. 2026

1 281 292 kontrol pro C referenci a pro IR přeložený C/LSL backendem;
nezávislý BFS model určuje živé objekty 32 náhodných grafů po 1 024 uzlech,
včetně cyklů. Int64/Float64 roundtrips, 30 000-node chain, malformed
fixed/tail pointer maps, foreign heap, duplicate/LIFO frames, map/unmap
failures, nulování recyklovaných payloadů a skutečné uvolnění všech
mapování při destroy. C ASan/UBSan prošly; LeakSanitizer nepoužit.
Standalone smoke s raw Linux mmap/munmap běží na x86-64 a v ARM64/RV64
QEMU; C/LSL binárky shodné. Assembler managed modulu shodný pro šest cílů.
Jde o single-owner precise explicit-root GC, nikoli produkční multithread
GC nebo automatické stack mapy.

Při C → IR převodu nalezena mezera: `qbe2lir` i LuxIR parser neuměly
`call extern symbol`. Opraveny value/void cally, samostatný execution test
`tests/qbe2lir_extern.py`. C reference stále zdroj pravdy pro backend;
465/465 LSL funkcí přegenerováno, bootstrap nového native backendu 63,29 s.

## Managed collections — 12. 10. 2026

799 669 kontrol pro C referenci a C/LSL-generated IR, ASan/UBSan C;
random list a tři typed-array modely, 100 000 map operací s nezávislým
pořadím, 10 000 vložení nově alokovaných shodných String keys, GC stress,
UTF-8 (včetně NUL a 4-byte scalar), slice/concat/split, float bit fidelity,
NaN identity, numeric-key policy a typed Bool/Int rozlišení, Set.
Standalone contest-like smoke (10 000 numeric array items a opakované
map updates/deletes/iteration) běží na třech Linux cílech; C/LSL binárky
shodné. Všechna mapování uvolněna při destroy.

Lookup benchmark: 4096 distinct-but-equal cached String keys, milion
lookupů/trial, 7 trialů + warm-up, checksum, alokace/GC mimo timing.
Původní implementace: native 75,32 ns, C 55,63 ns. Po přímém cached hash
loadu, jednorázovém avalanche při konstrukci String a odstranění
opakovaných type/coercion/out-parameter helperů: native 27,19 ns,
C-emitted IR 29,14 ns, C reference 25,18 ns. Nejde o měření celé LSL
varianta autobusů nebo srovnání s předchozím LSL release.
