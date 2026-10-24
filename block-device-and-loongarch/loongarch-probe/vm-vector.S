.section .text.vector,"ax"
/* Both vectors preserve integer state before calling C. No nested IRQs,
 * floating-point state or user-stack switching are supported here. */
.align 12
.globl vm_refill_vector
.type vm_refill_vector, @function
vm_refill_vector:
  addi.d $sp, $sp, -256
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
  ori $a0, $zero, 1
  bl vm_exception
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
  addi.d $sp, $sp, 256
  ertn
.size vm_refill_vector, .-vm_refill_vector
.align 12
.globl vm_fault_vector
.type vm_fault_vector, @function
vm_fault_vector:
  addi.d $sp, $sp, -256
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
  ori $a0, $zero, 0
  bl vm_exception
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
  addi.d $sp, $sp, 256
  ertn
.size vm_fault_vector, .-vm_fault_vector
.section .text,"ax"
.align 4
.globl vm_test_load, vm_load_pc, vm_load_resume
vm_test_load:
vm_load_pc:
  ld.d $a0, $a0, 0
vm_load_resume:
  jr $ra
.globl vm_test_store, vm_store_pc, vm_store_resume
vm_test_store:
vm_store_pc:
  st.d $a1, $a0, 0
vm_store_resume:
  jr $ra
.globl vm_test_fetch, vm_fetch_resume
vm_test_fetch:
  jirl $zero, $a0, 0
vm_fetch_resume:
  jr $ra
.globl vm_payload_begin, vm_payload_end
vm_payload_begin:
  ori $a0, $zero, 0x5a
  jr $ra
vm_payload_end:
