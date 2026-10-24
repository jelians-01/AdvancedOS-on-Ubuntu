.section .text.start,"ax"
.globl _user_start
_user_start:
  bl main
  bl exit
1: b 1b
