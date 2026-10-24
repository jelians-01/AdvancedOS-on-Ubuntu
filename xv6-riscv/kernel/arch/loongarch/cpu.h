#ifndef XV6_LOONGARCH_CPU_H
#define XV6_LOONGARCH_CPU_H
#define ARCH_ELF_MACHINE 258
#define LA_DIRECT 0x9000000000000000UL
#define LA_PHYS_MASK 0x0000ffffffffffffUL
#define LA_READ(n) ({ uint64 v; __asm__ volatile("csrrd %0, " #n : "=r"(v)); v; })
#define LA_WRITE(n,x) do { uint64 v=(uint64)(x); __asm__ volatile("csrwr %0, " #n : "+r"(v) :: "memory"); } while(0)
static inline uint64 r_sp(void) { uint64 v; __asm__ volatile("move %0, $sp" : "=r"(v)); return v; }
static inline uint64 r_tp(void) { return 0; } /* Initial port supports one CPU. */
static inline void intr_on(void) { LA_WRITE(0x0,LA_READ(0x0)|4); }
static inline void intr_off(void) { LA_WRITE(0x0,LA_READ(0x0)&~4UL); }
static inline int intr_get(void) { return (LA_READ(0x0)&4)!=0; }
static inline void sfence_vma(void) { __asm__ volatile("dbar 0; invtlb 0, $zero, $zero; ibar 0" ::: "memory"); }
typedef uint64 pte_t;
typedef uint64 *pagetable_t;
#define PGSIZE 4096
#define PGSHIFT 12
#define PGROUNDUP(sz) (((sz)+PGSIZE-1)&~(PGSIZE-1))
#define PGROUNDDOWN(a) ((a)&~(PGSIZE-1))
/* This is the common software PTE format, translated to TLBELO on refill. */
#define PTE_V (1UL<<0)
#define PTE_R (1UL<<1)
#define PTE_W (1UL<<2)
#define PTE_X (1UL<<3)
#define PTE_U (1UL<<4)
#define PA2PTE(pa) (((((uint64)(pa))&LA_PHYS_MASK)>>12)<<10)
#define PTE2PA(pte) (((((pte)>>10)<<12)&LA_PHYS_MASK)|LA_DIRECT)
#define PTE_FLAGS(pte) ((pte)&0x3ff)
#define PXMASK 0x1ff
#define PXSHIFT(level) (PGSHIFT+9*(level))
#define PX(level,va) ((((uint64)(va))>>PXSHIFT(level))&PXMASK)
#define MAXVA (1UL<<38)
#endif
