# LuxIR – specifikace jazyka

LuxIR je textová mezireprezentace (IR) backendu LuxIR. Syntaxí vychází
z LSL: bloky končí `end`, hlavičky dvojtečkou, typované deklarace mají tvar
`Int32(x) = …` a komentáře začínají `#`. Významem odpovídá SSA IR jazyka
QBE 1.3. Backend z ní vytváří assembler pro amd64 (System V, Apple,
Windows), arm64 (Linux, Apple) a rv64.

```lsl
# Faktoriál
export function fact(Int64(n)) -> Int64:
start:
    goto loop
loop:
    Int64(i) = phi(start: n, body: i1)
    Int64(acc) = phi(start: 1, body: acc1)
    Int32(done) = le(i, 1)
    if done goto finish else body
body:
    Int64(acc1) = mul(acc, i)
    Int64(i1) = sub(i, 1)
    goto loop
finish:
    return acc
end
```

## Verze protokolu

`luxir --version` (samostatný příkaz) vypíše podporovanou revizi IR a
runtime kontrakty. Aktuálně: `LuxIR 0.2.0 (IR 1; runtime ABI none, lsl-0)`.
Číslo původního QBE není verzí rozhraní LuxIR.

Frontend může před všemi definicemi a direktivou `file` zapsat:

```text
ir_version "1"
runtime_abi "none"
```

Obě hlavičky jsou volitelné; jejich pořadí je libovolné. Starší vstupy bez
hlavičky zachovávají dnešní význam (IR 1, externí runtime). Verze jsou
kanonické řetězce bez escape sekvencí, nikoli číselné operandy: nepřetékají
modulo 2^64. Komentáře a prázdné řádky před hlavičkou jsou povolené.
Opakovaná, pozdní nebo nekompatibilní hlavička skončí diagnostikou s cestou
a řádkem. Přijatá hlavička nemění generovaný assembler.

`runtime_abi "none"` znamená, že backend nespravuje integrované luxrt ABI.
Nezakazuje externí volání; přijetí IR není důkazem kompatibility externě
slinkované knihovny.

`runtime_abi "lsl-0"` označuje program pro runtime LSL, který dodává LuxIR
(`runtime/lsl/`, rozhraní v [lsl-runtime.md](lsl-runtime.md)). Assembler se
tím nemění; hlavička určuje, se kterým runtime `luxir -x` program slinkuje.
Všechny vstupní soubory jednoho překladu musí mít stejné runtime ABI (soubor
bez hlavičky ho nemění). Jiné hodnoty se odmítají. Viz [runtime.md](runtime.md).

## Lexikální pravidla

- Soubor je v UTF-8. Mimo řetězce a komentáře se smí používat jen ASCII.
  Nulový bajt je chyba.
- `#` zahajuje komentář do konce řádku.
- Příkazy končí koncem řádku. Uvnitř `( … )` a `[ … ]` se konce řádků
  ignorují, takže dlouhé seznamy lze psát na více řádků.
- Jméno začíná písmenem, `_` nebo `.` a pokračuje písmeny, číslicemi a
  znaky `_ . $`. Velká a malá písmena se rozlišují.
- Celé číslo se zapisuje desítkově, `0x…`, `0o…` nebo `0b…`, volitelně se
  znaménkem `-`. Hodnota se redukuje modulo 2^64.
- Desetinné číslo obsahuje tečku nebo exponent: `1.5`, `-0.25`, `1e9`,
  `2.5e-3`. Hodnoty `inf` a `nan` lze zapsat jen jako `Float64(inf)` nebo
  `Float32(nan)`.
- Řetězec `"…"` je na jednom řádku. Escape sekvence jsou stejné jako v LSL
  (`\n \r \t \0 \" \' \\`) a navíc `\xHH`.
- Oddělovače: `( ) [ ] , : = + * -> ...`.

## Typy

| LuxIR | Význam | Třída QBE |
|---|---|---|
| `Int32` | 32bitové celé číslo | `w` |
| `Int64` | 64bitové celé číslo | `l` |
| `Ptr` | ukazatel s šířkou cíle (dnes 64 bitů) | `l` |
| `Float32` | IEEE binary32 | `s` |
| `Float64` | IEEE binary64 | `d` |
| `Int8`, `Int16` | malé celé číslo se znaménkem | `sb`, `sh`; v datech `b`, `h` |
| `UInt8`, `UInt16` | malé celé číslo bez znaménka | `ub`, `uh`; v datech `b`, `h` |
| jméno z `type` | agregát (struktura, union, opaque) | `:jméno` |
| `Env` | parametr nebo argument prostředí | `env` |

Hodnoty v registrech (lokální hodnoty) mají typ `Int32`, `Int64`, `Ptr`,
`Float32` nebo `Float64`. Hodnota typu `Int64`/`Ptr` smí být použita tam,
kde se čeká `Int32`; použije se spodních 32 bitů.

