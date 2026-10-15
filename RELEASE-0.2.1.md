# LuxIR 0.2.1

- **LuxIR v LSL běží na Windows a macOS.** Paměť mimo spravovaný graf
  objektů (`lsl/luxir_memory.lsl`) pochází na Windows a macOS
  z vynulovaných ByteArray přišpendlených ve slovníku (GC objekty
  nepřesouvá). Na Linuxu zůstávají `mmap`/`munmap` a generovaný kód se
  nemění. LSL 0.10.1 dodává `luxir` pro Windows x64/ARM64 a macOS
  Intel/Apple Silicon.
- Generování kódu je beze změny proti 0.2.0.

Ověřeno: `make check` s referenčním QBE 1.3 (diferenciální test i
fuzzer) a `make check-cross`. `luxir.exe` přeložený pro Windows dává na
celém runtime LSL bajtově stejný výstup jako linuxový build. V CI LSL
(Windows x64/ARM64, macOS Intel/Apple Silicon) LuxIR na těchto systémech
přeložil všechny samostatné testy a ověřil fixed point překladače LSL.
