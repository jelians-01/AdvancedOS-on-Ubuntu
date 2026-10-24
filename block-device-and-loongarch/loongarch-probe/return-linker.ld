OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x9000000000200000;

  .text : ALIGN(16) { *(.text.init) } :text
  .text.vector ALIGN(4096) : { *(.text.vector) } :text
  .text.rest : ALIGN(16) { *(.text .text.*) } :text
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } :text

  .data : ALIGN(16) { *(.data .data.*) } :data
  .bss : ALIGN(16) {
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
    stack_bottom = .;
    . += 4096;
    stack_top = .;
  } :data

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
