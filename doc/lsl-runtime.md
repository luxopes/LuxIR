# Runtime pro LSL — požadavky frontendu

Stav 13. 10. 2026. LSL bude jen frontend: překládá LSL do LuxIR a nic víc.
Generování kódu, runtime (model objektů, alokace a GC, kolekce, řetězce,
převody, I/O, procesy, vlákna, chyby) i formát binárky patří LuxIR. Rozhraní
standardizuje LuxIR a LSL se mu přizpůsobí. Tento dokument popisuje, co
dnešní LSL od runtime potřebuje, aby šlo dnešní ručně psaný x86-64 runtime
LSL nahradit runtime z LuxIR beze změny chování a bez zpomalení.

## Stav (LuxIR 0.1.2)

- Runtime patří LuxIR: zdroje `runtime/lsl/linux-x86_64/` (převzaté emitory
  x86-64 v LSL z LSL `461dd58`), sestavení `make runtime-lsl`, objekt
  `luxir-runtime/lsl-0/amd64_sysv.o`, bajtově shodný s dřívějším
  `compiler/qbe/runtime.o` z LSL.
- Frontend zapíše `ir_version "1"` a `runtime_abi "lsl-0"` a zavolá
  `luxir -x -t amd64_sysv -o program program.lir`; zbytek (assembler, linker,
  runtime) udělá LuxIR. Ověřeno na LSL: kompilátor v pevném bodu a bajtově
  shodný s dosavadní cestou, 227 binárek testů a benchmarků shodných,
  rychlá i plná sada testů LSL prošla (VALIDATION.md).
- Volání z vygenerovaného kódu: SysV funkce `rt_runtime_<jméno>` (můstek
  nastaví stavový registr a volá interní x86 rutinu), dále `qbe_error_set`,
  `qbe_error_flag`, `qbe_error_pending`, `qbe_error_catch`, `qbe_error_clear`,
  `qbe_exit`. Program definuje `lsl_entry`, `lsl_globals`, `lsl_global_bytes`,
  `lsl_global_flags` a obvykle `lsl_init_literals`.
- Zbývá: přepsat runtime z emitorů x86 do přenositelného LuxIR a změnit ABI
  podle požadavků níže (sekce Výkon); LSL pak přejde na novou verzi ABI.

## Model hodnot a objektů (musí zůstat kompatibilní, nebo se LSL přizpůsobí)

- Int je signed 64bit, aritmetika modulo 2^64. Hodnota Any je 64bitové slovo:
  surový Int, nebo ukazatel na objekt. Rozlišení dnes zajišťuje bitmapa
  začátků objektů (stav +1328, kapacita +1336) a hlavička bloku; standardní
  runtime by měl zavést tagované hodnoty (viz sekce Výkon runtime v TODO).
- Tagy objektů (+0): String 1, List 2, Dictionary 3, Bool 4, None 5, Float 6,
  Tuple 7, RealArray 8, Int 9, Function 10, NativeLibrary 11, NativeFunction 12,
  IntArray 13, FloatArray 14, ByteArray 15, Set 16.
- String: +0 tag, +8 délka v bajtech, od +16 UTF-8 bajty (+ nulový bajt).
  Literály jsou statické záznamy `Int64(1, délka), "text", Int8(0)`.
- List a pole: +0 tag, +8 počet, +16 kapacita, +24 data (backing blok).
- Dictionary: +0 tag, +8 počet, +16 kapacita, +24 klíče, +32 hodnoty
  (pořadí vkládání), +40 index (0 u malého slovníku), +48 počet slotů indexu.
  Položka indexu: pozice + 1 (bity 0-31), klíč je String (bit 32), horních
  31 bitů hashe (bity 33-63). Referenční kód: `doc/lsl-runtime/dict.lir`.
- Blok haldy: 32 B hlavička před objektem (−32 kapacita, −24 další blok,
  −16 druh: 0 objekt, 1 volný, 2 surový backing, −8 značka GC / odkaz
  volného seznamu).

## Chování, které musí runtime zachovat

- Chyby: čekající chyba a příznak v TLS vlákna (dnes fs:0 ukazatel na
  záznam String, fs:8 příznak); texty chyb jsou součástí výstupů testů LSL
  (např. `KeyError: key not found`, `IndexError: index out of range`,
  `TypeError: … got T`).
