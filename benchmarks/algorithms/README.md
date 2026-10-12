# Sdílené algoritmy po vydání 0.1

Výchozí stav je neměnné [vydání LuxIR 0.1](https://git.lux-ai.cz/LuxAI/LuxIR/releases/tag/v0.1),
commit `7480a0fe0a70ec9525cbae7363cf9da457194fce`. Bylo publikováno před úpravami
algoritmů. Srovnání používá jeho skutečnou LSL binárku, nikoli C referenci.

## Výsledek

Linux x86_64, Intel Core i5 760, LSL 0.9.1. Sedm sériových střídavých dvojic,
stejné vstupní bajty, připnutí na CPU 2, target `amd64_sysv`, výstup do
`/dev/null`. Před měřením se porovnal celý assembler bajt po bajtu.
Časy zahrnují samostatný backend; frontend, assembler a linker zde nejsou.

| Vstup | Bajtů | LuxIR 0.1 | Upravený LuxIR | Pokles času |
|---|---:|---:|---:|---:|
| IR LSL frontendu 0.9.1 | 4 181 223 | 1,6250 s | 1,4201 s | 12,6 % |
| IR původního LuxIR backendu | 9 790 262 | 5,7267 s | 4,3698 s | 23,7 % |

Čas CPU potvrzuje tentýž výsledek. Všechny vzorky, hashe a mediány jsou v
[final-native.json](results/final-native.json). Jde o pokles délky překladu;
propustnost druhého případu vzrostla přibližně 1,31×. Generované programy
mají totožný assembler, jejich výkon se touto změnou nemění.

Samostatné procesy měřené pomocí Linux `RUSAGE_CHILDREN`, největší RSS procesu:

| Vstup | LuxIR 0.1 | Upravený LuxIR | Pokles |
|---|---:|---:|---:|
| IR frontendu | 224,0 MiB | 163,0 MiB | 27,2 % |
| IR backendu | 823,5 MiB | 589,2 MiB | 28,5 % |

Paměťové vzorky jsou v `results/final-*-rss-*.json`. Měření nepoužívá
kumulativní maximum několika předchozích překladů: pracovní proces se vždy
nově forkuje. Uživatelské procesy se při měření nevypínaly.

## Změny

- `cfg.c`: velké CFG používají Lengauerův–Tarjanův výpočet dominátorů
  s iterativní kompresí cest. Malé grafy si ponechávají původní algoritmus.
  Soukromé DFS číslování nemění RPO, pořadí bloků ani rozhodování backendu.
- `cfg.c`, `all.h`: intervaly podstromů dominátorů umožňují dotaz na dominanci
  v konstantním čase. Dva nové 32bitové údaje jsou na konci `Blk`.
- `spill.c`: pomocná množina pro rozdělení registrových tříd se alokuje jednou
  za funkci a při každém použití zcela přepíše. Rozdělení obou tříd probíhá
  jedním průchodem, rozvinutým po čtyřech slovech.
- `util.c`: počítání bitů přeskočí nulová slova řídkých množin.
- `tools/translate.py`: odstraní instrukce po terminátoru do dalšího návěští
  a skok těsně před svým cílovým návěštím. LSL frontend již stejné instrukce
  ignoruje. Zdroj má 57 997 místo 62 308 řádků; celé výsledné IR i ELF se
  shodují s verzí před touto úpravou.

Všechny změny jsou společné šesti targetům. Přímá práce s pamětí a kompaktní
reprezentace zůstávají zachované. `lsl/luxir.lsl` se přegeneroval ze zdrojů;
za běhu není potřeba C ani Python.

Čtyři kontrolní hashe v generátoru se změnily pouze kvůli posunu čísel řádků
v asercích za upraveným `bscount`. Nezávislé porovnání normalizovaných těl
ověřilo, že se žádná jiná část těchto čtyř funkcí nezměnila:
[pin-audit.json](results/pin-audit.json).

## Ověření

- 6 435 072 kontrol dominance proti nezávislému odstranění vrcholu a BFS;
  běžný práh i vynucený nový algoritmus, včetně cyklů a dlouhého řetězce.
- 12 384 porovnání počtu bitů proti nezávislému počítání každého bitu.
- 456/456 dvojic vstup/target proti QBE 1.3 pro C i LSL sestavu.
- 6 spustitelných programů a 36 diagnostických vstupů pro každou sestavu.
- 300 mutací vstupu, shodný výsledek C a LSL, žádný pád.
- 216 porovnání assembleru velkých CFG proti původní C referenci a 36
  skutečných spuštění; hranice 63/64/65 bloků, až 1 025 bloků, zpětné hrany.
- 59/59 upstream spustitelných testů na Linux x86_64.

Výpis: [check.log](results/check.log). Generování po odstranění redundantních
skoků vytvořilo **identickou binárku**, takže se uvedené výsledky a měření
vztahují i na konečný zdroj. Hashe jsou v
[final-manifest.json](results/final-manifest.json).

### Self-host

Nemodifikovaný frontend LSL 0.9.1 přeložil `lsl/luxir.lsl` za použití
samotného nového LSL backendu. Následující generace pak sestavila další.

| Generace | Backend použitý při sestavení | Celý překlad |
|---|---|---:|
| stage 2 | nový LuxIR stage 1, přímo | 58,80 s |
| stage 3 | nový LuxIR stage 2, přímo | 59,03 s |

Tyto časy zahrnují frontend, backend, assembler i linker, na CPU 2.
Nejde o slib stejného času na jiném stroji nebo při jiném zatížení.
Stage 1, 2 a 3 mají shodné bajty a SHA-256
`cfa92a61e11df38dfa8397428a6375ba370d2742246f3851635d2a4b23c93adb`.
Ověření: [selfhost-proof.json](results/selfhost-proof.json), časové vzorky
`results/selfhost-stage*.json`. Při překladu se nepoužívá C QBE ani C compiler;
pro sestavení ELF jsou použity GNU assembler a linker.

## Reprodukce

Použijte LSL 0.9.1 a pycparser 3.0. Nejprve sestavte novou binárku a jako
`BASELINE` zadejte binárku z vydání 0.1. Oba backendy musí dostat stejné IR:

```sh
make lsl
make native LSLC=/cesta/k/lsl-0.9.1/bin/lslc
make check LSLC=/cesta/k/lsl-0.9.1/bin/lslc QBE=/cesta/k/qbe-1.3
python3 tools/performance/compare.py --baseline "$BASELINE" \
  --candidate build/luxir --input frontend.lir --input backend.lir \
  --repeats 7 --report build/comparison.json
python3 tools/performance/resources.py --report build/memory.json -- \
  build/luxir -o /dev/null backend.lir
python3 tests/dominance_pipeline.py --baseline "$BASELINE" --backend build/luxir
```

`tools/performance/capture.py --output capture-backend --ir frontend.lir
--backend "$BASELINE"` vytvoří spustitelný wrapper, který lze předat jako
třetí argument `lslc SOURCE OUTPUT BACKEND`. Stejně zachyťte IR původního
`v0.1/lsl/luxir.lsl`. Použijte vždy tentýž lokální zachycený soubor pro obě
varianty. Absolutní cesty v diagnostice mohou mezi stroji změnit hash IR.

`tools/performance/profile.py` připraví izolovanou instrumentovanou kopii
backendu; produkci nepřepisuje. Její časovače měří CPU jednotlivého vlákna.
Prvotní profily v `results/baseline-profile-*.txt` sloužily k výběru úprav,
nejsou hlavními benchmarky.

## Vyřazené experimenty

Řazení spill kandidátů pomocí předpočítaných klíčů nepřineslo zrychlení celého
překladu. Ani index podmíněných dominátorů v GVN neprokázal přínos. Obě úpravy
byly vráceny. Skalární sloučení bitset průchodů bylo přibližně o 1 % pomalejší;
ponechána je až varianta s uloženými ukazateli a rozvinutím po čtyřech slovech.
Surová měření těchto pokusů jsou přiložena, jejich kód není součástí změny.
