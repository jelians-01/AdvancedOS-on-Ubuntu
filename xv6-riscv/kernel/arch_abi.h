#ifndef XV6_ARCH_ABI_H
#define XV6_ARCH_ABI_H
#ifdef ARCH_LOONGARCH
static inline uint64 arch_arg(struct trapframe *f, int n) { return f->r[4+n]; }
static inline uint64 arch_sysno(struct trapframe *f) { return f->r[11]; }
static inline void arch_result(struct trapframe *f, uint64 n) { f->r[4]=n; }
static inline void arch_exec_frame(struct trapframe *f, uint64 pc, uint64 sp, uint64 argv) { f->epc=pc; f->r[3]=sp; f->r[5]=argv; }
static inline void arch_idle(void) { __asm__ volatile("idle 0"); }
#else
static inline uint64 arch_arg(struct trapframe *f, int n) {
  switch(n) { case 0:return f->a0; case 1:return f->a1; case 2:return f->a2;
    case 3:return f->a3; case 4:return f->a4; default:return f->a5; }
}
static inline uint64 arch_sysno(struct trapframe *f) { return f->a7; }
static inline void arch_result(struct trapframe *f, uint64 n) { f->a0=n; }
static inline void arch_exec_frame(struct trapframe *f, uint64 pc, uint64 sp, uint64 argv) { f->epc=pc; f->sp=sp; f->a1=argv; }
static inline void arch_idle(void) { __asm__ volatile("wfi"); }
#endif
#endif
