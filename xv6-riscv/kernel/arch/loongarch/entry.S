.section .text.entry,"ax"
.globl _entry
_entry:
  csrwr $zero, 0x4
  la.abs $sp, boot_stack_top
  la.abs $t0, bss_begin
  la.abs $t1, bss_end
1:
  bgeu $t0, $t1, 2f
  st.b $zero, $t0, 0
  addi.d $t0, $t0, 1
  b 1b
2:
  b main
