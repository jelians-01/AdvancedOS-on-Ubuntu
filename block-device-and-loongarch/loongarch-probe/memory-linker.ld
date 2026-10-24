OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS {
  . = 0x9000000000200000;
  .text : ALIGN(16) { *(.text.init) *(.text .text.*) } :text
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } :text
  .data : ALIGN(16) { *(.data .data.*) } :data
  .bss (NOLOAD) : ALIGN(16) {
    memory_bss_begin = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
    memory_bss_end = .;
    memory_stack_bottom = .;
    . += 16384;
    memory_stack_top = .;
    . = ALIGN(4096);
    memory_pool_begin = .;
    . += 0x100000;
    memory_pool_end = .;
  } :data
  /* QEMU 7.2.22 virt.c maps the first 256 MiB at physical 0. */
  ASSERT(memory_pool_end <= 0x9000000010000000, "pool exceeds low RAM")
  ASSERT(memory_pool_begin >= memory_stack_top, "pool overlaps stack")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
