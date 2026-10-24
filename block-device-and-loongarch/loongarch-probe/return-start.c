#include <stdint.h>

#define UART_BASE ((uintptr_t)0x1fe001e0UL)

extern void return_vector(void);
extern char stack_top[];

static inline void
uart_putc(char c)
{
  *(volatile uint8_t *)UART_BASE = (uint8_t)c;
}

static void
uart_puts(const char *s)
{
  while (*s != '\0')
    uart_putc(*s++);
}

static inline void
write_eentry(uintptr_t address)
{
  __asm__ volatile("csrwr %0, 0xc" : : "r"(address) : "memory");
}

void
_start(void)
{
  /* 异常入口要保存通用寄存器，因此先建立一个内核栈。 */
  __asm__ volatile("la.abs $sp, stack_top" ::: "memory");
  write_eentry((uintptr_t)return_vector);
  uart_puts("trap return: before syscall\r\n");

  __asm__ volatile("syscall 0" ::: "memory");

  /* return_vector 修改 ERA 并执行 ertn 后，应回到这里。 */
  uart_puts("trap return: after ertn\r\n");
  for (;;) {
    __asm__ volatile("idle 0");
  }
}
