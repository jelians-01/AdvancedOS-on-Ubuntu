// Exercise the production FAT32 code with a small memory block device.
// Build with function-section GC so unrelated mount/VFS entry points need
// no fake implementation. I/O may fail after bytes have already been stored.
#define FAT32_HOST_TEST
#include "../kernel/fat32.c"

#define SECTOR 512
#define DATA_START (3 * SECTOR)
#define DISK_BYTES (DATA_START + 32 * SECTOR)

static uchar disk[DISK_BYTES], snapshot[DISK_BYTES], payload[900];
static struct fat32_fs fs;
static struct fat32_node file;
enum { NONE, WRITE_ERROR, FLUSH_ERROR, READ_ERROR };
static struct {
  int kind, fired;
  uint64 offset;
  uint occurrence, matches, persisted;
  uint writes_after_error;
} fault;

int
blk_size(uint dev, uint64 *out)
{
  assert(dev == 3);
  *out = sizeof(disk);
  return 0;
}

int
blk_read(uint dev, uint64 offset, void *dst, uint n)
{
  assert(dev == 3 && offset + n <= sizeof(disk));
  if(fault.kind == READ_ERROR && !fault.fired && offset == fault.offset){
    fault.fired = 1;
    return -1;
  }
  memmove(dst, disk + offset, n);
  return 0;
}

int
blk_write(uint dev, uint64 offset, const void *src, uint n)
{
  assert(dev == 3 && offset + n <= sizeof(disk));
  if(fault.fired)
    fault.writes_after_error++;
  if(fault.kind == WRITE_ERROR && !fault.fired && offset == fault.offset &&
     ++fault.matches == fault.occurrence){
    uint persisted = n < fault.persisted ? n : fault.persisted;
    memmove(disk + offset, src, persisted);
    fault.fired = 1;
    return -1;
  }
  memmove(disk + offset, src, n);
  return 0;
}

int
blk_flush(uint dev)
{
  assert(dev == 3);
  if(fault.fired)
    fault.writes_after_error++;
  if(fault.kind == FLUSH_ERROR && !fault.fired){
    fault.fired = 1;
    return -1;
  }
  return 0;
}

static uint
fat(uint copy, uint cluster)
{
  return fat32_get32(disk + (copy + 1) * SECTOR + cluster * 4) & FAT32_FAT_MASK;
}

static void
set_fat(uint cluster, uint value)
{
  for(uint copy = 0; copy < 2; copy++)
    fat32_put32(disk + (copy + 1) * SECTOR + cluster * 4, value);
}

static void
reset(int existing)
{
  memset(disk, 0, sizeof(disk));
  memset(&fs, 0, sizeof(fs));
  memset(&file, 0, sizeof(file));
  memset(&fault, 0, sizeof(fault));
  host_allocations = host_fail_allocation = 0;
  fs.dev = 3;
  fs.bytes_per_sector = fs.cluster_bytes = SECTOR;
  fs.sectors_per_cluster = 1;
  fs.reserved_sectors = fs.fat_sectors = 1;
  fs.fat_count = 2;
  fs.mirrored = 1;
  fs.fat_offset = SECTOR;
  fs.data_offset = DATA_START;
  fs.volume_bytes = sizeof(disk);
  fs.root_cluster = 2;
  fs.cluster_count = 32;
  fs.max_cluster = 33;
  fs.next_free = existing ? 4 : 3;
  initsleeplock(&fs.write_lock, "testwrite");
  initsleeplock(&file.lock, "testnode");
  file.fs = &fs;
  file.attributes = FAT32_ATTR_FILE;
  file.entry_offset = DATA_START;
  set_fat(2, FAT32_EOC);
  if(existing){
    set_fat(3, FAT32_EOC);
    file.first_cluster = 3;
    file.size = SECTOR;
  }
  assert(fat32_store_entry(&fs, file.entry_offset, file.first_cluster,
                           file.size, (const uchar *)"TEST    BIN",
                           FAT32_ATTR_FILE) == 0);
}

static void
disk_file_is_allocated(void)
{
  struct fat32_entry entry;
  uint tail, count;
  fat32_decode_entry(disk + DATA_START, DATA_START, &entry);
  assert(fat32_file_chain(&fs, entry.first_cluster, entry.size, &tail, &count) == 0);
}

static void
check_frozen(void)
{
  assert(fault.fired && fs.write_failed);
  memmove(snapshot, disk, sizeof(disk));
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, 1) == -1);
  assert(fat32_truncate_zero(&file) == -1);
  assert(fat32_flush(&fs) == -1);
  assert(fault.writes_after_error == 0);
  assert(memcmp(snapshot, disk, sizeof(disk)) == 0);
}

static void
write_fault(uint64 offset, uint occurrence, uint persisted)
{
  fault.kind = WRITE_ERROR;
  fault.offset = offset;
  fault.occurrence = occurrence;
  fault.persisted = persisted;
}

