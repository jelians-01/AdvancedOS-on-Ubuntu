OUTPUT_ARCH(loongarch)
ENTRY(_user_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0;
  .text : { *(.text.start) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  . = ALIGN(4096);
  .data : { *(.data .data.* .sdata .sdata.*) } :data
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) } :data
  PROVIDE(end = .);
  /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
