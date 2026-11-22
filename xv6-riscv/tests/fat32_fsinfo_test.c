// Production FAT32 backend on a real mkfs.fat image and a memory block device.
#define FAT32_HOST_TEST
#include "../kernel/fat32.c"

static uchar *disk, *original;
static uint64 disk_bytes;
static struct fat32_fs fs;
static struct fat32_node *root;
static uint info_sector, backup_sector, bps;
static uint64 fail_offset;
static int fired, writes_after_failure;

int
blk_size(uint dev, uint64 *out)
{ assert(dev == 3); *out = disk_bytes; return 0; }

int
blk_read(uint dev, uint64 off, void *dst, uint n)
{
  assert(dev == 3 && off <= disk_bytes && n <= disk_bytes - off);
  memmove(dst, disk + off, n);
  return 0;
}

int
blk_write(uint dev, uint64 off, const void *src, uint n)
{
  assert(dev == 3 && off <= disk_bytes && n <= disk_bytes - off);
  if(fired) writes_after_failure++;
  memmove(disk + off, src, n);
  if(fail_offset && off == fail_offset){
    fired = 1;
    return -1; // All bytes reached storage before the error was reported.
  }
  return 0;
}

int
blk_flush(uint dev)
{
  assert(dev == 3);
  if(fired) writes_after_failure++;
  return 0;
}

static uchar *
info(void)
{ return disk + (uint64)info_sector * bps; }

static void
reset(void)
{
  memmove(disk, original, disk_bytes);
  memset(&fs, 0, sizeof(fs));
  fs.dev = 3;
  root = 0;
  fired = writes_after_failure = 0;
  fail_offset = 0;
}

static void
mount_image(void)
{
  assert(fat32_load(&fs) == 0);
  initlock(&fs.nodes_lock, "nodes");
  void *data = 0;
  assert(fat32_root(&fs, &data) == 0);
  root = data;
}

static struct fat32_node *
create(const char *name)
{
  void *data = 0;
  assert(fat32_create_file(root, name, strlen(name), &data) == 0);
  return data;
}

static uint
scan_free(void)
{
  uint count = 0;
  for(uint i = 2; i <= fs.max_cluster; i++)
    if((fat32_get32(disk + fs.fat_offset + (uint64)i * 4) & FAT32_FAT_MASK) == 0)
      count++;
  return count;
}

static void
check_info(uint expected)
{
  assert(fat32_get32(info() + 488) == expected);
  uint hint = fat32_get32(info() + 492);
  assert(hint >= 2 && hint <= fs.max_cluster);
  // Update only the primary hint fields, preserving all reserved bytes.
  uint64 off = (uint64)info_sector * bps;
  assert(memcmp(info(), original + off, 488) == 0);
  assert(memcmp(info() + 496, original + off + 496, bps - 496) == 0);
  uint64 backup = (uint64)(backup_sector + info_sector) * bps;
  assert(memcmp(disk + backup, original + backup, bps) == 0);
}

static void
finish(void)
{
  fat32_release(root);
  root = 0;
  assert(fs.nodes == 0);
}

