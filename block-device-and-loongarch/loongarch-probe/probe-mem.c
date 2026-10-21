#include <stddef.h>
/* GCC may lower aggregate initialization to memset even when freestanding.
 * Volatile accesses prevent these fallback loops from folding recursively.
 */
void *memset(void *destination,int value,size_t length)
{
  volatile unsigned char *p = destination;
  for (size_t i = 0; i < length; ++i) p[i] = (unsigned char)value;
  return destination;
}
