// Exercise the production mount parser without modifying the fixture.
#define FAT32_HOST_TEST
#include "../kernel/fat32.c"

static uchar *disk, *original;
static uint64 disk_bytes, original_bytes;
static struct fat32_fs fs;
static uint64 start, length;
static uint cases;

int blk_size(uint dev, uint64 *out)
{ assert(dev == 3); *out = disk_bytes; return 0; }
int blk_read(uint dev, uint64 off, void *dst, uint n)
{
  assert(dev == 3 && off <= disk_bytes && n <= disk_bytes - off);
  memmove(dst, disk + off, n);
  return 0;
}
int blk_write(uint dev, uint64 off, const void *src, uint n)
{ (void)dev; (void)off; (void)src; (void)n; assert(0); return -1; }
int blk_flush(uint dev)
{ (void)dev; assert(0); return -1; }

static uchar *part(uint i) { return disk + 446 + i * 16; }
static void reset(void)
{
  disk_bytes = original_bytes;
  memmove(disk, original, disk_bytes);
  memset(&fs, 0, sizeof(fs));
  fs.dev = 3;
}
static void result(int accepted)
{
  assert((fat32_load(&fs) == 0) == accepted);
  if(accepted){
    assert(fs.volume_offset == start && fs.volume_limit == length);
    assert(fs.volume_bytes <= fs.volume_limit);
  }
  cases++;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  FILE *stream = fopen(argv[1], "rb");
  assert(stream && fseek(stream, 0, SEEK_END) == 0);
  original_bytes = ftell(stream);
  rewind(stream);
  original = malloc(original_bytes);
  disk = malloc(original_bytes);
  assert(original && disk && fread(original, 1, original_bytes, stream) == original_bytes);
  fclose(stream);
  start = (uint64)fat32_get32(original + 446 + 2 * 16 + 8) * 512;
  length = (uint64)fat32_get32(original + 446 + 2 * 16 + 12) * 512;
  assert(start == 1024 * 1024 && length == 64 * 1024 * 1024);

  reset(); result(1);
  reset(); part(2)[4] = 0x0b; part(2)[0] = 0x80; result(1);
  reset(); part(2)[4] = 0x83; result(0);
  reset(); part(2)[0] = 1; result(0);
  reset(); fat32_put32(part(2) + 8, 0); result(0);
  reset(); fat32_put32(part(2) + 12, 0); result(0);
  reset(); fat32_put32(part(2) + 12, 0xffffffffU); result(0);
  reset(); fat32_put32(part(2) + 12, length / 512 - 1); result(0);
  reset(); fat32_put32(disk + start + 28, 7); result(0);
  reset(); fat32_put32(disk + start + 28, 0); result(1);
  reset(); fat32_put32(part(0) + 12, 4096); result(0);
  reset(); part(1)[4] = 0x0b;
  fat32_put32(part(1) + 8, 1024); fat32_put32(part(1) + 12, 512); result(0);
  reset(); part(0)[4] = 0xee; result(0);
  reset(); part(1)[0] = 0x80; result(0); // Empty slot with stale location/flag.
  reset(); disk[start + 510] = 0; result(0);
  reset(); fat32_put32(part(2) + 8, 0xffffffffU); result(0);

  // Raw volumes take precedence over MBR-looking boot code bytes.
  reset(); memmove(disk, original + start, length);
  disk_bytes = length; start = 0;
  fat32_put32(disk + 28, 0);
  memset(disk + 446, 0, 64);
  part(0)[4] = 0x0c;
  fat32_put32(part(0) + 8, 1); fat32_put32(part(0) + 12, 10);
  result(1);
  free(original); free(disk);
  printf("fat32_mbr_test: %u mount cases PASS (no writes)\n", cases);
  return 0;
}
