#include "user-copy.h"
#ifdef USER_DISK
#include "pci-block.h"
static unsigned disk_reads,disk_writes,disk_flushes;
#endif

/* Experiment-local processes and syscall numbers, not xv6's public ABI. */
#define TASKS 7
#define U_CODE UINT64_C(0x40000000)
#define U_DATA UINT64_C(0x41000000)
#define U_STACK UINT64_C(0x42000000)
#define K_SECRET UINT64_C(0x43000000)
#define K_HIGH UINT64_C(0x9000000000200000)
#define READ_CSR(n) ({ uintptr_t v; __asm__ volatile("csrrd %0, " #n : "=r"(v)); v; })
#define WRITE_CSR(n,x) do { uintptr_t v = (uintptr_t)(x); \
  __asm__ volatile("csrwr %0, " #n : "+r"(v) :: "memory"); } while (0)
struct frame { uint64_t r[32], era, prmd; };
struct task {
  struct probe_vm vm;
  struct frame frame;
  void *data[3];
  unsigned done, entered, timers, status, faulted;
};
static struct task tasks[TASKS];
static struct frame finish_frame;
static unsigned current;
static uint64_t ticks, timer_switches, yields, write_ok, copies_ok, copy_rejects;
static void *code_frame, *secret_frame;
extern char memory_pool_begin[],memory_pool_end[],memory_stack_top[];
extern char user_payload_begin[],user_payload_end[];
extern char user_fault_high[],user_fault_priv[],user_fault_ro[],user_fault_guard[];
extern void user_normal_vector(void),user_refill_vector(void),user_launch(struct frame *);

static void putc_uart(char c)
{ *(volatile unsigned char *)(uintptr_t)UINT64_C(0x900000001fe001e0) = (unsigned char)c; }
static void puts_uart(const char *s) { while (*s) putc_uart(*s++); }
static void report(const char *s) { puts_uart(s); puts_uart("\r\n"); }
static void hex(uint64_t v)
{
  puts_uart("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc_uart("0123456789abcdef"[(v >> shift) & 15]);
}
static void stop(const char *s) __attribute__((noreturn));
static void stop(const char *s)
{
  puts_uart("user: FAIL "); report(s);
  for (;;) __asm__ volatile("idle 0");
}
#define CHECK(c,s) do { if (!(c)) stop(s); } while (0)
static void flush(void)
{ __asm__ volatile("dbar 0\n\tinvtlb 0, $zero, $zero\n\tibar 0" ::: "memory"); }
static void copy_frame(struct frame *to,const struct frame *from)
{ for (size_t i = 0; i < 32; ++i) to->r[i] = from->r[i]; to->era = from->era; to->prmd = from->prmd; }
static uintptr_t user_pc(const char *label)
{ return U_CODE + (uintptr_t)label - (uintptr_t)user_payload_begin; }

void user_finished(void) __attribute__((noreturn));
void user_finished(void)
{
  WRITE_CSR(0x41,0); WRITE_CSR(0x4,0); WRITE_CSR(0x44,1);
  CHECK((READ_CSR(0x0) & 7) == 0,"kernel finish privilege");
  for (unsigned i = 0; i < TASKS; ++i)
    CHECK(tasks[i].done && tasks[i].entered,"task never entered user mode");
  CHECK(!tasks[0].status && !tasks[1].status && !tasks[0].faulted && !tasks[1].faulted,
        "normal process exit");
  CHECK(tasks[0].timers && tasks[1].timers && timer_switches >= 2 && yields >= 2,
        "timer preemption not observed");
  CHECK(write_ok == 4 && copies_ok == 2 && copy_rejects == 4,"syscall validation counts");
  for (unsigned i = 2; i < TASKS; ++i) CHECK(tasks[i].faulted,"isolation case missed");
  report("user: PLV3 syscalls cross-page copies and register stacks PASS");
  report("user: two address spaces and timer preemption PASS");
  report("user: kernel window privileged page RO NX guard isolation PASS");
#ifdef USER_DISK
  CHECK(disk_reads == 4 && disk_writes == 2 && disk_flushes == 2,"user disk syscall counts");
  report("user: PLV3 syscall D1 PCI disk roundtrip PASS");
#endif
  puts_uart("user: timer ticks="); hex(ticks); puts_uart(" switches="); hex(timer_switches); puts_uart("\r\n");
  flush();
  for (unsigned i = 0; i < TASKS; ++i) {
    vm_destroy(&tasks[i].vm);
    for (unsigned j = 0; j < 3; ++j) vm_frame_free(tasks[i].data[j]);
  }
  vm_frame_free(code_frame); vm_frame_free(secret_frame);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES,"runtime memory leak");
  report("user: all process memory reclaimed PASS");
  report("user: ALL PASS");
  for (;;) __asm__ volatile("idle 0");
}

static struct frame *schedule(unsigned timer)
{
  unsigned previous = current;
  for (unsigned step = 1; step <= TASKS; ++step) {
    unsigned next = (previous + step) % TASKS;
    if (!tasks[next].done) {
      current = next;
      if (timer && next != previous) ++timer_switches;
      WRITE_CSR(0x19,vm_physical(tasks[current].vm.root));
      flush();
      return &tasks[current].frame;
    }
  }
  return &finish_frame;
}

struct frame *user_trap(struct frame *frame,unsigned refill)
{
  struct task *task = &tasks[current];
  CHECK((frame->prmd & 3) == 3 && !task->done,"trap not from live PLV3 task");
  CHECK(frame->r[3] > U_STACK && frame->r[3] <= U_STACK + VM_PAGE_SIZE,
        "user stack range");
  uintptr_t address = refill ? READ_CSR(0x89) : READ_CSR(0x7);
  unsigned code = refill ? 0 : (unsigned)((READ_CSR(0x5) >> 16) & 63);
  if (refill && vm_lookup(&task->vm,address)) {
    uintptr_t pair = address & ~(uintptr_t)(2 * VM_PAGE_SIZE - 1);
    WRITE_CSR(0x8c,vm_lookup(&task->vm,pair));
    WRITE_CSR(0x8d,vm_lookup(&task->vm,pair + VM_PAGE_SIZE));
    WRITE_CSR(0x8e,pair | VM_PAGE_SHIFT);
    __asm__ volatile("tlbfill" ::: "memory");
    return frame;
  }
  copy_frame(&task->frame,frame);
  if (!refill && code == 0) {
    CHECK(READ_CSR(0x5) & (1U << 11),"unexpected interrupt");
    WRITE_CSR(0x44,1);
    ++ticks; ++task->timers;
    return schedule(1);
  }
  if (!refill && code == 11) {
    task->frame.era += 4;
    uint64_t number = frame->r[11], result = UINT64_MAX;
    switch (number) {
    case 0:
      task->entered = 1; result = current; break;
    case 1: {
      unsigned char buffer[64];
      size_t length = (size_t)frame->r[5];
      if (length <= sizeof buffer &&
          user_copyin(&task->vm,buffer,frame->r[4],length) == 0) {
        puts_uart("user: write pid="); hex(current); puts_uart(" ");
        for (size_t i = 0; i < length; ++i) putc_uart((char)buffer[i]);
        puts_uart("\r\n");
        ++write_ok; result = length;
      } else ++copy_rejects;
      break;
    }
    case 2:
      ++yields; task->frame.r[4] = 0; return schedule(0);
    case 3:
      CHECK(current < 2,"faulting task unexpectedly exited");
      task->status = (unsigned)frame->r[4]; task->done = 1;
      return schedule(0);
    case 4: {
      uint64_t value = current + 0x100;
      if (user_copyout(&task->vm,frame->r[4],&value,sizeof value) == 0) {
        result = 0; ++copies_ok;
      } else ++copy_rejects;
      break;
    }
    case 5: result = ticks; break;
    case 6: stop("user payload assertion");
#ifdef USER_DISK
    case 7: {
      uint64_t bytes;
      if (frame->r[4] < 1 || frame->r[4] > 3) break;
      if (blk_size((unsigned)frame->r[4],&bytes) == 0) result = bytes;
      break;
    }
    case 8: {
      unsigned char buffer[512];
      size_t length = (size_t)frame->r[7];
      if (frame->r[4] < 1 || frame->r[4] > 3) break;
      if (length <= sizeof buffer &&
          blk_read((unsigned)frame->r[4],frame->r[5],buffer,(unsigned)length) == 0 &&
          user_copyout(&task->vm,frame->r[6],buffer,length) == 0) {
        result = length; ++disk_reads;
      }
      break;
    }
    case 9: {
      unsigned char buffer[512];
      size_t length = (size_t)frame->r[7];
      if (frame->r[4] < 1 || frame->r[4] > 3) break;
      if (length <= sizeof buffer &&
          user_copyin(&task->vm,buffer,frame->r[6],length) == 0 &&
          blk_write((unsigned)frame->r[4],frame->r[5],buffer,(unsigned)length) == 0) {
        result = length; ++disk_writes;
      }
      break;
    }
    case 10:
      if (frame->r[4] < 1 || frame->r[4] > 3) break;
      if (blk_flush((unsigned)frame->r[4]) == 0) { result = 0; ++disk_flushes; }
      break;
#endif
    default: break; /* Unknown syscall returns -1. */
    }
    task->frame.r[4] = result;
    return &task->frame;
  }
  uintptr_t want_address = 0, want_pc = 0;
  unsigned want_code = 0, want_refill = 0;
  switch (current) {
  case 2: want_address = K_HIGH; want_pc = user_pc(user_fault_high); want_code = 8; break;
  case 3: want_address = K_SECRET; want_pc = user_pc(user_fault_priv); want_code = 7; break;
  case 4: want_address = U_CODE; want_pc = user_pc(user_fault_ro); want_code = 4; break;
  case 5: want_address = U_DATA; want_pc = U_DATA; want_code = 6; break;
  case 6: want_address = U_STACK - VM_PAGE_SIZE; want_pc = user_pc(user_fault_guard); want_refill = 1; break;
  default: stop("normal task faulted");
  }
  puts_uart("user: isolated pid="); hex(current); puts_uart(" cause="); hex(code);
  puts_uart(" badv="); hex(address); puts_uart(" pc="); hex(frame->era); puts_uart("\r\n");
  CHECK(refill == want_refill && code == want_code && address == want_address &&
        frame->era == want_pc,"unexpected isolation fault");
  task->done = 1; task->faulted = 1;
  return schedule(0); /* Only this process is terminated. */
}

void user_main(void)
{
  report("user: begin (PLV3, seven isolated processes)");
#ifdef USER_DISK
  blk_init(); CHECK(pci_block_count() == 3 && pci_block_readonly(3),"user disk initialization");
#endif
  CHECK(probe_pages_init(memory_pool_begin,memory_pool_end) == 0,"pool init");
  code_frame = vm_frame_alloc(); secret_frame = vm_frame_alloc();
  CHECK(code_frame && secret_frame,"shared frame allocation");
  size_t code_size = (uintptr_t)user_payload_end - (uintptr_t)user_payload_begin;
  CHECK(code_size <= VM_PAGE_SIZE,"payload size");
  for (size_t i = 0; i < code_size; ++i)
    ((unsigned char *)code_frame)[i] = (unsigned char)user_payload_begin[i];
  const unsigned char boundary[] = "BOUNDARY";
  for (unsigned i = 0; i < TASKS; ++i) {
    struct task *t = &tasks[i];
    CHECK(vm_init(&t->vm) == 0,"process root");
    for (unsigned j = 0; j < 3; ++j) {
      t->data[j] = vm_frame_alloc(); CHECK(t->data[j],"process frame");
    }
    CHECK(vm_map(&t->vm,U_CODE,vm_physical(code_frame),VM_VALID|VM_USER) == 0 &&
          vm_map(&t->vm,U_DATA,vm_physical(t->data[0]),VM_VALID|VM_USER|VM_WRITE|VM_NX) == 0 &&
          vm_map(&t->vm,U_DATA+VM_PAGE_SIZE,vm_physical(t->data[1]),VM_VALID|VM_USER|VM_WRITE|VM_NX) == 0 &&
          vm_map(&t->vm,U_STACK,vm_physical(t->data[2]),VM_VALID|VM_USER|VM_WRITE|VM_NX) == 0 &&
          vm_map(&t->vm,K_SECRET,vm_physical(secret_frame),VM_VALID|VM_NX) == 0,"process mappings");
    for (size_t j = 0; j < 8; ++j) {
      unsigned char *p = j < 4 ? t->data[0] : t->data[1];
      p[j < 4 ? VM_PAGE_SIZE - 4 + j : j - 4] = boundary[j];
    }
    t->frame.era = U_CODE; t->frame.prmd = 7;
    t->frame.r[3] = U_STACK + VM_PAGE_SIZE;
  }
  finish_frame.era = (uintptr_t)user_finished;
  finish_frame.r[3] = (uintptr_t)memory_stack_top;
  WRITE_CSR(0xc,(uintptr_t)user_normal_vector);
  WRITE_CSR(0x88,(uintptr_t)user_refill_vector);
  WRITE_CSR(0x19,vm_physical(tasks[0].vm.root));
  WRITE_CSR(0x18,0); WRITE_CSR(0x1e,VM_PAGE_SHIFT);
  WRITE_CSR(0x180,UINT64_C(0x9000000000000011));
  WRITE_CSR(0x181,0); WRITE_CSR(0x182,0); WRITE_CSR(0x183,0);
  WRITE_CSR(0x4,1U << 11); WRITE_CSR(0x44,1);
  WRITE_CSR(0x41,(UINT64_C(1000000) << 2) | 3);
  flush();
  WRITE_CSR(0x0,(READ_CSR(0x0) & ~(uintptr_t)31) | 16);
  user_launch(&tasks[0].frame);
  stop("user launch returned");
}
