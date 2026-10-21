.section .text.vector,"ax"
.align 12
.globl timer_vector
.type timer_vector, @function
timer_vector:
  li.d  $t0, 0x1fe001e0
  li.w  $t1, 0x54
  st.b  $t1, $t0, 0
  li.w  $t1, 0x0a
  st.b  $t1, $t0, 0
  li.w  $t0, 1
  csrwr $t0, 0x44
1:
  idle  0
  b     1b
.size timer_vector, .-timer_vector
