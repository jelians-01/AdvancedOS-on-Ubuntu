#include "pci-block.h"

/* QEMU 7.2 LoongArch virt (ls7a.h). PCI ECAM and translated port I/O. */
#define DIRECT UINT64_C(0x9000000000000000)
#define ECAM UINT64_C(0x20000000)
#define PIO UINT64_C(0x18000000)
#define MAX_DEV 3
#define MAX_QUEUE 256
#define MAX_RING 12288
#define F_RO (1U << 5)
#define F_FLUSH (1U << 9)
struct descriptor { uint64_t address; uint32_t length; uint16_t flags,next; };
struct request { uint32_t type,reserved; uint64_t sector; };
struct disk {
  uintptr_t io,common,notify,config,isr;
  uint64_t capacity;
  unsigned present,modern,readonly,flush,failed,busy;
  uint16_t queue,available,used;
  volatile struct descriptor *descriptors;
  volatile uint16_t *avail,*used_ring;
  struct request request;
  volatile unsigned char status;
  unsigned char bounce[512] __attribute__((aligned(16)));
};
static struct disk disks[MAX_DEV];
static unsigned char rings[MAX_DEV][MAX_RING] __attribute__((aligned(4096)));
static unsigned count,initialized;
static uintptr_t next_mmio = UINT64_C(0x40000000);
static uint64_t physical(const void *p) { return (uintptr_t)p & UINT64_C(0x0000ffffffffffff); }
static uintptr_t direct(uintptr_t p) { return p | DIRECT; }
static uint8_t r8(uintptr_t p) { return *(volatile uint8_t *)p; }
static uint16_t r16(uintptr_t p) { return *(volatile uint16_t *)p; }
static uint32_t r32(uintptr_t p) { return *(volatile uint32_t *)p; }
static void w8(uintptr_t p,uint8_t v) { *(volatile uint8_t *)p = v; }
static void w16(uintptr_t p,uint16_t v) { *(volatile uint16_t *)p = v; }
static void w32(uintptr_t p,uint32_t v) { *(volatile uint32_t *)p = v; }
static void w64(uintptr_t p,uint64_t v) { w32(p,(uint32_t)v); w32(p+4,(uint32_t)(v >> 32)); }
static void barrier(void) { __asm__ volatile("dbar 0" ::: "memory"); }
static uint64_t clock_now(void)
{ uint64_t v; __asm__ volatile("rdtime.d %0, $zero" : "=r"(v)); return v; }

static int configure_bars(uintptr_t cfg,unsigned index,uintptr_t bars[6],uint64_t sizes[6])
{
  w16(cfg+4,0); /* Disable decoding while sizing/reassigning BARs. */
  for (unsigned b = 0; b < 6; ++b) {
    uintptr_t reg = cfg + 0x10 + b * 4;
    uint32_t old = r32(reg);
    unsigned io = old & 1, wide = !io && (old & 6) == 4;
    if (wide && b == 5) return -1;
    w32(reg,UINT32_MAX);
    if (wide) w32(reg+4,UINT32_MAX);
    uint32_t low = r32(reg);
    uint64_t mask = low & (io ? ~UINT32_C(3) : ~UINT32_C(15));
    if (wide) mask |= (uint64_t)r32(reg+4) << 32;
    w32(reg,old);
    if (wide) w32(reg+4,0);
    if (!mask) { if (wide) ++b; continue; }
    uint64_t size = wide ? ~mask + 1 : (uint32_t)(~(uint32_t)mask + 1);
    if (!size || (size & (size-1))) return -1;
    uintptr_t assigned;
    if (io) {
      if (size > 256 || b != 0) return -1;
      assigned = 0x4000 + index * 0x100;
      w32(reg,(uint32_t)assigned | 1);
      bars[b] = direct(PIO + assigned);
    } else {
      if (size > 0x100000) return -1;
      assigned = (next_mmio + size-1) & ~(uintptr_t)(size-1);
      if (assigned >= 0x80000000 || size > 0x80000000-assigned) return -1;
      next_mmio = assigned + size;
      w32(reg,(uint32_t)assigned | (old & 15));
      if (wide) w32(reg+4,0);
      bars[b] = direct(assigned);
    }
    sizes[b] = size;
    if (wide) ++b;
  }
  w16(cfg+4,0x407); /* Memory, IO, bus master; mask INTx (polling). */
  return 0;
}

