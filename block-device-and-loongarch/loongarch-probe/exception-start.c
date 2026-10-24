#include <stdint.h>

#define UART_BASE ((uintptr_t)0x1fe001e0UL)

extern void exception_vector(void);

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
  /* LoongArch CSR.EENTRY 编号为 0xc。 */
  __asm__ volatile("csrwr %0, 0xc" : : "r"(address) : "memory");
}

void
_start(void)
{
  write_eentry((uintptr_t)exception_vector);
  uart_puts("exception probe: before syscall\r\n");

  /* syscall 触发一般异常，控制流应转到 exception_vector。 */
  __asm__ volatile("syscall 0" ::: "memory");
  for (;;) {
    __asm__ volatile("idle 0");
  }
}
