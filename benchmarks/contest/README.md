# Soutěžní workloady: opery, výpovědi, autobusový graf

Aktuálně skutečně implementován a měřen **opera** workload; vražda a autobusový
graf nejsou tímto testem měřené. Chybí uživatelův konkrétní LSL/C kód a data.
Zadání opery má ve slovním výpočtu překlep: správně 25 + (-2) + 95 = 118.

## Metodika

100 000 řádků × 100 hodnot, 10 000 000 Int64 hodnot -100..100. Deterministický
xorshift, každý 31. řádek pouze záporný. Numeric kernel: součet řádkových maxim.
Text kernel: stejná data, 34,295,316 bajtů, streaming bounded parser +
stejný výpočet. Header N/M, příprava dat, alokace driveru, čtení disku a výpis
jsou mimo čas. Žádné objekty LSL, split, Any, Dict ani GC. Int64 v C i LuxIR.
Jde o kontrolované backendové kernels, **nikoli celý LSL program**.

CPU 0, sedm střídavých sériových měření každé varianty. Každý vzorek
průměruje 16 numeric / 4 text průchody, tabulka uvádí medián času na průchod.
Driver se vždy kompiluje -O2 odděleně; C kernel -O2/-O3/-O3 -march=native,
LuxIR kernel C i LSL backendem (stejný assembler pro všech šest cílů). Bez LTO.
Native varianta C používá instrukce zdejšího CPU, není skrytě součástí -O2.

| Kernel | C -O2 | C -O3 | C -O3-native | LuxIR |
|---|---:|---:|---:|---:|
| Int64 maxima | 4.622 ms | 4.512 ms | 3.416 ms | 6.409 ms |
| Text parse + maxima | 47.753 ms | 46.992 ms | 46.822 ms | 65.903 ms |

LuxIR/C-O2: numeric 1.387×, text 1.380×.
Disassembly v JSON: C -O2 rozvíjí numeric loop po dvou, LuxIR scalar loop
má na iteraci conditional exit + unconditional backedge. C-native používá
AVX-512 vpmaxsq, LuxIR scalar cmov. To potvrzuje rezervu v unroll/SIMD,
ale nedokazuje, že tyto dvě věci vysvětlují zpomalení celého LSL programu.

14 correctness kontrol (ukázka 118, záporné řádky, 0 řádků, chybný bounded
vstup), plus checksum 9 523 478 na velkých datech pro každou timed invokaci.
Raw výsledky a disassembly: `results/final.json`. `results/initial.json` je
starší měření s jedním průchodem; nesmí se míchat s finální metodikou.

## Vyřazený experiment

Zkusilo se pozdní přeuspořádání straight-line loop body za účelem odstranění
jednoho skoku, včetně opravy alignmentu podle fyzické pozice. Diferenční test
nezávislé uint64 recurrence prošel, ale delší opakované měření nepotvrdilo
zrychlení: numeric +0,98 %, text +3,07 %. Experiment byl **odstraněn**,
backend zůstal na ověřeném commitu 05ee9d7; žádná volba -O není dodána.
Raw evidence: `results/layout-experiment-rejected.json`. První experiment
běžel při další práci a nebyl použit jako rozhodující výkonové měření.

## Reprodukce a další diagnóza

```sh
python3 benchmarks/contest/run.py --backend src/luxir --backend build/luxir
```

Opery: oddělit souborové I/O, split/konverze, alokace/GC a max reduction.
Vražda: oddělit q=a parsing, hash/equality a frekvence výpovědí. Za běžného
hashování cílit O(N), nikoli porovnání všech dvojic.
Autobusy: oddělit převod stop na dense IDs, stavbu grafu a jednotlivé DFS.
Vytvořit jen hrany a_i -> a_(i+1) pro i < k-2 v zero-based indexaci; poslední
úsek každé linky není platná pasažérská hrana. Zastávka může být dosažitelná
jinou linkou, proto neodstraňovat všechny koncové vrcholy globálně.
Baseline porovnávat při stejném algoritmu i reprezentaci. Pro skutečnou
lokalizaci jsou nutné konkrétní zdroje, větší vstup, verze a build flags.