static int modern_regions(struct disk *d,uintptr_t cfg,uintptr_t bars[6],uint64_t sizes[6])
{
  unsigned cap = r8(cfg+0x34), visited[64] = {0};
  uintptr_t notify_base = 0;
  uint32_t notify_length = 0, multiplier = 0;
  for (unsigned steps = 0; cap && steps < 48; ++steps) {
    if (cap < 0x40 || cap > 0xfc || (cap & 3) || visited[cap/4]) return -1;
    visited[cap/4] = 1;
    unsigned next = r8(cfg+cap+1);
    if (r8(cfg+cap) == 9) {
      unsigned length = r8(cfg+cap+2), type = r8(cfg+cap+3), bar = r8(cfg+cap+4);
      if (length < 16 || length > 256-cap) return -1;
      if (type >= 1 && type <= 4) {
        if (bar >= 6) return -1;
        uint32_t offset = r32(cfg+cap+8), bytes = r32(cfg+cap+12);
        if (!bars[bar] || offset > sizes[bar] || bytes > sizes[bar]-offset) return -1;
        uintptr_t base = bars[bar] + offset;
        if (type == 1 && bytes >= 56) d->common = base;
        if (type == 2 && bytes >= 2 && length >= 20) {
          notify_base = base; notify_length = bytes; multiplier = r32(cfg+cap+16);
        }
        if (type == 3 && bytes >= 1) d->isr = base;
        if (type == 4 && bytes >= 8) d->config = base;
      }
    }
    cap = next;
  }
  if (cap || !d->common || !notify_base || !d->isr || !d->config) return -1;
  w16(d->common+22,0);
  uint64_t offset = (uint64_t)r16(d->common+30) * multiplier;
  if (offset > notify_length || 2 > notify_length-offset) return -1;
  d->notify = notify_base + offset;
  return 0;
}

static void status_write(struct disk *d,unsigned value)
{ w8(d->modern ? d->common+20 : d->io+18,(uint8_t)value); }
static unsigned status_read(struct disk *d)
{ return r8(d->modern ? d->common+20 : d->io+18); }

static int start(struct disk *d,unsigned index)
{
  status_write(d,0);
  if (status_read(d) != 0) return -1;
  status_write(d,1); status_write(d,3);
  uint32_t features;
  if (d->modern) {
    w32(d->common,1);
    if (!(r32(d->common+4) & 1)) return -1; /* VERSION_1 */
    w32(d->common,0); features = r32(d->common+4);
    w32(d->common+8,0); w32(d->common+12,features & (F_RO|F_FLUSH));
    w32(d->common+8,1); w32(d->common+12,1);
    status_write(d,11);
    if (!(status_read(d) & 8)) return -1;
    w16(d->common+22,0);
    unsigned offered = r16(d->common+24);
    d->queue = (uint16_t)(offered > MAX_QUEUE ? MAX_QUEUE : offered);
    w16(d->common+24,d->queue); w16(d->common+26,UINT16_MAX);
  } else {
    features = r32(d->io);
    w32(d->io+4,features & (F_RO|F_FLUSH));
    w16(d->io+14,0); d->queue = r16(d->io+12);
  }
  if (d->queue < 3 || d->queue > MAX_QUEUE || (d->queue & (d->queue-1))) return -1;
  d->readonly = !!(features & F_RO); d->flush = !!(features & F_FLUSH);
  unsigned char *ring = rings[index];
  d->descriptors = (volatile struct descriptor *)ring;
  d->avail = (volatile uint16_t *)(ring + 16*d->queue);
  uintptr_t used_offset = (16*d->queue + 2*(3+d->queue) + 4095) & ~(uintptr_t)4095;
  if (used_offset + 6 + 8*d->queue > MAX_RING) return -1;
  d->used_ring = (volatile uint16_t *)(ring + used_offset);
  d->avail[0] = 1; /* NO_INTERRUPT */
  barrier();
  if (d->modern) {
    w64(d->common+32,physical(ring));
    w64(d->common+40,physical((const void *)d->avail));
    w64(d->common+48,physical((const void *)d->used_ring));
    w16(d->common+28,1); status_write(d,15);
  } else {
    w32(d->io+8,(uint32_t)(physical(ring) >> 12)); status_write(d,7);
    d->config = d->io+20;
  }
  uint32_t high1,high2,low;
  unsigned attempts = 0;
  do {
    if (++attempts > 100) return -1;
    high1 = r32(d->config+4); low = r32(d->config); high2 = r32(d->config+4);
  } while (high1 != high2);
  uint64_t sectors = ((uint64_t)high1 << 32) | low;
  if (!sectors || sectors > UINT64_MAX/512) return -1;
  d->capacity = sectors*512; d->present = 1;
  return 0;
}