int
main(void)
{
  memset(payload, 'x', sizeof(payload));

  reset(0);
  write_fault(DATA_START, 1, FAT32_ENTRY_SIZE);
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(fat32_get32(disk + DATA_START + 28) == sizeof(payload));
  disk_file_is_allocated();
  check_frozen();
  puts("PASS: directory entry persisted before write error; chain retained");

  reset(0);
  fault.kind = FLUSH_ERROR;
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(fat32_get32(disk + DATA_START + 28) == sizeof(payload));
  disk_file_is_allocated();
  check_frozen();
  puts("PASS: flush failed after publication; referenced clusters remain allocated");

  reset(0);
  write_fault(DATA_START + SECTOR, 2, 128); // Zero first, then partially write data.
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(fat32_get32(disk + DATA_START + 28) == 0);
  assert(fat(0, 3) == 4 && fat(0, 4) >= FAT32_EOC);
  check_frozen();
  puts("PASS: partial data I/O failure stops cleanup writes");

  reset(0);
  write_fault(2 * SECTOR + 3 * 4, 1, 0);
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, 1) == -1);
  assert(fat(0, 3) >= FAT32_EOC && fat(1, 3) == 0);
  check_frozen();
  puts("PASS: second FAT copy failure freezes volume without undoing the first");

  reset(1);
  host_fail_allocation = 2; // New cluster zero-buffer succeeds; data-buffer fails.
  write_fault(SECTOR + 3 * 4, 2, 0); // Link succeeds; detaching old tail fails.
  assert(fat32_write_at(&file, 0, (uint64)payload, SECTOR, 1) == -1);
  assert(fat(0, 3) == 4 && fat(0, 4) >= FAT32_EOC);
  disk_file_is_allocated();
  check_frozen();
  puts("PASS: failed rollback detach cannot free the still-linked extension");

  reset(0);
  host_fail_allocation = 3; // Two zero-buffers succeed; data-buffer fails.
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(!fs.write_failed && file.first_cluster == 0);
  assert(fat(0, 3) == 0 && fat(1, 3) == 0 && fat(0, 4) == 0);
  disk_file_is_allocated();
  host_fail_allocation = 0;
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, 1) == 1);
  puts("PASS: clean prepublication OOM reclaims extension and permits retry");

  reset(1);
  fault.kind = FLUSH_ERROR;
  assert(fat32_truncate_zero(&file) == -1);
  assert(fat32_get32(disk + DATA_START + 28) == 0 && fat(0, 3) >= FAT32_EOC);
  check_frozen();
  puts("PASS: truncate flush failure retains original chain");

  reset(0);
  fault.kind = READ_ERROR;
  fault.offset = 2 * SECTOR + 3 * 4;
  memmove(snapshot, disk, sizeof(disk));
  assert(fat32_write_at(&file, 0, (uint64)payload, 0, 1) == -1);
  assert(memcmp(snapshot, disk, sizeof(disk)) == 0);
  check_frozen();
  puts("PASS: allocation read I/O failure freezes further mutations");
  reset(1);
  memmove(snapshot, disk, sizeof(disk));
  // Host copyin rejects this user range. No block or old byte may change.
  assert(fat32_write_at(&file, 1, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(!fs.write_failed && file.first_cluster == 3 && file.size == SECTOR);
  assert(memcmp(snapshot, disk, sizeof(disk)) == 0);
  puts("PASS: invalid user buffer leaves file data and allocation unchanged");

  reset(1);
  host_fail_allocation = 2;
  fault.kind = FLUSH_ERROR;
  assert(fat32_write_at(&file, 0, (uint64)payload, SECTOR, 1) == -1);
  assert(fat(0, 3) >= FAT32_EOC && fat(0, 4) >= FAT32_EOC);
  disk_file_is_allocated();
  check_frozen();
  puts("PASS: rollback detach flush failure retains extension allocation");

  reset(1);
  set_fat(3, 4);
  set_fat(4, FAT32_EOC);
  struct fat32_name plan = {0};
  plan.old_tail = 3;
  plan.added = 4;
  fault.kind = FLUSH_ERROR;
  fat32_cancel_name(&fs, &plan);
  assert(fat(0, 3) >= FAT32_EOC && fat(0, 4) >= FAT32_EOC);
  check_frozen();
  puts("PASS: directory-extension rollback flush failure retains allocation");

  reset(0);
  struct fat32_node *root = 0, *a, *b;
  void *dot = 0, *up = 0, *up2 = 0;
  assert(fat32_root(&fs, (void **)&root) == 0);
  a = fat32_node_new(&fs, 12, DATA_START + 32, 3, 0,
                     FAT32_ATTR_DIR, 2, 1, 2, 1);
  b = fat32_node_new(&fs, 13, DATA_START + 64, 4, 0,
                     FAT32_ATTR_DIR, 3, 12, 2, 1);
  assert(a && b);
  assert(fat32_lookup(root, ".", 1, &dot) == 0 && dot == root);
  fat32_release(dot);
  assert(fat32_lookup(b, "..", 2, &up) == 0 && up == a);
  assert(fat32_lookup(up, "..", 2, &up2) == 0 && up2 == root);
  // The parent reference is the same object unlink would mark as deleted.
  a->deleted = 1;
  assert(((struct fat32_node *)up)->deleted);
  assert(fat32_lookup(up, ".", 1, &dot) == 0 && dot == a);
  assert(((struct fat32_node *)dot)->deleted);
  fat32_release(dot);
  fat32_release(up2);
  fat32_release(up);
  fat32_release(b);
  fat32_release(a);
  fat32_release(root);
  assert(fs.nodes == 0);
  puts("PASS: dot, dot-dot and deleted directories retain one live identity");
  puts("fat32_io_test: ALL TESTS PASSED");
  return 0;
}
