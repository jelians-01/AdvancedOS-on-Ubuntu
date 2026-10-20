.section .text.init,"ax"
.align 4
.globl _start
.type _start, @function
_start:
  la.abs $sp, stack_top
  b      context_main
.size _start, .-_start
