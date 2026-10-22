.section .text.user,"ax"
/* Preserve page-relative relocations when copying to the user code frame. */
.align 14
.globl user_payload_begin, user_payload_end
.globl user_fault_high, user_fault_priv, user_fault_ro, user_fault_guard
user_payload_begin:
  ori $a7, $zero, 0
  syscall 0
  or $s0, $a0, $zero
  ori $t0, $zero, 2
  bgeu $s0, $t0, user_fault_dispatch
  li.d $s1, 0x41000000
  addi.d $t0, $s0, 0x40
  st.d $t0, $s1, 0
  addi.d $sp, $sp, -16
  addi.d $s3, $s0, 0x500
  st.d $s3, $sp, 0
  la.pcrel $a0, user_message
  ori $a1, $zero, 15
  ori $a7, $zero, 1
  syscall 0
  ori $t0, $zero, 15
  bne $a0, $t0, user_assert_fail
  li.d $a0, 0x41003ffc
  ori $a1, $zero, 8
  ori $a7, $zero, 1
  syscall 0
  ori $t0, $zero, 8
  bne $a0, $t0, user_assert_fail
  li.d $a0, 0x9000000000200000
  ori $a1, $zero, 1
  ori $a7, $zero, 1
  syscall 0
  li.d $t0, -1
  bne $a0, $t0, user_assert_fail
  li.d $a0, 0x41003ffc
  ori $a7, $zero, 4
  syscall 0
  bnez $a0, user_assert_fail
  li.d $t1, 0x41003ffc
  ld.w $t0, $t1, 0
  addi.d $t2, $s0, 0x100
  bne $t0, $t2, user_assert_fail
  ld.w $t0, $t1, 4
  bnez $t0, user_assert_fail
  li.d $a0, 0x40000000
  ori $a7, $zero, 4
  syscall 0
  li.d $t0, -1
  bne $a0, $t0, user_assert_fail
#ifdef USER_DISK
  ori $a0, $zero, 2
  ori $a7, $zero, 7
  syscall 0
  li.d $t0, 0x800000
  bne $a0, $t0, user_assert_fail
  ori $a0, $zero, 2
  ori $a1, $zero, 0
  addi.d $a2, $s1, 128
  ori $a3, $zero, 512
  ori $a7, $zero, 8
  syscall 0
  ori $t0, $zero, 512
  bne $a0, $t0, user_assert_fail
  addi.d $t0, $s1, 128
  ori $t1, $zero, 0
user_check_initial_disk:
  ld.bu $t2, $t0, 0
  ori $t3, $zero, 7
  mul.d $t3, $t3, $t1
  addi.d $t3, $t3, 62
  andi $t3, $t3, 255
  bne $t2, $t3, user_assert_fail
  addi.d $t0, $t0, 1
  addi.d $t1, $t1, 1
  ori $t2, $zero, 512
  bltu $t1, $t2, user_check_initial_disk
  addi.d $t0, $s1, 128
  ori $t1, $zero, 512
  addi.d $t2, $s0, 0x60
user_prepare_disk_write:
  st.b $t2, $t0, 0
  addi.d $t0, $t0, 1
  addi.d $t1, $t1, -1
  bnez $t1, user_prepare_disk_write
  ori $a0, $zero, 2
  slli.d $a1, $s0, 9
  li.d $t0, 8192
  add.d $a1, $a1, $t0
  addi.d $a2, $s1, 128
  ori $a3, $zero, 512
  ori $a7, $zero, 9
  syscall 0
  ori $t0, $zero, 512
  bne $a0, $t0, user_assert_fail
  ori $a0, $zero, 2
  ori $a7, $zero, 10
  syscall 0
  bnez $a0, user_assert_fail
  ori $a0, $zero, 2
  slli.d $a1, $s0, 9
  li.d $t0, 8192
  add.d $a1, $a1, $t0
  addi.d $a2, $s1, 1024
  ori $a3, $zero, 512
  ori $a7, $zero, 8
  syscall 0
  ori $t0, $zero, 512
  bne $a0, $t0, user_assert_fail
  addi.d $t0, $s1, 1024
  ori $t1, $zero, 512
  addi.d $t2, $s0, 0x60
user_check_disk_readback:
  ld.bu $t3, $t0, 0
  bne $t2, $t3, user_assert_fail
  addi.d $t0, $t0, 1
  addi.d $t1, $t1, -1
  bnez $t1, user_check_disk_readback
#endif
  ori $a7, $zero, 999
  syscall 0
  li.d $t0, -1
  bne $a0, $t0, user_assert_fail
  ori $a7, $zero, 2
  syscall 0
  ld.d $t0, $s1, 0
  addi.d $t1, $s0, 0x40
  bne $t0, $t1, user_assert_fail
  ld.d $t0, $sp, 0
  bne $t0, $s3, user_assert_fail
  ori $a7, $zero, 5
  syscall 0
  or $s2, $a0, $zero
user_busy_again:
  li.w $t0, 1000000
1:
  addi.d $t0, $t0, -1
  bnez $t0, 1b
  ori $a7, $zero, 5
  syscall 0
  sub.d $t0, $a0, $s2
  ori $t1, $zero, 4
  bltu $t0, $t1, user_busy_again
  ld.d $t0, $sp, 0
  bne $t0, $s3, user_assert_fail
  ori $a0, $zero, 0
  ori $a7, $zero, 3
  syscall 0
user_assert_fail:
  ori $a7, $zero, 6
  syscall 0
  b user_assert_fail
user_fault_dispatch:
  ori $t0, $zero, 2
  beq $s0, $t0, user_high_test
  ori $t0, $zero, 3
  beq $s0, $t0, user_priv_test
  ori $t0, $zero, 4
  beq $s0, $t0, user_ro_test
  ori $t0, $zero, 5
  beq $s0, $t0, user_nx_test
  li.d $a0, 0x41ffc000
user_fault_guard:
  ld.d $a0, $a0, 0
  b user_assert_fail
user_high_test:
  li.d $a0, 0x9000000000200000
user_fault_high:
  ld.d $a0, $a0, 0
  b user_assert_fail
user_priv_test:
  li.d $a0, 0x43000000
user_fault_priv:
  ld.d $a0, $a0, 0
  b user_assert_fail
user_ro_test:
  li.d $a0, 0x40000000
user_fault_ro:
  st.b $zero, $a0, 0
  b user_assert_fail
user_nx_test:
  li.d $a0, 0x41000000
  jirl $zero, $a0, 0
  b user_assert_fail
user_message:
  .ascii "hello from PLV3"
user_message_end:
.align 2
user_payload_end:
