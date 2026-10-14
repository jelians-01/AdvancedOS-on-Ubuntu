/*
 * LoongArch virt 最小启动探查。
 *
 * 这个程序不是 xv6 内核，只验证三件事：
 *   1. LoongArch 交叉编译器能生成目标文件；
 *   2. 链接脚本能生成可加载的 ELF；
 *   3. 入口代码可以向 QEMU virt 的串口寄存器写入一行文字。
 *
 * QEMU virt 的串口地址来自 LoongArch virt 平台约定；正式接入内核前，
 * 仍需以 QEMU 启动日志和平台文档再次确认。
 */

#include <stdint.h>

#define UART_BASE ((uintptr_t)0x1fe001e0UL)

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

void
_start(void)
{
  uart_puts("loongarch probe: hello\r\n");
  for (;;) {
    __asm__ volatile("idle 0");
  }
}
