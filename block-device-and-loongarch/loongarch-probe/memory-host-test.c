#include <stdio.h>
#include "page-allocator.h"

static unsigned char pool[PROBE_PAGE_SIZE * PROBE_MAX_PAGES]
    __attribute__((aligned(PROBE_PAGE_SIZE)));

static void report(const char *text) { puts(text); }

int
main(void)
{
  if (probe_page_alloc() != NULL || probe_page_free(NULL) != -1 ||
      probe_pages_init(NULL, pool + sizeof pool) != -1 ||
      probe_pages_init(pool + 1, pool + sizeof pool) != -1 ||
      probe_pages_init(pool, pool + sizeof pool - 1) != -1 ||
      probe_pages_init(pool + sizeof pool, pool) != -1 ||
      probe_pages_init(pool, pool) != -1 ||
      probe_pages_init(pool, (void *)((uintptr_t)pool + sizeof pool +
                       PROBE_PAGE_SIZE)) != -1 ||
      probe_pages_init(pool, pool + sizeof pool) != 0)
    return 1;
  report("memory: initialization bounds PASS");
  int result = probe_pages_selftest(report);
  if (result) {
    fprintf(stderr, "memory: FAIL selftest %d\n", result);
    return result;
  }
  report("memory: ALL PASS");
  return 0;
}