`Int8`, `UInt8`, `Int16` a `UInt16` jsou jen typy ABI: parametrů, argumentů,
návratových hodnot, výsledků volání a uložení do paměti. V ABI se zaručuje
jen spodních 8 nebo 16 bitů. Příjemce si hodnotu rozšíří sám přes `sext8`,
`zext8`, `sext16` nebo `zext16`.

`Ptr` je pro budoucí 32bitové cíle. Na dnešních 64bitových cílech se
chová stejně jako `Int64`.

## Definice nejvyšší úrovně

```lsl
[export] [thread] [common] [section "jméno" ["příznaky"]] function …
[export] [thread] [common] [section "jméno" ["příznaky"]] data …
type …
file "zdroj.c"
```

`export` zviditelní symbol pro linker. `thread` je jen pro data a vytvoří
thread-local úložiště. `common` vytvoří common symbol. `section` určí sekci.
`file` nastaví soubor pro následující `dbgloc`.

### Funkce

```lsl
export function jméno(Int32(a), Ptr(p), Env(e), ...) -> Typ:
    …
end
```

- Parametr se píše `Typ(jméno)`. Agregátní parametr `Pair(p)` dostane
  ukazatel na kopii hodnoty.
- `Env(jméno)` je nejvýše jeden parametr prostředí.
- `...` na konci označí funkci s proměnným počtem argumentů.
- `-> Typ` je návratový typ (základní, malý nebo agregát). Bez něj funkce
  nic nevrací. Dvojtečka za hlavičkou je volitelná.
- Tělo končí řádkem, na kterém je samotné `end`.

### Data

```lsl
data jméno [align N] = položka
data jméno [align N] = [položka, položka, …]
```

Položky se v paměti ukládají za sebou bez automatického zarovnání:

| Položka | Význam |
|---|---|
| `"text"` | bajty řetězce, bez koncové nuly |
| `Int8(1, 2, …)`, `Int16(…)`, `Int32(…)`, `Int64(…)`, `Ptr(…)` | celá čísla, řetězce nebo adresy |
| `Float32(1.5, …)`, `Float64(…)` | desetinná čísla |
| `Ptr(symbol)`, `Int64(symbol + 8)` | adresa symbolu s posunem |
| `bits(N)` uvnitř `Float32/Float64` | přesný bitový vzor |
| `zero(N)` | N nulových bajtů |

Data obsahující jen `zero(…)` jdou do BSS. Výchozí zarovnání je 8.

### Agregátní typy

```lsl
type Pair = struct(Int64, Int32)
type Bytes = struct(Int8 * 16, Pair * 2)
type Either align 8 = union(struct(Int32), struct(Float64, Int8))
type Blob align 16 = opaque(32)
```

Pole struktury dostávají přirozené zarovnání. Agregát použitý jako pole
musí být definovaný dříve. `opaque(N)` je blok N bajtů a vyžaduje `align`.

## Tělo funkce

Tělo tvoří bloky. Blok začíná návěštím `jméno:` na samostatném řádku.
Pokud tělo nezačíná návěštím, první blok je skrytý a nelze na něj skočit.
Blok, který nekončí skokem, pokračuje do následujícího bloku.

Instrukce `phi` smějí stát jen na začátku bloku, před ostatními instrukcemi.

### Jména

Jméno je lokální hodnota, pokud je parametrem funkce nebo je kdekoli v těle
definované tvarem `Typ(jméno) = …`. Všechna ostatní jména jsou globální
symboly (funkce a data). Prefixy jako `%` a `$` v QBE nejsou potřeba.
Lokální jméno zakryje stejně pojmenovaný globální symbol. Návěští mají
vlastní jmenný prostor.

Hodnotu lze přiřadit vícekrát, i ve více větvích. Backend z toho sám
sestaví SSA.

### Operandy

| Zápis | Význam |
|---|---|
| `x` | lokální hodnota nebo adresa globálního symbolu |
| `42`, `-1`, `0xFF` | celočíselná konstanta |
| `1.5` | desetinná konstanta s přesností podle kontextu |
| `Int64(5)`, `Float32(1.5)` | konstanta s uvedeným typem |
| `Int32(x)` | hodnota použitá jako daný typ (např. spodních 32 bitů) |
| `bits(N)` | bitový vzor N i v desetinném kontextu |
| `thread(x)` | adresa thread-local symbolu |
| `extern(x)` | symbol přes GOT/PLT |

Celé číslo v desetinném kontextu musí být `0`. Jinak je to chyba, protože
v QBE by se `1` vyložila jako bitový vzor a vznikla by skoro nula. Napiš
`1.0`, nebo když opravdu chceš bity, `bits(N)`. Desetinné číslo
v celočíselném kontextu je chyba.

### Instrukce s výsledkem

```lsl
Typ(jméno) = instrukce(operandy)
Typ(jméno) = operand              # kopie
Typ(jméno) = call f(argumenty)
Typ(jméno) = phi(blok: hodnota, …)
```

Výsledek instrukce má typ `Int32`, `Int64`, `Ptr`, `Float32` nebo
`Float64`. Výsledek volání smí být i malý nebo agregátní typ. Pro agregát
dostaneš ukazatel na vrácenou hodnotu.