- Hash klíčů: seed procesu, FNV-1a pro String, SplitMix64; pořadí slovníku
  je pořadí vkládání; `1`, `1.0` a `True` jako klíče podle dnešních testů.
- Float: převod textu správně zaokrouhlený (Clinger + Eisel-Lemire + přesné
  porovnání dlouhých vstupů), výpis nejkratší nejbližší (jako `repr`
  v Pythonu); dnes v LSL `runtime_decimal_bits`, `runtime_decimal_compare`,
  `runtime_decimal_digits_compare`, `runtime_float_nearer_upper`.
- GC: konzervativní procházení zásobníků a registrů všech vláken, kořeny
  v `lsl_globals`, rozpočet 3× živé haldy, bloky se nedělí ani neslučují.
- Vlákna: `thread_spawn`/`thread_join`, zámky, atomiky, GC zastaví vlákna.
- Procesy a I/O: `run`, `capture*` včetně stdin/stream variant, zavírání
  zděděných deskriptorů, limity, EINTR; bufferovaný stdout/stdin.

## Funkce volané z kódu generovaného LSL (145)

Jméno je `rt_runtime_<jméno>` (u `invoke` přímo uvedený symbol). Sloupec
uvádí počet argumentů a typ výsledku podle `qbe_emit.lsl`; `intrinsic X`
znamená vestavěnou funkci LSL `X` mapovanou na tuto rutinu.

