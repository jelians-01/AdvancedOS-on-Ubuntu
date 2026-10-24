#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
struct spinlock tickslock;
uint ticks;
extern pagetable_t kernel_pagetable;
extern char loong_vector[],loong_refill[],refill_stack_top[];

void trapinit(void) { initlock(&tickslock,"time"); }
void trapinithart(void) {
  LA_WRITE(0xc,(uint64)loong_vector);
  LA_WRITE(0x88,(uint64)loong_refill & LA_PHYS_MASK);
  LA_WRITE(0x31,(uint64)refill_stack_top);
  LA_WRITE(0x1e,PGSHIFT);
  LA_WRITE(0x4,1UL<<11);
  LA_WRITE(0x44,1);
  LA_WRITE(0x41,(1000000UL<<2)|3);
}
void clockintr(void) {
  LA_WRITE(0x44,1);
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
  uartintr(); /* UART polling is bounded; sleeping readers still wake normally. */
}
void prepare_return(void) {
  struct proc *p=myproc();
  intr_off();
  p->trapframe->prmd=7;
  LA_WRITE(0x30,p->kstack+PGSIZE);
  LA_WRITE(0x19,(uint64)p->pagetable & LA_PHYS_MASK);
  sfence_vma();
}
static uint64 entry(pagetable_t pt,uint64 va) {
  pte_t *p=pt && va<MAXVA ? walk(pt,va,0):0;
  if(!p || !(*p&PTE_V)) return 0;
  uint64 f=*p;
  uint64 hardware=(PTE2PA(f)&LA_PHYS_MASK)|1|16;
  if(f&PTE_W) hardware|=2;
  if(f&PTE_U) hardware|=12;
  if(!(f&PTE_R)) hardware|=1UL<<61;
  if(!(f&PTE_X)) hardware|=1UL<<62;
  return hardware;
}
void loong_refill_c(void) {
  uint64 va=LA_READ(0x89),pair=va&~(2UL*PGSIZE-1);
  struct proc *p=cpus[0].proc;
  pagetable_t pt=va>=KSTACK(NPROC) ? kernel_pagetable:(p?p->pagetable:0);
  LA_WRITE(0x8c,entry(pt,pair));
  LA_WRITE(0x8d,entry(pt,pair+PGSIZE));
  LA_WRITE(0x8e,pair|PGSHIFT);
  __asm__ volatile("tlbfill" ::: "memory");
}
struct trapframe *loong_trap(struct trapframe *frame) {
  uint64 status=LA_READ(0x5),badv=LA_READ(0x7);
  unsigned cause=(status>>16)&63;
  int user=(frame->prmd&3)==3;
  struct proc *p=myproc();
  if(user) {
    *p->trapframe=*frame;
    if(cause==11) {
      if(killed(p)) kexit(-1);
      p->trapframe->epc+=4;
      intr_on();
      syscall();
    } else if(cause==0 && (status&(1UL<<11))) {
      clockintr();
      yield();
    } else if((cause==1 || cause==2) && vmfault(p->pagetable,badv,cause==1)) {
      sfence_vma();
    } else {
      printf("usertrap: cause=%d pid=%d pc=%lx badv=%lx\n",cause,p->pid,frame->epc,badv);
      setkilled(p);
    }
    if(killed(p)) kexit(-1);
    prepare_return();
    return p->trapframe;
  }
  if(cause==0 && (status&(1UL<<11))) {
    clockintr();
    if(p && p->state==RUNNING) yield();
  } else {
    printf("kerneltrap: cause=%d pc=%lx badv=%lx\n",cause,frame->epc,badv);
    panic("loongarch trap");
  }
  return frame;
}
