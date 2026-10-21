#include "pci-block.h"
#ifndef DISK_PHASE
#define DISK_PHASE 1
#endif
#ifndef EXPECTED_DISKS
#define EXPECTED_DISKS 3
#endif
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
{ puts_uart("disk: FAIL "); report(s); for (;;) __asm__ volatile("idle 0"); }
#define CHECK(c,s) do { if (!(c)) stop(s); } while (0)
static const unsigned char marker[] = "LOONGARCH-D1-PERSIST";
void disk_main(void)
{
  report("disk: begin (PCI VirtIO, synchronous D1)");
  blk_init(); blk_init(); /* One-time initialization is idempotent. */
  unsigned count = pci_block_count();
  if (!count) {
    uint64_t bytes;
    CHECK(blk_size(1,&bytes) == -1 && blk_read(1,0,&bytes,1) == -1,"no-device API");
    report("disk: no device handled PASS");
    for (;;) __asm__ volatile("idle 0");
  }
  CHECK(count == EXPECTED_DISKS,"device discovery count");
  unsigned char buffer[1033], pattern[1031];
  for (unsigned dev = 1; dev <= count; ++dev) {
    uint64_t bytes;
    CHECK(blk_size(dev,&bytes) == 0 && bytes == 8U*1024*1024,"capacity");
    puts_uart("disk: dev="); hex(dev); puts_uart(" modern="); hex(pci_block_modern(dev));
    puts_uart(" ro="); hex(pci_block_readonly(dev)); puts_uart(" bytes="); hex(bytes); puts_uart("\r\n");
    if (dev != 2 || DISK_PHASE == 1) {
      CHECK(blk_read(dev,0,buffer+1,1031) == 0,"initial unaligned buffer read");
      for (unsigned i = 0; i < 1031; ++i)
        CHECK(buffer[1+i] == (unsigned char)(dev*31+i*7),"initial disk content");
    }
    CHECK(blk_read(dev,bytes,NULL,0) == 0 && blk_read(dev,bytes,buffer,1) == -1 &&
          blk_read(dev,UINT64_MAX,buffer,2) == -1 &&
          blk_read(dev,0,NULL,1) == -1,"range checks");
  }
  uint64_t ignored;
  CHECK(blk_size(0,&ignored) == -1 && blk_size(4,&ignored) == -1 &&
        blk_size(1,NULL) == -1 && blk_flush(0) == -1,"invalid device API");
  CHECK(blk_write(1,0,buffer,1) == -1,"root write protection");
  if (count == 1) {
    report("disk: single-root compatibility PASS");
    for (;;) __asm__ volatile("idle 0");
  }
  CHECK(pci_block_readonly(3) && blk_write(3,0,buffer,1) == -1,"read-only disk");
  CHECK(!pci_block_readonly(2),"RW disk unexpectedly readonly");
#ifdef DISK_IO_ERROR
  for (unsigned i = 0; i < 512; ++i) buffer[i] = 0xaa;
  CHECK(blk_read(2,14*512,buffer,512) == -1,"injected I/O error not reported");
  for (unsigned i = 0; i < 512; ++i) CHECK(buffer[i] == 0xaa,"failed read modified caller buffer");
  CHECK(blk_read(2,14*512,buffer,512) == 0,"retry after injected error");
  for (unsigned i = 0; i < 512; ++i)
    CHECK(buffer[i] == (unsigned char)(62+i*7),"retry data");
  report("disk: device I/O error propagation and retry PASS");
  report("disk: ALL PASS");
  for (;;) __asm__ volatile("idle 0");
#endif
  for (unsigned i = 0; i < sizeof pattern; ++i) pattern[i] = (unsigned char)(i*13+7);
  if (DISK_PHASE == 1) {
    CHECK(blk_write(2,777,pattern,sizeof pattern) == 0,"partial sector write");
    CHECK(blk_write(2,4096,marker,sizeof marker) == 0 && blk_flush(2) == 0,"write and flush");
  }
  CHECK(blk_read(2,777,buffer+1,sizeof pattern) == 0,"written bytes readback");
  for (unsigned i = 0; i < sizeof pattern; ++i)
    CHECK(buffer[1+i] == pattern[i],"readback bytes differ");
  CHECK(blk_read(2,4096,buffer,sizeof marker) == 0,"persistent marker read");
  for (unsigned i = 0; i < sizeof marker; ++i)
    CHECK(buffer[i] == marker[i],"persistent marker differs");
  if (DISK_PHASE == 2) {
    report("disk: same-image reboot persistence PASS");
    report("disk: ALL PASS");
    for (;;) __asm__ volatile("idle 0");
  }
  for (unsigned i = 0; i < 300; ++i)
    CHECK(blk_read(2,(i%32)*512,buffer,512) == 0,"virtqueue reuse");
  CHECK(blk_flush(2) == 0,"final flush");
  report("disk: three devices capacity and initial bytes PASS");
  report("disk: D1 unaligned RMW range RO root protection PASS");
  report("disk: queue reuse and flush PASS");
  report("disk: ALL PASS");
  for (;;) __asm__ volatile("idle 0");
}