| Rutina | Použití (argumenty → výsledek) |
|---|---|
| `appendfile` | intrinsic appendfile->None |
| `atomic_pause` | 0->None |
| `box_bool` | 1->Any; intrinsic __lsl_box_bool->Any |
| `box_dynamic_function` | 1->Any |
| `box_dynamic_int` | 1->Any |
| `box_function` | 1->Any |
| `box_int` | 1->Any; intrinsic __lsl_box_int->Any |
| `box_none` | 0->Any; intrinsic __lsl_box_none->Any |
| `byte_array_new` | 0->Any |
| `byte_array_zeros` | 1->Any; intrinsic __lsl_byte_zeros->ByteArray |
| `byte_copy` | intrinsic __lsl_byte_copy->Int |
| `capture` | intrinsic __lsl_capture->String; intrinsic capture->String |
| `capture_clean` | intrinsic __lsl_capture_clean->String |
| `capture_status` | intrinsic capture_status->Int |
| `capture_stderr` | intrinsic capture_stderr->String |
| `checked_index` | 2->Any; 2->Int; intrinsic __lsl_checked_index->Int |
| `chr` | intrinsic chr->String |
| `clock` | intrinsic __lsl_monotonic->Float; intrinsic clock->Float; intrinsic monotonic->Float |
| `coerce_float` | 1->Any; 1->Float |
| `cpu_count` | intrinsic cpu_count->Int |
| `data_address` | intrinsic __lsl_data_address->Int |
| `decimal_bits` | intrinsic __lsl_float_from_decimal->Int |
| `decimal_compare` | intrinsic __lsl_decimal_compare->Int |
| `decimal_digits_compare` | intrinsic __lsl_decimal_digits_compare->Int |
| `dict_delete` | 2->Any |
| `dict_get` | 1->Any; 2->Any |
| `dict_keys` | 1->Any |
| `dict_new` | 0->Any; 0->Dictionary |
| `dict_set` | 1->Any |
| `dynamic_append` | 1->Any |
| `dynamic_clear` | 1->Any |
| `dynamic_delete` | 2->Any |
| `dynamic_dict_keys` | 1->Any |
| `dynamic_get` | 1->Any |
| `dynamic_length` | 1->Any |
| `equal_any` | 4->Bool; intrinsic __lsl_raw_equal->Int |
| `executable_path` | intrinsic executable_path->String |
| `exists` | intrinsic exists->Bool |
| `fd_close` | intrinsic __lsl_fd_close->Int |
| `fd_connect` | intrinsic __lsl_fd_connect->Int |
| `fd_ioctl` | intrinsic __lsl_fd_ioctl->Int |
| `fd_mmap` | intrinsic __lsl_fd_mmap->Int |
| `fd_open` | intrinsic __lsl_fd_open->Int |
| `fd_read` | intrinsic __lsl_fd_read->Int |
| `fd_seek` | intrinsic __lsl_fd_seek->Int |
| `fd_snapshot` | intrinsic __lsl_fd_snapshot->Int |
| `fd_socket` | intrinsic __lsl_fd_socket->Int |
| `fd_write` | intrinsic __lsl_fd_write->Int |
| `float_accumulate` | intrinsic __lsl_float_accumulate->Float |
| `float_array_axpy` | 3->Any |
| `float_array_bytes` | intrinsic __lsl_floatarray_bytes->String |
| `float_array_dot` | 1->Any; intrinsic float_dot->Float |
| `float_array_fill` | 1->Any; intrinsic float_fill->None |
| `float_array_from_bytes` | intrinsic __lsl_floatarray_from_bytes->FloatArray |
| `float_array_matmul` | 1->Any |
| `float_array_sum` | 1->Any; intrinsic float_sum->Float |
| `float_candidate_bits` | intrinsic __lsl_float_candidate_bits->Int |
| `float_div` | 3->Float |
| `float_from_int` | 1->Any; 1->Float; 2->Any; intrinsic __lsl_float_from_int->Float |
| `float_nearer_upper` | intrinsic __lsl_float_nearer_upper->Bool |
| `float_new` | 1->Float; 2->Float; 3->Float; intrinsic __lsl_float_from_bits->Float |
| `float_sqrt` | 1->Any |
| `float_to_int` | intrinsic __lsl_float_to_int->Int |
| `from_cstring` | 1->String |
| `gc` | intrinsic gc->Int |
| `get` | 1->Any |
| `getenv` | intrinsic __lsl_getenv->Any |
| `input` | intrinsic input->String |
| `int_power` | 2->Int |
| `int_to_string` | 1->Any |
| `intern_table_mask` | intrinsic __lsl_intern_table_mask->Int |
| `is_heap_object` | intrinsic __lsl_is_heap->Bool |
| `isatty` | intrinsic __lsl_isatty->Bool |
| `iter_get` | 1->Any |
| `length` | 1->Any |
| `list_append` | intrinsic __lsl_set_append->None |
| `list_clear` | 1->Any |
| `list_new` | 1->Any; 2->Any; 2->FloatArray; 2->Set; intrinsic __lsl_new_set->Set |
| `list_pop_last` | intrinsic __lsl_pop_last->Any |
| `list_set` | intrinsic __lsl_set_store->None |
| `lsl_init_literals` | invoke->? |
| `lsl_join_native` | invoke->? |
| `maybe_gc` | 0->None |
| `mem_pack` | intrinsic __lsl_mem_pack->Int |
| `mem_unpack` | intrinsic __lsl_mem_unpack->Int |
| `mkdir` | intrinsic mkdir->Int |
| `mkdir_private` | intrinsic __lsl_mkdir_private->Int |
| `native_call` | intrinsic native_call->Int |
| `native_close` | intrinsic native_close->Int |
| `native_function` | intrinsic native_function->NativeFunction |
| `native_name_equal` | intrinsic native_name_equal->Bool |
| `native_open` | intrinsic native_open->NativeLibrary |
| `native_size` | intrinsic native_size->Int |
| `object_table_mask` | intrinsic __lsl_object_table_mask->Int |
| `ord` | intrinsic ord->Int |
| `parse_literal_int` | intrinsic __lsl_parse_literal_int->Int |
| `print_string` | 1->Any; 2->Any |
| `qbe_error_catch` | invoke->? |
| `qbe_error_clear` | invoke->? |
| `qbe_error_pending` | invoke->? |
| `qbe_error_set` | invoke->? |
| `qbe_exit` | invoke->? |
| `raster_copy` | intrinsic __lsl_raster_copy->Int |
| `raster_fill` | intrinsic __lsl_raster_fill->Int |
| `raster_pack` | intrinsic __lsl_raster_pack->Int |
| `readbytes` | intrinsic readbytes->ByteArray |
| `readfile` | intrinsic readfile->String |
| `require_numeric_pair` | 1->Any; 3->None |
| `require_numeric_value` | 1->Any; 2->Any; 2->None; 3->None |
| `reserve_object_table` | 2->None |
| `rmdir` | intrinsic rmdir->Int |
| `rt_runtime_` | invoke->? |
| `run` | intrinsic __lsl_run->Int; intrinsic run->Int |
| `run_clean` | intrinsic __lsl_run_clean->Int |
| `set_global_roots` | 2->None |
| `sleep` | intrinsic __lsl_sleep->None |
| `slice` | 1->Any |
| `stdin_eof` | intrinsic __lsl_stdin_eof->Bool |
| `string_byte_offset` | intrinsic __lsl_text_byte_offset->Int |
| `string_chars` | intrinsic __lsl_string_chars->List |
| `string_concat` | 2->Any; 2->String |
| `string_equal` | 1->Any |
| `syscall` | intrinsic __lsl_syscall->Int |
| `syscall6` | intrinsic __lsl_syscall6->Int |
| `test_fd_fault` | intrinsic __lsl_test_fd_fault->Int |
| `test_small_tables` | intrinsic __lsl_test_small_tables->None |
| `text_byte` | 1->Any; intrinsic __lsl_text_byte->Int |
| `text_byte_length` | intrinsic __lsl_text_byte_length->Int |
| `text_digits` | intrinsic __lsl_text_digits->Int |
| `text_from_bytes` | intrinsic __lsl_text_from_bytes->String |
| `text_piece` | 1->Any; intrinsic __lsl_text_piece->String |
| `text_scan_digits` | intrinsic __lsl_text_scan_digits->Int |
| `text_split_byte` | 1->Any; intrinsic __lsl_text_split_byte->List |
| `thread_spawn` | 2->Any |
| `truth` | 1->Any |
| `type_code` | 1->Any; 2->Any; intrinsic __lsl_type_code->Int |
| `unbox_dynamic_bool` | intrinsic __lsl_unbox_bool->Bool |
| `unbox_dynamic_float` | intrinsic __lsl_unbox_float->Float |
| `unbox_dynamic_function` | 1->Any; intrinsic __lsl_unbox_function->Function |
| `unbox_dynamic_int` | 1->Any; intrinsic __lsl_unbox_int->Int |
| `uncaught_error` | 0->None |
| `unlink` | intrinsic unlink->Int |
| `wall_time` | 0->Float; intrinsic __lsl_wall_time->Float; intrinsic wall_time->Float |
| `writebytes` | intrinsic writebytes->None |
| `writefile` | intrinsic writefile->None |

