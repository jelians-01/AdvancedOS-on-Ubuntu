.section .text.init,"ax"
.align 4
.globl _start
_start:
  csrwr $zero, 0x4
  la.abs $sp, memory_stack_top
  la.abs $t0, memory_bss_begin
  la.abs $t1, memory_bss_end
1:
  bgeu $t0, $t1, 2f
  st.b $zero, $t0, 0
  addi.d $t0, $t0, 1
  b 1b
2:
  b user_main