Typ operandů určuje cílový typ instrukce, pokud tabulka neříká jinak:

| Skupina | Instrukce |
|---|---|
| aritmetika | `add sub mul div neg` (celá i desetinná), `rem udiv urem` |
| bity | `and or xor`, posuny `shl shr sar` (posun je `Int32`) |
| kopie | `copy` |
| paměť | `load(p)` (šířka podle výsledku), `load_i8 load_u8 load_i16 load_u16 load_i32 load_u32` |
| zásobník | `alloc4(n)`, `alloc8(n)`, `alloc16(n)` (číslo je zarovnání) |
| rozšíření | `sext8 zext8 sext16 zext16 sext32 zext32` |
| desetinná | `fext` (Float32→Float64), `ftrunc` (Float64→Float32), `bitcast` |
| varargs | `vaarg(ap)` |

U instrukcí v další tabulce určuje přesnou operaci typ operandu. Je to
anotace `Typ(…)` nebo typ lokální hodnoty prvního operandu, který ho má.
Pokud ho nemá žádný operand (například `lt(1, 2)`), je to chyba.

| Instrukce | Celá čísla | Desetinná čísla |
|---|---|---|
| `eq ne` | rovnost | rovnost |
| `lt le gt ge` | se znaménkem | uspořádané |
| `ult ule ugt uge` | bez znaménka | – |
| `ord unord` | – | žádný / některý operand NaN |
| `ftoi ftou` | – | na celé číslo se znaménkem / bez |
| `itof utof` | ze čísla se znaménkem / bez | – |

Porovnání vrací 1 nebo 0 typu `Int32` nebo `Int64`.

### Příkazy

```lsl
store(Int32(v), p)          # šířka podle typu hodnoty
call f(Int32(a), ...)       # volání bez výsledku
blit(zdroj, cíl, 16)        # kopie N bajtů, N je konstanta
vastart(ap)                 # jen ve funkci s ...
dbgloc(řádek, sloupec)
goto blok
if c goto ano else ne       # c je Int32, nenulové = ano
return
return hodnota
halt                        # nedosažitelné místo
```

`store` bere šířku z typu hodnoty. U konstanty ji uveď vždy:
`store(Int8(0), p)`. `Int8`/`UInt8` uloží bajt a `Int16`/`UInt16` dva bajty.

Argument volání se píše `Typ(hodnota)`. U lokální hodnoty lze typ vynechat
a použije se její vlastní. `...` odděluje pevné argumenty od proměnných.
`Env(x)` předá prostředí. Volat lze globální funkci i ukazatel v lokální
hodnotě.

## Příkazová řádka

```text
luxir [-t cíl] [-o výstup.s] [-d příznaky] soubor.lir …
```

Cíle: `amd64_sysv` (výchozí na Linuxu x86_64), `amd64_apple`, `amd64_win`,
`arm64`, `arm64_apple`, `rv64`. Bez `-o` jde výstup na standardní výstup.
Bez vstupu nebo se vstupem `-` se čte standardní vstup. `-d P` vypíše
funkce po parsování ve tvaru LuxIR. Další ladicí příznaky odpovídají QBE.

## Převod z QBE IL

Nástroj `tools/qbe2lir.py` převede QBE IL na LuxIR se zachovaným
významem. Z převedeného vstupu vznikne bajtově stejný assembler, jaký
vytvoří QBE 1.3 z původního IL. Hlavní rozdíly v zápisu:

| QBE IL | LuxIR |
|---|---|
| `%x =w add %a, 1` | `Int32(x) = add(a, 1)` |
| `%c =w csltl %a, %b` | `Int32(c) = lt(a, b)` (a je `Int64`) |
| `%c =w ceqw %l, 0` (l je `l`) | `Int32(c) = eq(Int32(l), 0)` |
| `%f =d swtof %i` | `Float64(f) = itof(i)` |
| `storeb %v, %p` | `store(Int8(v), p)` |
| `%v =w loadw %p` | `Int32(v) = load_i32(p)` |
| `jnz %c, @a, @b` | `if c goto a else b` |
| `call $f(w %a, ...)` | `call f(Int32(a), ...)` |
| `d_1.5`, `s_1.5` | `1.5` (přesnost z kontextu) |
| `@start` | `start:` |
| `function w $f(l %a) { … }` | `function f(Int64(a)) -> Int32: … end` |
| `data $d = { b "x", b 0 }` | `data d = ["x", Int8(0)]` |
| `type :p = { l, w 2 }` | `type p = struct(Int64, Int32 * 2)` |

## rv64 static PIE

Volba `-p` použije u rv64 PC-relative `lla` pro lokální i externí adresy
symbolů. Výchozí režim zachovává assembler původního QBE, včetně absolutních
lokálních adres. `-p` je režim pro staticky slinkovaný obraz; neslibuje
interpozici symbolů sdílené knihovny ani dynamické TLS. Backend neprovádí
relokace dat obsahujících absolutní pointery při startu statického PIE;
ty vyžadují vlastní linker/relokátor a zůstávají otevřené. Na ostatních
cílech volba zatím adresování nemění.