## Měřítka

LSL testy: `build/dev/lsl-test qbe-fast|qbe <lslc>` v repozitáři LSL, plus
benchmarky v `tests/benchmarks/v089` a programy „autobusy“ a „vrah“ (TODO).
Nahrazení runtime nesmí změnit výstupy testů ani zpomalit překlad či běh
proti předchozímu vydání LSL.

## Výkon runtime pro překladač LSL (profil 13. 10. 2026)

Překlad kompilátoru LSL (frontend + generování LuxIR, ~2,1 s po optimalizacích
frontendu) tráví asi **35 % času v GC a alokátoru** dnešního runtime:

| Místo | Podíl | Příčina |
|---|---|---|
| konzervativní test ukazatele (`is_heap_allocation`) při značkování | ~16 % | každé slovo každého živého objektu se ověřuje přes bitmapu a hlavičku bloku |
| sweep | ~13 % | prochází všechny bloky haldy při každém GC |
| hledání volného bloku (`alloc_reuse_loop`) | ~9 % | až 128 pokusů v seznamu velikostní třídy |
| `is_heap_object` u hodnot Any | ~10 % | rozlišení surový Int / ukazatel |

Požadavky na runtime LuxIR (frontend dodá potřebné informace):

- [ ] **Přesné GC:** frontend zná u každého slotu, zda nese ukazatel (typovaný
  Int/Float/Bool vs. Any/objekt); LuxIR potřebuje způsob, jak to vyjádřit
  (typ `Ref`/`Any` v IR, mapy zásobníku a objektů), aby značkování nemuselo
  testovat každé slovo.
- [ ] **Tagované hodnoty Any** místo bitmapy: test ukazatele jednou instrukcí.
- [ ] **Rychlá alokace malých objektů** (bump/TLAB nebo segregované volné
  seznamy bez prohledávání) a sweep, který nechodí přes celou haldu
  (generační nebo líný sweep po stránkách).
- [ ] **Slovník s kapacitou:** `dict_new(kapacita)` pro literály; AST a tokeny
  frontendu jsou malé slovníky (4-8 klíčů), dnes vždy s kapacitou 16
  (2 × 128 B polí + hlavičky), což zdvojnásobuje živou haldu i práci GC.
