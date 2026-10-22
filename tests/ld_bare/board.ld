/* Kopie runtime/arch/baremetal_riscv64.ld z LSL: rozložení, které vestavěný
   linker napodobuje v režimu -b (tests/ld_bare.py ho porovnává s GNU ld).
   Sekce začínají na stránce kvůli oprávněním stránkování (kód R+X, data
   jen pro čtení R, ostatní RW). Pod hlavním zásobníkem (8 MiB) a pod
   zásobníky pastí hartů je nenamapovaná strážní stránka. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { KEEP(*(.text.start)) *(.text .text.*) }
  . = ALIGN(4096);
  lsl_text_end = .;
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  . = ALIGN(4096);
  lsl_rodata_end = .;
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  /* Zásobníky pastí: 8 hartů po 20 KiB (4 KiB stráž + 16 KiB). */
  .trapstacks (NOLOAD) : ALIGN(4096) {
    lsl_trap_stacks = .;
    . += 8 * 20480;
  }
  .stack (NOLOAD) : ALIGN(4096) {
    lsl_stack_guard = .;
    . += 4096;
    . += 0x800000;
    lsl_stack_top = .;
  }
  lsl_image_end = .;
  /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.riscv.attributes) *(.eh_frame) }
}
