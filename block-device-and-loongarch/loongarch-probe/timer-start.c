#include <stdint.h>

extern void timer_vector(void);

static inline void
write_csr(uintptr_t value, int csr)
{
  __asm__ volatile("csrwr %0, %1" : : "r"(value), "i"(csr) : "memory");
}

static inline void
set_csr_bits(uintptr_t value, uintptr_t mask, int csr)
{
  __asm__ volatile("csrxchg %0, %1, %2"
                   :
                   : "r"(value), "r"(mask), "i"(csr)
                   : "memory");
}

void
_start(void)
{
  /* EENTRY=0xc, ECFG=0x4, CRMD=0x0, TCFG=0x41. */
  write_csr((uintptr_t)timer_vector, 0xc);
  write_csr((uintptr_t)1 << 11, 0x4); /* enable timer interrupt, INT_TI=11 */
  write_csr(((uintptr_t)1000000 << 2) | 0x3, 0x41); /* periodic + enable */
  set_csr_bits((uintptr_t)1 << 2, (uintptr_t)1 << 2, 0x0); /* CRMD.IE */

  for (;;) {
    __asm__ volatile("idle 0");
  }
}