void blk_init(void)
{
  if (initialized) return;
  initialized = 1;
  for (unsigned slot = 0; slot < 32 && count < MAX_DEV; ++slot) {
    uintptr_t cfg = direct(ECAM + ((uintptr_t)slot << 15));
    uint32_t id = r32(cfg);
    if (id != UINT32_C(0x10011af4) && id != UINT32_C(0x10421af4)) continue;
    struct disk *d = &disks[count];
    /* A rejected candidate must not leak capability pointers into the next. */
    volatile unsigned char *state = (volatile unsigned char *)d;
    for (size_t i = 0; i < sizeof *d; ++i) state[i] = 0;
    uintptr_t bars[6] = {0}; uint64_t sizes[6] = {0};
    d->modern = id == UINT32_C(0x10421af4);
    if (configure_bars(cfg,count,bars,sizes)) { d->failed = 1; continue; }
    if (d->modern) {
      if (modern_regions(d,cfg,bars,sizes)) { d->failed = 1; continue; }
    } else {
      if (!bars[0] || sizes[0] < 28) { d->failed = 1; continue; }
      d->io = bars[0]; d->notify = d->io+16; d->isr = d->io+19;
    }
    if (start(d,count)) { d->failed = 1; status_write(d,128); continue; }
    ++count;
  }
}

static struct disk *device(unsigned dev)
{ return dev && dev <= count && disks[dev-1].present && !disks[dev-1].failed ? &disks[dev-1] : NULL; }

static int submit(struct disk *d,unsigned type,uint64_t sector)
{
  if (d->busy || d->failed) return -1;
  d->busy = 1; d->request.type = type; d->request.reserved = 0;
  d->request.sector = sector; d->status = 0xff;
  d->descriptors[0] = (struct descriptor){physical(&d->request),16,1,(uint16_t)(type == 4 ? 2 : 1)};
  d->descriptors[1] = (struct descriptor){physical(d->bounce),512,(uint16_t)(1 | (type == 0 ? 2 : 0)),2};
  d->descriptors[2] = (struct descriptor){physical((const void *)&d->status),1,2,0};
  d->avail[2 + d->available % d->queue] = 0;
  barrier(); ++d->available; d->avail[1] = d->available;
  barrier(); w16(d->notify,0);
  uint64_t begin = clock_now();
  while (d->used_ring[1] == d->used) {
    if (clock_now()-begin > UINT64_C(100000000)) {
      d->failed = 1; status_write(d,0); d->busy = 0; return -1;
    }
    barrier();
  }
  barrier();
  volatile uint32_t *element = (volatile uint32_t *)&d->used_ring[2 + 4*(d->used % d->queue)];
  int valid = d->used_ring[1] == (uint16_t)(d->used+1) && element[0] == 0 &&
              element[1] == (type == 0 ? 513U : 1U) && d->status == 0;
  ++d->used; (void)r8(d->isr); d->busy = 0;
  return valid ? 0 : -1;
}

static int transfer(unsigned dev,uint64_t offset,void *buffer,unsigned length,int writing)
{
  struct disk *d = device(dev);
  if (!d || offset > d->capacity || length > d->capacity-offset ||
      (length && !buffer) || (writing && (dev == 1 || d->readonly))) return -1;
  unsigned char *bytes = buffer;
  while (length) {
    unsigned within = (unsigned)(offset % 512), step = 512-within;
    if (step > length) step = length;
    if (!writing || within || step != 512)
      if (submit(d,0,offset/512)) return -1;
    for (unsigned i = 0; i < step; ++i)
      if (writing) d->bounce[within+i] = bytes[i]; else bytes[i] = d->bounce[within+i];
    if (writing && submit(d,1,offset/512)) return -1;
    bytes += step; offset += step; length -= step;
  }
  return 0;
}
int blk_read(unsigned dev,uint64_t offset,void *dst,unsigned length)
{ return transfer(dev,offset,dst,length,0); }
int blk_write(unsigned dev,uint64_t offset,const void *src,unsigned length)
{ return transfer(dev,offset,(void *)src,length,1); }
int blk_size(unsigned dev,uint64_t *bytes)
{ struct disk *d = device(dev); if (!d || !bytes) return -1; *bytes = d->capacity; return 0; }
int blk_flush(unsigned dev)
{ struct disk *d = device(dev); if (!d || !d->flush) return -1; return submit(d,4,0); }
unsigned pci_block_count(void) { return count; }
unsigned pci_block_modern(unsigned dev) { struct disk *d = device(dev); return d ? d->modern : 0; }
unsigned pci_block_readonly(unsigned dev) { struct disk *d = device(dev); return d ? d->readonly : 0; }
