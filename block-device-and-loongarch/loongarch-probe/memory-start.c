#include "page-allocator.h"

extern char memory_pool_begin[], memory_pool_end[];

static void
report(const char *text)
{
  while (*text)
    *(volatile unsigned char *)(uintptr_t)0x1fe001e0UL = (unsigned char)*text++;
  *(volatile unsigned char *)(uintptr_t)0x1fe001e0UL = '\r';
  *(volatile unsigned char *)(uintptr_t)0x1fe001e0UL = '\n';
}

void
memory_main(void)
{
  report("memory: begin (4 KiB pages, 1 MiB reserved pool)");
  /* Reject bad initialization without changing the empty allocator. */
  int invalid = probe_page_alloc() != NULL || probe_page_free(NULL) != -1 ||
      probe_pages_init(NULL, memory_pool_end) != -1 ||
      probe_pages_init((void *)((uintptr_t)memory_pool_begin + 1), memory_pool_end) != -1 ||
      probe_pages_init(memory_pool_begin, (void *)((uintptr_t)memory_pool_end - 1)) != -1 ||
      probe_pages_init(memory_pool_end, memory_pool_begin) != -1 ||
      probe_pages_init(memory_pool_begin, memory_pool_begin) != -1 ||
      probe_pages_init(memory_pool_begin, (void *)((uintptr_t)memory_pool_end +
                       PROBE_PAGE_SIZE)) != -1;
  if (!invalid && probe_pages_init(memory_pool_begin, memory_pool_end) == 0) {
    report("memory: initialization bounds PASS");
    if (probe_pages_selftest(report) == 0)
      report("memory: ALL PASS");
    else
      report("memory: FAIL selftest");
  } else {
    report("memory: FAIL initialization");
  }
  for (;;)
    __asm__ volatile("idle 0");
}
