.section .text.vector,"ax"
/* SAVE0/SAVE1 hold trusted kernel stacks, never dereference a user SP. */
.align 12
.globl user_normal_vector
user_normal_vector:
  csrwr $sp, 0x30
  addi.d $sp, $sp, -288
  st.d $zero, $sp, 0
  st.d $r1, $sp, 8
  st.d $r2, $sp, 16
  st.d $r4, $sp, 32
  st.d $r5, $sp, 40
  st.d $r6, $sp, 48
  st.d $r7, $sp, 56
  st.d $r8, $sp, 64
  st.d $r9, $sp, 72
  st.d $r10, $sp, 80
  st.d $r11, $sp, 88
  st.d $r12, $sp, 96
  st.d $r13, $sp, 104
  st.d $r14, $sp, 112
  st.d $r15, $sp, 120
  st.d $r16, $sp, 128
  st.d $r17, $sp, 136
  st.d $r18, $sp, 144
  st.d $r19, $sp, 152
  st.d $r20, $sp, 160
  st.d $r21, $sp, 168
  st.d $r22, $sp, 176
  st.d $r23, $sp, 184
  st.d $r24, $sp, 192
  st.d $r25, $sp, 200
  st.d $r26, $sp, 208
  st.d $r27, $sp, 216
  st.d $r28, $sp, 224
  st.d $r29, $sp, 232
  st.d $r30, $sp, 240
  st.d $r31, $sp, 248
  csrrd $t0, 0x30
  st.d $t0, $sp, 24
  csrrd $t0, 0x6
  st.d $t0, $sp, 256
  csrrd $t0, 0x1
  st.d $t0, $sp, 264
  or $a0, $sp, $zero
  ori $a1, $zero, 0
  bl user_trap
  b user_restore_0
.align 12
.globl user_refill_vector
user_refill_vector:
  csrwr $sp, 0x31
  addi.d $sp, $sp, -288
  st.d $zero, $sp, 0
  st.d $r1, $sp, 8
  st.d $r2, $sp, 16
  st.d $r4, $sp, 32
  st.d $r5, $sp, 40
  st.d $r6, $sp, 48
  st.d $r7, $sp, 56
  st.d $r8, $sp, 64
  st.d $r9, $sp, 72
  st.d $r10, $sp, 80
  st.d $r11, $sp, 88
  st.d $r12, $sp, 96
  st.d $r13, $sp, 104
  st.d $r14, $sp, 112
  st.d $r15, $sp, 120
  st.d $r16, $sp, 128
  st.d $r17, $sp, 136
  st.d $r18, $sp, 144
  st.d $r19, $sp, 152
  st.d $r20, $sp, 160
  st.d $r21, $sp, 168
  st.d $r22, $sp, 176
  st.d $r23, $sp, 184
  st.d $r24, $sp, 192
  st.d $r25, $sp, 200
  st.d $r26, $sp, 208
  st.d $r27, $sp, 216
  st.d $r28, $sp, 224
  st.d $r29, $sp, 232
  st.d $r30, $sp, 240
  st.d $r31, $sp, 248
  csrrd $t0, 0x31
  st.d $t0, $sp, 24
  csrrd $t0, 0x8a
  bstrins.d $t0, $zero, 1, 0
  st.d $t0, $sp, 256
  csrrd $t0, 0x8f
  st.d $t0, $sp, 264
  or $a0, $sp, $zero
  ori $a1, $zero, 1
  bl user_trap
  b user_restore_1
.section .text,"ax"
.align 4
.globl user_launch
user_launch:
  b user_restore_0
user_restore_0:
  or $sp, $a0, $zero
  la.abs $t0, memory_stack_top
  csrwr $t0, 0x30
  la.abs $t0, memory_stack_top
  csrwr $t0, 0x31
  ld.d $t0, $sp, 256
  csrwr $t0, 0x6
  ld.d $t0, $sp, 264
  csrwr $t0, 0x1
  ld.d $r1, $sp, 8
  ld.d $r2, $sp, 16
  ld.d $r4, $sp, 32
  ld.d $r5, $sp, 40
  ld.d $r6, $sp, 48
  ld.d $r7, $sp, 56
  ld.d $r8, $sp, 64
  ld.d $r9, $sp, 72
  ld.d $r10, $sp, 80
  ld.d $r11, $sp, 88
  ld.d $r12, $sp, 96
  ld.d $r13, $sp, 104
  ld.d $r14, $sp, 112
  ld.d $r15, $sp, 120
  ld.d $r16, $sp, 128
  ld.d $r17, $sp, 136
  ld.d $r18, $sp, 144
  ld.d $r19, $sp, 152
  ld.d $r20, $sp, 160
  ld.d $r21, $sp, 168
  ld.d $r22, $sp, 176
  ld.d $r23, $sp, 184
  ld.d $r24, $sp, 192
  ld.d $r25, $sp, 200
  ld.d $r26, $sp, 208
  ld.d $r27, $sp, 216
  ld.d $r28, $sp, 224
  ld.d $r29, $sp, 232
  ld.d $r30, $sp, 240
  ld.d $r31, $sp, 248
  ld.d $sp, $sp, 24
  ertn
user_restore_1:
  or $sp, $a0, $zero
  la.abs $t0, memory_stack_top
  csrwr $t0, 0x30
  la.abs $t0, memory_stack_top
  csrwr $t0, 0x31
  ld.d $t0, $sp, 256
  ori $t0, $t0, 1
  csrwr $t0, 0x8a
  ld.d $t0, $sp, 264
  csrwr $t0, 0x8f
  ld.d $r1, $sp, 8
  ld.d $r2, $sp, 16
  ld.d $r4, $sp, 32
  ld.d $r5, $sp, 40
  ld.d $r6, $sp, 48
  ld.d $r7, $sp, 56
  ld.d $r8, $sp, 64
  ld.d $r9, $sp, 72
  ld.d $r10, $sp, 80
  ld.d $r11, $sp, 88
  ld.d $r12, $sp, 96
  ld.d $r13, $sp, 104
  ld.d $r14, $sp, 112
  ld.d $r15, $sp, 120
  ld.d $r16, $sp, 128
  ld.d $r17, $sp, 136
  ld.d $r18, $sp, 144
  ld.d $r19, $sp, 152
  ld.d $r20, $sp, 160
  ld.d $r21, $sp, 168
  ld.d $r22, $sp, 176
  ld.d $r23, $sp, 184
  ld.d $r24, $sp, 192
  ld.d $r25, $sp, 200
  ld.d $r26, $sp, 208
  ld.d $r27, $sp, 216
  ld.d $r28, $sp, 224
  ld.d $r29, $sp, 232
  ld.d $r30, $sp, 240
  ld.d $r31, $sp, 248
  ld.d $sp, $sp, 24
  ertn
