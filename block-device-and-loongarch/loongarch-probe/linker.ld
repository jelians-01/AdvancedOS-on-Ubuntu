OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  /* 留在 QEMU virt 可用的低地址，供第一阶段构建探查使用。 */
  . = 0x9000000000200000;

  .text : ALIGN(16) {
    *(.text.init)
    *(.text .text.*)
  } :text

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
  } :text

  .data : ALIGN(16) {
    *(.data .data.*)
  } :data

  .bss : ALIGN(16) {
    *(.bss .bss.* COMMON)
  } :data

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
