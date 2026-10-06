// RISC-V QEMU virt: bus slot i maps to logical dev i+1; ROOTDEV stays 1.
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "virtio_disk.h"
#include "blkdev.h"

#define REG(d, r) ((volatile uint32 *)((d)->base + (r)))
static struct disk {
  uint64 base, sectors;
  int present, readonly, flush;
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  char free[NUM];
  uint16 used_idx;
  struct { volatile uchar status; int done; } info[NUM];
  struct virtio_blk_req ops[NUM];
  struct spinlock lock;
  struct sleeplock request_lock;
} disks[VDISK_COUNT];

static struct disk *
getdisk(uint dev)
{
  if(dev == 0 || dev > VDISK_COUNT || !disks[dev-1].present)
    return 0;
  return &disks[dev-1];
}

static void
initdisk(struct disk *d, int slot)
{
  d->base = VIRTIO0 + slot * PGSIZE;
  initlock(&d->lock, "virtio_disk");
  initsleeplock(&d->request_lock, "virtio_request");
  if(*REG(d, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *REG(d, VIRTIO_MMIO_VERSION) != 2 || *REG(d, VIRTIO_MMIO_DEVICE_ID) != 2)
    return;
  *REG(d, VIRTIO_MMIO_STATUS) = 0;
  uint32 status = VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER;
  *REG(d, VIRTIO_MMIO_STATUS) = status;
  *REG(d, VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 1;
  if(!(*REG(d, VIRTIO_MMIO_DEVICE_FEATURES) & 1))
    goto failed; // Modern transport requires bit 32: VERSION_1.
  *REG(d, VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
  *REG(d, VIRTIO_MMIO_DRIVER_FEATURES) = 1;
  *REG(d, VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 0;
  uint32 features = *REG(d, VIRTIO_MMIO_DEVICE_FEATURES);
  features &= (1U << VIRTIO_BLK_F_RO) | (1U << VIRTIO_BLK_F_FLUSH);
  d->readonly = (features >> VIRTIO_BLK_F_RO) & 1;
  d->flush = (features >> VIRTIO_BLK_F_FLUSH) & 1;
  *REG(d, VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
  *REG(d, VIRTIO_MMIO_DRIVER_FEATURES) = features;
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *REG(d, VIRTIO_MMIO_STATUS) = status;
  if(!(*REG(d, VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK))
    goto failed;
  uint32 generation, lo, hi;
  do {
    generation = *REG(d, VIRTIO_MMIO_CONFIG_GENERATION);
    lo = *REG(d, VIRTIO_MMIO_CONFIG);
    hi = *REG(d, VIRTIO_MMIO_CONFIG + 4);
    __sync_synchronize();
  } while(generation != *REG(d, VIRTIO_MMIO_CONFIG_GENERATION));
  d->sectors = ((uint64)hi << 32) | lo;
  if(d->sectors > (~(uint64)0 / VDISK_SECTOR_SIZE))
    goto failed;
  *REG(d, VIRTIO_MMIO_QUEUE_SEL) = 0;
  if(*REG(d, VIRTIO_MMIO_QUEUE_READY) || *REG(d, VIRTIO_MMIO_QUEUE_NUM_MAX) < NUM)
    goto failed;
  d->desc = kalloc(); d->avail = kalloc(); d->used = kalloc();
  if(!d->desc || !d->avail || !d->used)
    panic("virtio queue allocation");
  memset(d->desc, 0, PGSIZE); memset(d->avail, 0, PGSIZE); memset(d->used, 0, PGSIZE);
  *REG(d, VIRTIO_MMIO_QUEUE_NUM) = NUM;
  *REG(d, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)d->desc;
  *REG(d, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)d->desc >> 32;
  *REG(d, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)d->avail;
  *REG(d, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)d->avail >> 32;
  *REG(d, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)d->used;
  *REG(d, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)d->used >> 32;
  for(int i = 0; i < NUM; i++)
    d->free[i] = 1;
  __sync_synchronize();
  *REG(d, VIRTIO_MMIO_QUEUE_READY) = 1;
  *REG(d, VIRTIO_MMIO_STATUS) = status | VIRTIO_CONFIG_S_DRIVER_OK;
  d->present = 1;
  printf("blkdev: dev=%d mmio=%p sectors=%ld ro=%d flush=%d\n",
         slot+1, (void *)d->base, d->sectors, d->readonly, d->flush);
  return;
failed:
  *REG(d, VIRTIO_MMIO_STATUS) = status | 128;
}

void
virtio_disk_init(void)
{
  for(int i = 0; i < VDISK_COUNT; i++)
    initdisk(&disks[i], i);
  if(!getdisk(ROOTDEV))
    panic("could not find root virtio disk");
  blk_init();
}

int
virtio_disk_size(uint dev, uint64 *bytes)
{
  struct disk *d = getdisk(dev);
  if(!d || !bytes)
    return -1;
  *bytes = d->sectors * VDISK_SECTOR_SIZE;
  return 0;
}

static int
alloc_desc(struct disk *d)
{
  for(int i = 0; i < NUM; i++)
    if(d->free[i]){ d->free[i] = 0; return i; }
  panic("virtio descriptors exhausted");
}

static int
request(struct disk *d, uint32 type, uint64 sector, void *data, uint bytes)
{
  // Wait for all earlier requests before sending FLUSH; no busy polling.
  acquiresleep(&d->request_lock);
  acquire(&d->lock);
  int head = alloc_desc(d), tail = alloc_desc(d);
  struct virtio_blk_req *op = &d->ops[head];
  op->type = type; op->reserved = 0; op->sector = sector;
  d->desc[head] = (struct virtq_desc){(uint64)op, sizeof(*op), VRING_DESC_F_NEXT, tail};
  if(type != VIRTIO_BLK_T_FLUSH){
    int middle = alloc_desc(d);
    d->desc[head].next = middle;
    d->desc[middle] = (struct virtq_desc){(uint64)data, bytes,
      VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0), tail};
  }
  d->info[head].done = 0;
  d->info[head].status = 0xff;
  d->desc[tail] = (struct virtq_desc){(uint64)&d->info[head].status, 1, VRING_DESC_F_WRITE, 0};
  d->avail->ring[d->avail->idx % NUM] = head;
  __sync_synchronize();
  d->avail->idx++;
  __sync_synchronize();
  *REG(d, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
  while(!d->info[head].done)
    sleep(&d->info[head], &d->lock);
  int result = d->info[head].status == 0 ? 0 : -1;
  for(int i = head;;){
    uint16 flags = d->desc[i].flags;
    int next = d->desc[i].next;
    memset(&d->desc[i], 0, sizeof(d->desc[i]));
    d->free[i] = 1;
    if(!(flags & VRING_DESC_F_NEXT))
      break;
    i = next;
  }
  release(&d->lock);
  releasesleep(&d->request_lock);
  return result;
}

int
virtio_disk_transfer(uint dev, uint64 sector, void *data, uint bytes, int write)
{
  struct disk *d = getdisk(dev);
  if(!d || !data || bytes == 0 || bytes % VDISK_SECTOR_SIZE ||
     (write && d->readonly) || sector > d->sectors ||
     bytes / VDISK_SECTOR_SIZE > d->sectors - sector)
    return -1;
  return request(d, write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, sector, data, bytes);
}

int
virtio_disk_flush(uint dev)
{
  struct disk *d = getdisk(dev);
  return d && d->flush ? request(d, VIRTIO_BLK_T_FLUSH, 0, 0, 0) : -1;
}

void
virtio_disk_rw(struct buf *b, int write)
{
#ifdef BLKDEV_SELFTEST
  extern void blkdev_selftest(void);
  static int tested;
  if(!tested){ tested = 1; blkdev_selftest(); }
#endif
  if(virtio_disk_transfer(b->dev, (uint64)b->blockno * (BSIZE / 512), b->data, BSIZE, write) < 0)
    panic("native disk I/O failed");
}

void
virtio_disk_intr_dev(uint dev)
{
  struct disk *d = getdisk(dev);
  if(!d)
    return;
  acquire(&d->lock);
  *REG(d, VIRTIO_MMIO_INTERRUPT_ACK) = *REG(d, VIRTIO_MMIO_INTERRUPT_STATUS) & 3;
  __sync_synchronize();
  while(d->used_idx != d->used->idx){
    __sync_synchronize();
    uint id = d->used->ring[d->used_idx % NUM].id;
    if(id >= NUM || d->free[id])
      panic("virtio bad completion");
    d->info[id].done = 1;
    wakeup(&d->info[id]);
    d->used_idx++;
  }
  release(&d->lock);
}

void
virtio_disk_intr(void)
{
  virtio_disk_intr_dev(ROOTDEV);
}