int
main(int argc, char **argv)
{
  assert(argc == 3);
  FILE *stream = fopen(argv[1], "rb");
  assert(stream && fseek(stream, 0, SEEK_END) == 0);
  disk_bytes = ftell(stream);
  rewind(stream);
  original = malloc(disk_bytes);
  disk = malloc(disk_bytes);
  assert(original && disk && fread(original, 1, disk_bytes, stream) == disk_bytes);
  fclose(stream);
  bps = fat32_get16(original + 11);
  info_sector = fat32_get16(original + 48);
  backup_sector = fat32_get16(original + 50);
  assert(bps == 512 && original[13] == 1 && info_sector && backup_sector);
  uchar payload[900], readback[900];
  memset(payload, 'p', sizeof(payload));

  reset();
  mount_image();
  uint baseline = scan_free();
  assert(fs.free_count == baseline && fs.fsinfo_sector == info_sector);
  struct fat32_node *file = create("temp.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, sizeof(payload)) == sizeof(payload));
  assert(scan_free() == baseline - 2);
  check_info(baseline - 2);
  assert(fat32_truncate_zero(file) == 0 && scan_free() == baseline);
  check_info(baseline);
  assert(fat32_write_at(file, 0, (uint64)payload, 0, sizeof(payload)) == sizeof(payload));
  assert(fat32_unlink(root, "temp.bin", 8) == 0);
  check_info(baseline - 2); // Still open, so its clusters must remain counted.
  fat32_release(file);
  assert(scan_free() == baseline);
  check_info(baseline);
  file = create("persist.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, sizeof(payload)) == sizeof(payload));
  fat32_lock(file);
  assert(fat32_read_at(file, 0, (uint64)readback, 0, sizeof(readback)) == sizeof(readback));
  fat32_unlock(file);
  assert(memcmp(readback, payload, sizeof(payload)) == 0);
  check_info(scan_free());
  fat32_release(file);
  finish();
  stream = fopen(argv[2], "wb");
  assert(stream && fwrite(disk, 1, disk_bytes, stream) == disk_bytes);
  assert(fclose(stream) == 0);
  puts("PASS: known count, allocation, truncate, deferred unlink, reserved bytes and backup");

  reset();
  fat32_put32(info() + 488, FAT32_UNKNOWN);
  fat32_put32(info() + 492, FAT32_UNKNOWN);
  mount_image();
  assert(fs.free_count == FAT32_UNKNOWN && fs.next_free == 2);
  file = create("unknown.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == 1);
  check_info(FAT32_UNKNOWN);
  fat32_release(file); finish();
  puts("PASS: unknown free count remains unknown; allocation still uses FAT");

  reset();
  fat32_put32(info() + 488, baseline + 100);
  fat32_put32(info() + 492, 1);
  mount_image();
  assert(fs.free_count == FAT32_UNKNOWN && fs.next_free == 2);
  file = create("range.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == 1);
  check_info(FAT32_UNKNOWN);
  fat32_release(file); finish();
  puts("PASS: invalid counts and next-free hints ignored");

  reset();
  fat32_put32(info() + 488, 0); // Stale but in range; never trust it for ENOSPC.
  mount_image();
  file = create("stale.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == 1);
  check_info(FAT32_UNKNOWN);
  fat32_release(file); finish();
  puts("PASS: stale zero count neither blocks allocation nor underflows");

  reset();
  fat32_put32(info(), 0);
  uchar invalid[512];
  memmove(invalid, info(), sizeof(invalid));
  mount_image();
  assert(fs.fsinfo_sector == 0);
  file = create("invalid.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == 1);
  assert(memcmp(invalid, info(), sizeof(invalid)) == 0);
  fat32_release(file); finish();
  puts("PASS: invalid FSInfo signature does not prevent file writes or get repaired");

  reset();
  fat32_put16(disk + 48, fat32_get16(disk + 14));
  memmove(invalid, info(), sizeof(invalid));
  mount_image();
  assert(fs.fsinfo_sector == 0);
  file = create("pointer.bin");
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == 1);
  assert(memcmp(invalid, info(), sizeof(invalid)) == 0);
  fat32_release(file); finish();
  puts("PASS: FSInfo pointer outside reserved region ignored");

  reset();
  mount_image();
  file = create("error.bin");
  fail_offset = (uint64)info_sector * bps + 488;
  assert(fat32_write_at(file, 0, (uint64)payload, 0, sizeof(payload)) == -1);
  assert(fired && fs.write_failed && scan_free() == baseline - 2);
  assert(fat32_write_at(file, 0, (uint64)payload, 0, 1) == -1);
  assert(fat32_truncate_zero(file) == -1);
  assert(writes_after_failure == 0);
  fat32_release(file); finish();
  puts("PASS: FSInfo write error freezes volume and retains referenced clusters");
  free(disk); free(original);
  puts("fat32_fsinfo_test: ALL TESTS PASSED");
  return 0;
}
