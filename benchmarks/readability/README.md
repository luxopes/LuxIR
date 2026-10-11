# Čitelnost a rychlost LuxIR — experiment 11. 10. 2026

**Na těchto osmi funkcích čitelnější řízení toku nezpomalilo celý backend
podstatným způsobem. Přechod na obecné indexování IntArray byl naopak drahý.**
Jde o omezený experiment, nikoli důkaz stejného výkonu libovolného přepisu.

## Varianty

- `raw`: nezměněný LuxIR `7480a0fe0a70ec9525cbae7363cf9da457194fce`.
- `readable`: osm bitset funkcí s běžnými `while`, `if`, návraty a pojmenovanými
  proměnnými. **Stejné surové rozložení dat, load/store intrinsics, algoritmy,
  assertion kontroly, unsigned konverze a čtyřnásobné rozvinutí bulk smyček.**
  Těla funkcí mají dohromady 153 řádků místo 264. Ostatní backend zůstává stejný.
- `array`: samostatné mikroměření stejného algoritmu přes běžné indexování
  `IntArray`; tato varianta už mění způsob přístupu k datům a reprezentaci hodnot.
- `array_typed`: totéž s explicitním `__lsl_typed_int` při čtení prvků, aby
  následná aritmetika používala známé celočíselné hodnoty.

Přepsané funkce: `bscount`, `bsequal`, `bsset`, `bsclr`, `bscopy`, `bsunion`,
`bsinter`, `bsdiff`. Kód je v [readable.lsl](readable.lsl). Přípony `q_f_`
zůstávají kvůli shodnému rozhraní původního backendu. Experiment nepřepisuje
produkční `lsl/luxir.lsl` ani nemění pravidla jeho generování z C reference.

## Skutečné velké vstupy

Dvě sestavy **nativního LSL backendu** zpracovávají stejné uložené IR, nikoli
různé verze zdrojů. Časy zahrnují start backendu, čtení IR a zápis assembleru;
nezahrnují práci frontendu, GNU assembleru ani linkeru.

| Vstup | Původní backend | Čitelnější backend | Změna času |
|---|---:|---:|---:|
| IR frontendu, 4,2 MB | 1,6584 s | 1,6578 s | −0,04 % |
| IR backendu, 9,8 MB | 5,8358 s | 5,8009 s | −0,60 % |

Medián CPU času: frontend 1,6490 / 1,6493 s; backend 5,8124 / 5,7870 s.
Rozsahy jednotlivých wall vzorků se překrývají. Z těchto malých rozdílů
nevyvozujeme obecné zrychlení. **Nezjistili jsme podstatnou výkonnostní penalizaci
tohoto konkrétního přepisu osmi funkcí.** Celý backend přepsaný nebyl.

## Izolované operace nad bitsety

256 slov po 64 bitech, sedm opakování, medián wall času. Každý řádek používá
stejný počet iterací pro oba členy dvojice; iterace se mezi řádky liší.
`bulk` volá copy, intersection, union a difference, s původním rozvinutím smyček.

| Operace | Původní explicitní CFG | Čitelné řízení toku, stejná paměť | Poměr časů |
|---|---:|---:|---:|
| Počet bitů | 192,209 ms | 186,172 ms | 0,969× |
| Bulk operace | 116,035 ms | 116,647 ms | 1,005× |
| Rovnost | 106,754 ms | 108,119 ms | 1,013× |
| Set/clear | 31,764 ms | 31,576 ms | 0,994× |

Samostatné dvojice porovnávají změnu přístupu k datům. Poměr je čas indexované
varianty dělený časem původní varianty; vyšší číslo znamená pomalejší běh:

| Operace | Běžný IntArray / raw | IntArray s explicitním Int / raw |
|---|---:|---:|
| Počet bitů | 67,82× | 3,88× |
| Bulk operace | 30,14× | 30,31× |
| Rovnost | 17,91× | 15,32× |
| Set/clear | 5,84× | 5,49× |

Tohle jsou **poměry konkrétních mikroměření v LSL 0.9.1**, nikoli zpomalení
celého backendu ani obecná vlastnost IntArray ve všech jazycích. IntArray má
kompaktní backing buffer, ale jeho obecné indexované rozhraní zde volá runtime.
Vygenerované IR obsahuje `rt_runtime_get`, `rt_runtime_set` a v netypované
variantě také boxing/unboxing a dynamické sčítání. Explicitní Int odstranil
tyto dynamické aritmetické kroky v popcountu, obecný přístup k prvkům zůstal.
Experiment není profilem, který by přisuzoval přesné procento času každé příčině.

Data záměrně využívají všech 64 bitů včetně znaménkového bitu, což je platný
a relevantní případ pro bitsety. Nelze výsledky přenášet na malá celá čísla.
Nejkratší raw běhy v těchto dvojicích trvají 6–22 ms včetně startu procesu;
poměry proto ovlivňuje i režie spuštění. Neodečítali jsme odhadovaný baseline.

## Metodika a kontrola

- Linux x86_64, Intel Core i5 760, čtyři fyzická jádra.
- Jedna pevná sestava LSL **0.9.1**, pro obě varianty stejný compiler, runtime
  a nativní LSL LuxIR při sestavení. Žádný C fallback v měření.
- CPU affinity 2, limit adresního prostoru 4 GiB. Měření jsou sériová, pořadí
  variant se střídá. Sedm vzorků pro každý člen každé dvojice, velké vstupy
  mají navíc warmup. Uživatelský Firefox/X11 běžel dál.
- Oba backendy: **456/456** vstup/target dvojic proti C QBE 1.3, šest cílových
  ABI přes tři ISA rodiny; výstupní assembler se shoduje bajtově.
- Oba backendy: všech šest maintained spustitelných testů a 36 očekávaných
  chybných vstupů PASS na Linuxu x86_64.
- **256 nezávislých kontrol** mikroměření proti výsledkům vypočteným Pythonem:
  prázdné množiny, hranice rozvinutých smyček, plná 64bitová slova, rovnost,
  nerovnost a mutace. Každý měřený vzorek má zkontrolovaný výsledek.
- Assembler obou velkých vstupů je mezi raw/readable bajtově shodný.

[Všechny vzorky, příkazy, hashe a kontrolní součty](results-2026-10-11.json).
Časy prvního sestavení variant nejsou opakovaný benchmark a nepoužíváme je
k porovnání výkonu. Neověřovali jsme self-host fixed point nového experimentu.

## Opakování

Rozbalit plný binární balík LSL 0.9.1 do soukromého adresáře; globální instalace
se nemění. `--reference` je C QBE 1.3 pouze pro diferenční kontrolu.

```sh
mkdir -p build/readability/toolchain
tar -xzf /cesta/lsl-0.9.1-linux-x86_64.tar.gz -C build/readability/toolchain
python3 benchmarks/readability/build.py \
  --toolchain build/readability/toolchain/lsl-0.9.1
python3 benchmarks/readability/measure.py \
  --toolchain build/readability/toolchain/lsl-0.9.1 \
  --reference /cesta/qbe-1.3/qbe --repeats 7 --cpu 2
```

`build/readability/` obsahuje izolované zdroje, binárky, zachycené IR a assembler.
`prepare.py` odmítne změněný produkční zdroj, dokud se experiment znovu neprověří
a nepřipne jeho hash. Zachovat surovou paměť a zpřehlednit algoritmy je podle
tohoto měření rozumný směr; obecné objekty či kolekce v hot path vyžadují vlastní
měření a případně lepší specializaci v LSL compileru.
