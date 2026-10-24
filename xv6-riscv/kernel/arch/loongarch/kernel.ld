OUTPUT_ARCH(loongarch)
ENTRY(_entry)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x9000000000200000;
  .text : { *(.text.entry) *(.text .text.*) } :text
  . = ALIGN(4096);
  PROVIDE(etext = .);
  .rodata : { *(.rodata .rodata.*) } :text
  .data : { *(.data .data.*) *(.sdata .sdata.*) } :data
  .bss (NOLOAD) : ALIGN(16) {
    bss_begin = .; *(.bss .bss.* .sbss .sbss.* COMMON) bss_end = .;
    . = ALIGN(4096); . += 16384; boot_stack_top = .;
    . += 16384; refill_stack_top = .;
  } :data
  . = ALIGN(4096); PROVIDE(end = .);
  /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
