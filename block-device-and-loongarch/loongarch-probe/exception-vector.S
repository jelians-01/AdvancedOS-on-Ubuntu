.section .text.vector,"ax"
.align 12
.globl exception_vector
.type exception_vector, @function
exception_vector:
  /* 这是一个不依赖栈的最小入口，只输出标志并停住。 */
  li.d  $t0, 0x1fe001e0
  li.w  $t1, 0x45
  st.b  $t1, $t0, 0
  li.w  $t1, 0x0a
  st.b  $t1, $t0, 0
1:
  idle  0
  b     1b
.size exception_vector, .-exception_vector
