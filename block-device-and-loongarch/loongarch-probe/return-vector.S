.section .text.vector,"ax"
.align 12
.globl return_vector
.type return_vector, @function
return_vector:
  /* 32 个通用寄存器各保存 8 字节；异常入口本身不使用栈外内存。 */
  addi.d $sp, $sp, -256
  st.d   $r0,  $sp, 0
  st.d   $r1,  $sp, 8
  st.d   $r2,  $sp, 16
  st.d   $r3,  $sp, 24
  st.d   $r4,  $sp, 32
  st.d   $r5,  $sp, 40
  st.d   $r6,  $sp, 48
  st.d   $r7,  $sp, 56
  st.d   $r8,  $sp, 64
  st.d   $r9,  $sp, 72
  st.d   $r10, $sp, 80
  st.d   $r11, $sp, 88
  st.d   $r12, $sp, 96
  st.d   $r13, $sp, 104
  st.d   $r14, $sp, 112
  st.d   $r15, $sp, 120
  st.d   $r16, $sp, 128
  st.d   $r17, $sp, 136
  st.d   $r18, $sp, 144
  st.d   $r19, $sp, 152
  st.d   $r20, $sp, 160
  st.d   $r21, $sp, 168
  st.d   $r22, $sp, 176
  st.d   $r23, $sp, 184
  st.d   $r24, $sp, 192
  st.d   $r25, $sp, 200
  st.d   $r26, $sp, 208
  st.d   $r27, $sp, 216
  st.d   $r28, $sp, 224
  st.d   $r29, $sp, 232
  st.d   $r30, $sp, 240
  st.d   $r31, $sp, 248

  /* syscall 指令长 4 字节；跳过它，避免 ertn 后再次触发同一异常。 */
  csrrd  $t0, 0x6
  addi.d $t0, $t0, 4
  csrwr  $t0, 0x6

  li.d   $t0, 0x1fe001e0
  li.w   $t1, 0x52
  st.b   $t1, $t0, 0
  li.w   $t1, 0x0a
  st.b   $t1, $t0, 0

  /* 恢复除 sp 之外的寄存器，再恢复原 sp，最后从异常返回。 */
  ld.d   $r0,  $sp, 0
  ld.d   $r1,  $sp, 8
  ld.d   $r2,  $sp, 16
  ld.d   $r4,  $sp, 32
  ld.d   $r5,  $sp, 40
  ld.d   $r6,  $sp, 48
  ld.d   $r7,  $sp, 56
  ld.d   $r8,  $sp, 64
  ld.d   $r9,  $sp, 72
  ld.d   $r10, $sp, 80
  ld.d   $r11, $sp, 88
  ld.d   $r12, $sp, 96
  ld.d   $r13, $sp, 104
  ld.d   $r14, $sp, 112
  ld.d   $r15, $sp, 120
  ld.d   $r16, $sp, 128
  ld.d   $r17, $sp, 136
  ld.d   $r18, $sp, 144
  ld.d   $r19, $sp, 152
  ld.d   $r20, $sp, 160
  ld.d   $r21, $sp, 168
  ld.d   $r22, $sp, 176
  ld.d   $r23, $sp, 184
  ld.d   $r24, $sp, 192
  ld.d   $r25, $sp, 200
  ld.d   $r26, $sp, 208
  ld.d   $r27, $sp, 216
  ld.d   $r28, $sp, 224
  ld.d   $r29, $sp, 232
  ld.d   $r30, $sp, 240
  ld.d   $r31, $sp, 248
  ld.d   $r3,  $sp, 24
  ertn
.size return_vector, .-return_vector
