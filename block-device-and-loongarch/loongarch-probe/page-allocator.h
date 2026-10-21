#ifndef PROBE_PAGE_ALLOCATOR_H
#define PROBE_PAGE_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#define PROBE_PAGE_SIZE 4096UL
#define PROBE_MAX_PAGES 256UL

/* Single CPU, interrupts disabled. Addresses are directly accessible pointers.
 * The caller owns and reserves this range; no firmware memory discovery here.
 * Reinitialization is rejected. Failed initialization leaves the state intact.
 */
int probe_pages_init(void *begin, void *end);
void *probe_page_alloc(void);
int probe_page_free(void *page);
size_t probe_pages_available(void);
size_t probe_pages_total(void);

/* Shared behavioral checks, run on the target and on a sanitizer host build. */
int probe_pages_selftest(void (*report)(const char *));

#endif
