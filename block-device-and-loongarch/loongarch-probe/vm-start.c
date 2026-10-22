#include "vm-table.h"

extern char memory_pool_begin[], memory_pool_end[];
extern void vm_refill_vector(void), vm_fault_vector(void);
extern uint64_t vm_test_load(uintptr_t);
extern void vm_test_store(uintptr_t,uint64_t);
extern uint64_t vm_test_fetch(uintptr_t);
extern char vm_load_pc[], vm_load_resume[], vm_store_pc[], vm_store_resume[];
extern char vm_fetch_resume[], vm_payload_begin[], vm_payload_end[];

#define READ_CSR(number) ({ uintptr_t value; \
  __asm__ volatile("csrrd %0, " #number : "=r"(value)); value; })
#define WRITE_CSR(number,input) do { uintptr_t value = (uintptr_t)(input); \
  __asm__ volatile("csrwr %0, " #number : "+r"(value) : : "memory"); } while (0)

static struct probe_vm space;
static volatile size_t refill_count, fault_count;
static volatile struct {
  unsigned active, kind, code;
  uintptr_t address, pc, resume;
} expected;

static void putc_uart(char c)
{
  /* DMW also remains usable while the refill handler temporarily uses DA. */
  *(volatile unsigned char *)(uintptr_t)UINT64_C(0x900000001fe001e0) = c;
}
static void puts_uart(const char *s)
{
  while (*s)
    putc_uart(*s++);
}
static void hex(uintptr_t value)
{
  puts_uart("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc_uart("0123456789abcdef"[(value >> shift) & 15]);
}
static void report(const char *s) { puts_uart(s); puts_uart("\r\n"); }
static void stop(const char *s) __attribute__((noreturn));
static void stop(const char *s)
{
  puts_uart("vm: FAIL "); report(s);
  for (;;)
    __asm__ volatile("idle 0");
}
#define CHECK(condition,message) do { if (!(condition)) stop(message); } while (0)
static void invalidate(void)
{
  __asm__ volatile("dbar 0\n\tinvtlb 0, $zero, $zero\n\tibar 0" ::: "memory");
}

/* Called only by vm-vector.S with all integer registers already preserved.
 * Never allocate or touch a translated test VA while handling an exception.
 */
void vm_exception(unsigned refill)
{
  uintptr_t address = refill ? READ_CSR(0x89) : READ_CSR(0x7);
  uintptr_t era = refill ? (READ_CSR(0x8a) & ~(uintptr_t)3) : READ_CSR(0x6);
  unsigned code = refill ? 0 : (unsigned)((READ_CSR(0x5) >> 16) & 63);
  if (refill && vm_lookup(&space,address)) {
    uintptr_t pair = address & ~(uintptr_t)(VM_PAGE_SIZE * 2 - 1);
    WRITE_CSR(0x8c,vm_lookup(&space,pair));
    WRITE_CSR(0x8d,vm_lookup(&space,pair + VM_PAGE_SIZE));
    WRITE_CSR(0x8e,pair | VM_PAGE_SHIFT);
    __asm__ volatile("tlbfill" ::: "memory");
    ++refill_count;
    return; /* Retry the original faulting instruction after ertn. */
  }
  puts_uart("vm: fault kind="); hex(refill ? 1 : 2);
  puts_uart(" code="); hex(code);
  puts_uart(" badv="); hex(address);
  puts_uart(" pc="); hex(era); puts_uart("\r\n");
  CHECK(expected.active && expected.kind == (refill ? 1U : 2U) &&
        expected.code == code && expected.address == address &&
        expected.pc == era,"unexpected exception");
  ++fault_count;
  expected.active = 0;
  /* Recovery is only for an explicitly armed test instruction, never for an
   * arbitrary fault. Production faults need process termination or recovery.
   */
  if (refill)
    WRITE_CSR(0x8a,expected.resume | 1);
  else
    WRITE_CSR(0x6,expected.resume);
}

static size_t arm(unsigned kind, unsigned code, uintptr_t address,
                  uintptr_t pc, uintptr_t resume)
{
  CHECK(!expected.active,"nested expected fault");
  expected.kind = kind; expected.code = code; expected.address = address;
  expected.pc = pc; expected.resume = resume; expected.active = 1;
  return fault_count;
}
static void handled(size_t previous)
{
  CHECK(!expected.active && fault_count == previous + 1,"fault not observed");
  CHECK((READ_CSR(0x0) & 31) == 16,"paging mode not restored");
}

void vm_main(void)
{
  report("vm: begin (software page walk, 16 KiB, PLV0)");
  CHECK(probe_pages_init(memory_pool_begin,memory_pool_end) == 0,"pool init");
  CHECK(vm_init(&space) == 0,"root allocation");
  uint64_t *first = vm_frame_alloc(), *second = vm_frame_alloc();
  uint64_t *third = vm_frame_alloc();
  unsigned char *codepage = vm_frame_alloc();
  CHECK(first && second && third && codepage,"data page allocation");
  first[0] = 0x12345678; second[0] = 0x22334455; third[0] = 0x55667788;
  size_t code_size = (uintptr_t)vm_payload_end - (uintptr_t)vm_payload_begin;
  CHECK(code_size <= VM_PAGE_SIZE,"payload size");
  for (size_t i = 0; i < code_size; ++i)
    codepage[i] = (unsigned char)vm_payload_begin[i];
  const uintptr_t a = 0x40000000, b = a + VM_PAGE_SIZE, alias = 0x60000000;
  const uintptr_t invalid = 0x40200000, unreadable = 0x40204000;
  const uintptr_t execute = 0x40400000;
  uint64_t rw = VM_VALID | VM_WRITE | VM_NX;
  CHECK(vm_map(&space,a,vm_physical(first),rw) == 0 &&
        vm_map(&space,b,vm_physical(second),rw) == 0 &&
        vm_map(&space,alias,vm_physical(first),rw) == 0 &&
        vm_map(&space,invalid,vm_physical(third),VM_NX) == 0 &&
        vm_map(&space,unreadable,vm_physical(third),rw|VM_NR) == 0 &&
        vm_map(&space,execute,vm_physical(codepage),VM_VALID) == 0,"map setup");

  WRITE_CSR(0xc,(uintptr_t)vm_fault_vector);
  WRITE_CSR(0x88,(uintptr_t)vm_refill_vector);
  WRITE_CSR(0x19,vm_physical(space.root));
  WRITE_CSR(0x18,0); WRITE_CSR(0x1e,VM_PAGE_SHIFT);
  WRITE_CSR(0x180,UINT64_C(0x9000000000000011));
  WRITE_CSR(0x181,0); WRITE_CSR(0x182,0); WRITE_CSR(0x183,0);
  invalidate();
  uintptr_t mode = READ_CSR(0x0);
  mode = (mode & ~(uintptr_t)31) | 16; /* PLV0, IE=0, DA=0, PG=1. */
  WRITE_CSR(0x0,mode);
  __asm__ volatile("ibar 0" ::: "memory");
  CHECK((READ_CSR(0x0) & 31) == 16,"enable paging");
  report("vm: paging enabled DMW kernel PASS");
  uint64_t read_a = vm_test_load(a), read_b = vm_test_load(b);
  CHECK(read_a == first[0] && read_b == second[0],"even odd read");
  vm_test_store(a + VM_PAGE_SIZE - 8,0x11223344);
  CHECK(first[VM_PAGE_SIZE / 8 - 1] == 0x11223344 && second[0] == 0x22334455,"page end write");
  vm_test_store(alias,0x778899);
  CHECK(vm_test_load(a) == 0x778899 && first[0] == 0x778899 && refill_count >= 2,
        "alias or refill");
  report("vm: software walk refill even-odd alias PASS");

  CHECK(vm_protect(&space,a,VM_VALID|VM_NX) == 0,"read only setup"); invalidate();
  size_t previous = arm(2,4,a,(uintptr_t)vm_store_pc,(uintptr_t)vm_store_resume);
  vm_test_store(a,0xdead); handled(previous);
  CHECK(first[0] == 0x778899 && vm_test_load(a) == first[0],"read only corruption");
  report("vm: read-only store fault PME PASS");
  CHECK(vm_protect(&space,a,rw) == 0,"write permission restore"); invalidate();
  vm_test_store(a,0xabcdef);
  CHECK(first[0] == 0xabcdef,"restored write");
  report("vm: permission update and TLB invalidation PASS");

  previous = arm(2,5,unreadable,(uintptr_t)vm_load_pc,(uintptr_t)vm_load_resume);
  (void)vm_test_load(unreadable); handled(previous);
  report("vm: no-read load fault PNR PASS");
  previous = arm(2,1,invalid,(uintptr_t)vm_load_pc,(uintptr_t)vm_load_resume);
  (void)vm_test_load(invalid); handled(previous);
  report("vm: invalid page load fault PIL PASS");

  __asm__ volatile("dbar 0\n\tibar 0" ::: "memory");
  CHECK(vm_test_fetch(execute) == 0x5a,"mapped instruction execution");
  CHECK(vm_protect(&space,execute,VM_VALID|VM_NX) == 0,"NX setup"); invalidate();
  previous = arm(2,6,execute,execute,(uintptr_t)vm_fetch_resume);
  (void)vm_test_fetch(execute); handled(previous);
  report("vm: mapped execution and no-execute fault PNX PASS");

  CHECK(vm_unmap(&space,b) == 0,"unmap"); invalidate();
  previous = arm(1,0,b,(uintptr_t)vm_load_pc,(uintptr_t)vm_load_resume);
  (void)vm_test_load(b); handled(previous);
  report("vm: unmapped refill miss recovery PASS");
  CHECK(vm_unmap(&space,a) == 0 && vm_map(&space,a,vm_physical(third),rw) == 0,
        "remap setup"); invalidate();
  CHECK(vm_test_load(a) == third[0] && first[0] == 0xabcdef,"stale translation");
  vm_test_store(a,0x998877);
  CHECK(third[0] == 0x998877 && first[0] == 0xabcdef,"remap physical isolation");
  report("vm: remap to different physical page PASS");

  invalidate(); /* No translated test access after reclaiming page tables. */
  vm_destroy(&space);
  vm_frame_free(first); vm_frame_free(second);
  vm_frame_free(third); vm_frame_free(codepage);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES,"page reclamation");
  report("vm: table and data page reclamation PASS");
  report("vm: ALL PASS");
  for (;;)
    __asm__ volatile("idle 0");
}
