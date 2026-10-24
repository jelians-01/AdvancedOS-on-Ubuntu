#include <stdint.h>

#define UART_BASE ((uintptr_t)0x1fe001e0UL)

struct context {
  uintptr_t ra;
  uintptr_t sp;
  uintptr_t fp;
  uintptr_t s0;
  uintptr_t s1;
  uintptr_t s2;
  uintptr_t s3;
  uintptr_t s4;
  uintptr_t s5;
  uintptr_t s6;
  uintptr_t s7;
  uintptr_t s8;
};

extern void switch_context(struct context *, struct context *);
extern char stack_top[];
extern char worker_stack_top[];

static struct context main_context;
static struct context worker_context;

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

static void
worker(void)
{
  uart_puts("context: worker\r\n");
  switch_context(&worker_context, &main_context);
  uart_puts("context: worker resumed\r\n");
  switch_context(&worker_context, &main_context);
  for (;;) {
    __asm__ volatile("idle 0");
  }
}

void
context_main(void)
{
  worker_context.ra = (uintptr_t)worker;
  worker_context.sp = (uintptr_t)worker_stack_top;
  uart_puts("context: main\r\n");

  switch_context(&main_context, &worker_context);
  uart_puts("context: main resumed\r\n");
  switch_context(&main_context, &worker_context);
  uart_puts("context: complete\r\n");
  for (;;) {
    __asm__ volatile("idle 0");
  }
}
