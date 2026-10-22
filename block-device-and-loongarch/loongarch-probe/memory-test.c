#include "page-allocator.h"

static unsigned char *pages[PROBE_MAX_PAGES];

int
probe_pages_selftest(void (*report)(const char *))
{
  size_t count = probe_pages_total();
  if (count != PROBE_MAX_PAGES || probe_pages_available() != count)
    return 1;
  /* Allocate the entire pool; check every byte, address and prior allocation. */
  for (size_t i = 0; i < count; ++i) {
    pages[i] = probe_page_alloc();
    if (!pages[i] || (uintptr_t)pages[i] % PROBE_PAGE_SIZE ||
        probe_pages_available() != count - i - 1)
      return 2;
    for (size_t j = 0; j < i; ++j)
      if (pages[i] == pages[j])
        return 3;
    for (size_t j = 0; j < PROBE_PAGE_SIZE; ++j) {
      if (pages[i][j] != 0)
        return 4;
      pages[i][j] = (unsigned char)(i % 255 + 1);
    }
  }
  report("memory: alignment uniqueness zero-fill PASS");
  if (probe_page_alloc() != NULL || probe_pages_available() != 0)
    return 5;
  report("memory: exhaustion PASS");
  for (size_t i = 0; i < count; ++i)
    for (size_t j = 0; j < PROBE_PAGE_SIZE; ++j)
      if (pages[i][j] != (unsigned char)(i % 255 + 1))
        return 6;
  report("memory: page isolation PASS");
  uintptr_t first = (uintptr_t)pages[0];
  if (probe_page_free(NULL) != -1 ||
      probe_page_free((void *)(first - PROBE_PAGE_SIZE)) != -1 ||
      probe_page_free((void *)(first + count * PROBE_PAGE_SIZE)) != -1 ||
      probe_page_free(pages[0] + 1) != -1 ||
      probe_page_free((void *)UINTPTR_MAX) != -1 ||
      probe_pages_init(pages[0], (void *)(first + count * PROBE_PAGE_SIZE)) != -1 ||
      probe_pages_available() != 0)
    return 7;
  report("memory: invalid free and reinit rejection PASS");
  if (probe_page_free(pages[17]) || probe_page_free(pages[17]) != -1 ||
      probe_pages_available() != 1)
    return 8;
  unsigned char *reused = probe_page_alloc();
  if (reused != pages[17] || probe_pages_available() != 0)
    return 9;
  for (size_t j = 0; j < PROBE_PAGE_SIZE; ++j)
    if (reused[j] != 0)
      return 10;
  report("memory: double free rejection and reuse clearing PASS");
  for (size_t i = count; i > 0; --i)
    if (probe_page_free(pages[i - 1]))
      return 11;
  if (probe_pages_available() != count)
    return 12;
  /* Repeated full cycles catch bookkeeping leaks and overlapping allocations. */
  for (size_t cycle = 0; cycle < 8; ++cycle) {
    for (size_t i = 0; i < count; ++i) {
      pages[i] = probe_page_alloc();
      if (!pages[i])
        return 13;
    }
    if (probe_page_alloc() || probe_pages_available())
      return 14;
    for (size_t i = 0; i < count; ++i)
      if (probe_page_free(pages[i]))
        return 15;
  }
  if (probe_pages_available() != count)
    return 16;
  report("memory: eight full allocation cycles PASS");
  return 0;
}
