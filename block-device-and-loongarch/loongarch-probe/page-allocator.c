#include "page-allocator.h"

static uintptr_t pool_begin;
static size_t page_count;
static size_t free_count;
static unsigned char used[PROBE_MAX_PAGES];

int
probe_pages_init(void *begin, void *end)
{
  uintptr_t first = (uintptr_t)begin;
  uintptr_t last = (uintptr_t)end;
  if (page_count || !first || last <= first ||
      first % PROBE_PAGE_SIZE || last % PROBE_PAGE_SIZE ||
      (last - first) / PROBE_PAGE_SIZE > PROBE_MAX_PAGES)
    return -1;
  pool_begin = first;
  page_count = (last - first) / PROBE_PAGE_SIZE;
  free_count = page_count;
  for (size_t i = 0; i < page_count; ++i)
    used[i] = 0;
  return 0;
}

void *
probe_page_alloc(void)
{
  for (size_t i = 0; i < page_count; ++i) {
    if (!used[i]) {
      used[i] = 1;
      --free_count;
      unsigned char *page = (unsigned char *)(pool_begin + i * PROBE_PAGE_SIZE);
      for (size_t j = 0; j < PROBE_PAGE_SIZE; ++j)
        page[j] = 0;
      return page;
    }
  }
  return NULL;
}

void *
probe_pages_alloc_aligned(size_t count, size_t alignment_pages)
{
  if (!count || count > page_count || !alignment_pages ||
      alignment_pages > PROBE_MAX_PAGES ||
      (alignment_pages & (alignment_pages - 1)))
    return NULL;
  for (size_t i = 0; i <= page_count - count; ++i) {
    uintptr_t address = pool_begin + i * PROBE_PAGE_SIZE;
    if (address % (alignment_pages * PROBE_PAGE_SIZE))
      continue;
    size_t j = 0;
    while (j < count && !used[i + j])
      ++j;
    if (j != count)
      continue;
    for (j = 0; j < count; ++j)
      used[i + j] = 1;
    free_count -= count;
    unsigned char *memory = (unsigned char *)address;
    for (j = 0; j < count * PROBE_PAGE_SIZE; ++j)
      memory[j] = 0;
    return memory;
  }
  return NULL;
}

int
probe_page_free(void *page)
{
  uintptr_t address = (uintptr_t)page;
  if (!page_count || address < pool_begin ||
      address - pool_begin >= page_count * PROBE_PAGE_SIZE ||
      (address - pool_begin) % PROBE_PAGE_SIZE)
    return -1;
  size_t index = (address - pool_begin) / PROBE_PAGE_SIZE;
  if (!used[index])
    return -1;
  used[index] = 0;
  ++free_count;
  return 0;
}

size_t probe_pages_available(void) { return free_count; }
size_t probe_pages_total(void) { return page_count; }
