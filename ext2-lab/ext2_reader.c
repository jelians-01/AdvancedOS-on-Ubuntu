#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* A small, read-only ext2 image inspector for the ext2-lab test image. */
#define EXT2_SUPER_OFFSET 1024u
#define EXT2_SUPER_SIZE 1024u
#define EXT2_ROOT_INO 2u
#define EXT2_MAGIC 0xEF53u
#define EXT2_NDIR_BLOCKS 12u

#define EXT2_FEATURE_COMPAT_EXT_ATTR 0x0008u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u

#define EXT2_S_IFMT  0170000u
#define EXT2_S_IFREG 0100000u
#define EXT2_S_IFDIR 0040000u
#define EXT2_S_IFLNK 0120000u

struct ext2_fs {
  int fd;
  int writable;
  uint64_t image_bytes;
  uint32_t blocks_count;
  uint32_t inodes_count;
  uint32_t first_data_block;
  uint32_t block_size;
  uint32_t blocks_per_group;
  uint32_t inodes_per_group;
  uint32_t first_ino;
  uint32_t group_count;
  uint16_t inode_size;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
  int dirent_has_type;
};

struct ext2_inode {
  uint32_t number;
  uint16_t mode;
  uint32_t size_lo;
  uint32_t size_hi;
  uint32_t blocks[15];
  uint32_t sectors;
};

static uint16_t
le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
put_le16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static int
read_exact(struct ext2_fs *fs, uint64_t off, void *dst, size_t len)
{
  uint8_t *p = dst;
  size_t done = 0;

  if (off > fs->image_bytes || (uint64_t)len > fs->image_bytes - off) {
    fprintf(stderr, "read outside image: offset=%" PRIu64 " length=%zu\n",
            off, len);
    return -1;
  }
  while (done < len) {
    ssize_t n = pread(fs->fd, p + done, len - done, (off_t)(off + done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      fprintf(stderr, "image read failed at offset=%" PRIu64 ": %s\n",
              off + done, n == 0 ? "unexpected end of file" : strerror(errno));
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static int
write_exact(struct ext2_fs *fs, uint64_t off, const void *src, size_t len)
{
  const uint8_t *p = src;
  size_t done = 0;

  if (!fs->writable || off > fs->image_bytes ||
      (uint64_t)len > fs->image_bytes - off) {
    fprintf(stderr, "write outside image or image is read-only\n");
    return -1;
  }
  while (done < len) {
    ssize_t n = pwrite(fs->fd, p + done, len - done, (off_t)(off + done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      fprintf(stderr, "image write failed at offset=%" PRIu64 ": %s\n",
              off + done, n == 0 ? "short write" : strerror(errno));
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static int
read_block(struct ext2_fs *fs, uint32_t block, void *dst)
{
  uint64_t off;

  if (block < fs->first_data_block || block >= fs->blocks_count) {
    fprintf(stderr, "invalid ext2 block number: %" PRIu32 "\n", block);
    return -1;
  }
  off = (uint64_t)block * fs->block_size;
  return read_exact(fs, off, dst, fs->block_size);
}

static int
open_ext2(struct ext2_fs *fs, const char *image, int writable)
{
  uint8_t sb[EXT2_SUPER_SIZE];
  struct stat st;
  uint32_t log_block_size;
  uint32_t compat_supported = EXT2_FEATURE_COMPAT_EXT_ATTR;
  uint32_t incompat_supported = EXT2_FEATURE_INCOMPAT_FILETYPE;
  uint32_t ro_supported = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                          EXT2_FEATURE_RO_COMPAT_LARGE_FILE;

  memset(fs, 0, sizeof(*fs));
  fs->fd = -1;
  fs->writable = writable;
  fs->fd = open(image, writable ? O_RDWR : O_RDONLY);
  if (fs->fd < 0) {
    fprintf(stderr, "cannot open %s: %s\n", image, strerror(errno));
    return -1;
  }
  if (fstat(fs->fd, &st) < 0 || st.st_size < 0) {
    fprintf(stderr, "cannot stat %s: %s\n", image, strerror(errno));
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }
  fs->image_bytes = (uint64_t)st.st_size;
  if (fs->image_bytes < EXT2_SUPER_OFFSET + EXT2_SUPER_SIZE ||
      read_exact(fs, EXT2_SUPER_OFFSET, sb, sizeof(sb)) < 0) {
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }
  if (le16(sb + 56) != EXT2_MAGIC) {
    fprintf(stderr, "not an ext2 image: bad magic 0x%04x\n", le16(sb + 56));
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }

  fs->inodes_count = le32(sb + 0);
  fs->blocks_count = le32(sb + 4);
  fs->first_data_block = le32(sb + 20);
  log_block_size = le32(sb + 24);
  fs->blocks_per_group = le32(sb + 32);
  fs->inodes_per_group = le32(sb + 40);
  fs->first_ino = le32(sb + 84);
  fs->feature_compat = le32(sb + 92);
  fs->feature_incompat = le32(sb + 96);
  fs->feature_ro_compat = le32(sb + 100);

  if (log_block_size > 2) {
    fprintf(stderr, "unsupported ext2 block size exponent: %" PRIu32 "\n",
            log_block_size);
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }
  fs->block_size = 1024u << log_block_size;
  fs->inode_size = le16(sb + 88);
  if (le32(sb + 76) == 0) /* Old revision uses the fixed 128-byte inode. */
    fs->inode_size = 128;

  if (fs->inodes_count == 0 || fs->blocks_count == 0 ||
      fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
      fs->inode_size < 128 || fs->inode_size > fs->block_size ||
      fs->first_data_block >= fs->blocks_count ||
      (uint64_t)fs->blocks_count * fs->block_size > fs->image_bytes) {
    fprintf(stderr, "invalid or unsupported ext2 geometry\n");
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }
  if (fs->first_ino == 0)
    fs->first_ino = 11;
  fs->group_count = (fs->inodes_count + fs->inodes_per_group - 1) /
                    fs->inodes_per_group;
  if ((fs->feature_compat & ~compat_supported) != 0 ||
      (fs->feature_incompat & ~incompat_supported) != 0 ||
      (fs->feature_ro_compat & ~ro_supported) != 0) {
    fprintf(stderr,
            "unsupported ext2 features: compat=0x%08" PRIx32
            " incompat=0x%08" PRIx32 " ro_compat=0x%08" PRIx32 "\n",
            fs->feature_compat, fs->feature_incompat, fs->feature_ro_compat);
    close(fs->fd);
    fs->fd = -1;
    return -1;
  }
  fs->dirent_has_type =
      (fs->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
  return 0;
}

static void
close_ext2(struct ext2_fs *fs)
{
  if (fs->fd >= 0)
    close(fs->fd);
  fs->fd = -1;
}

static int
read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out)
{
  uint8_t gd[32];
  uint8_t *raw;
  uint64_t group_count;
  uint32_t group, index;
  uint32_t gdt_block;
  uint32_t inode_table;
  uint64_t inode_off;

  if (ino == 0 || ino > fs->inodes_count) {
    fprintf(stderr, "invalid inode number: %" PRIu32 "\n", ino);
    return -1;
  }
  group = (ino - 1) / fs->inodes_per_group;
  index = (ino - 1) % fs->inodes_per_group;
  group_count = ((uint64_t)fs->inodes_count + fs->inodes_per_group - 1) /
                fs->inodes_per_group;
  if (group >= group_count) {
    fprintf(stderr, "inode group out of range: %" PRIu32 "\n", group);
    return -1;
  }

  /* The primary group-descriptor table follows the superblock block. */
  gdt_block = fs->first_data_block + 1;
  if (read_exact(fs, (uint64_t)gdt_block * fs->block_size +
                     (uint64_t)group * sizeof(gd),
                 gd, sizeof(gd)) < 0)
    return -1;
  inode_table = le32(gd + 8);
  if (inode_table == 0 || inode_table >= fs->blocks_count) {
    fprintf(stderr, "invalid inode table block for group %" PRIu32 "\n", group);
    return -1;
  }

  inode_off = (uint64_t)inode_table * fs->block_size +
              (uint64_t)index * fs->inode_size;
  raw = malloc(fs->inode_size);
  if (raw == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  if (read_exact(fs, inode_off, raw, fs->inode_size) < 0) {
    free(raw);
    return -1;
  }

  memset(out, 0, sizeof(*out));
  out->number = ino;
  out->mode = le16(raw + 0);
  out->size_lo = le32(raw + 4);
  out->sectors = le32(raw + 28);
  for (unsigned i = 0; i < 15; i++)
    out->blocks[i] = le32(raw + 40 + i * 4);
  /* i_dir_acl is the high size word for regular files with LARGE_FILE. */
  if ((out->mode & EXT2_S_IFMT) == EXT2_S_IFREG &&
      (fs->feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
    out->size_hi = le32(raw + 108);
  free(raw);
  return 0;
}

static uint64_t
group_desc_offset(struct ext2_fs *fs, uint32_t group)
{
  uint32_t gdt_block = fs->first_data_block + 1;
  return (uint64_t)gdt_block * fs->block_size + (uint64_t)group * 32;
}

static int
load_group_desc(struct ext2_fs *fs, uint32_t group, uint8_t gd[32])
{
  if (group >= fs->group_count) {
    fprintf(stderr, "invalid block group: %" PRIu32 "\n", group);
    return -1;
  }
  return read_exact(fs, group_desc_offset(fs, group), gd, 32);
}

static int
store_group_desc(struct ext2_fs *fs, uint32_t group, const uint8_t gd[32])
{
  if (!fs->writable || group >= fs->group_count)
    return -1;
  return write_exact(fs, group_desc_offset(fs, group), gd, 32);
}

static int
inode_location(struct ext2_fs *fs, uint32_t ino, uint32_t *group_out,
               uint32_t *index_out, uint64_t *offset_out)
{
  uint8_t gd[32];
  uint32_t group, index, inode_table;

  if (ino == 0 || ino > fs->inodes_count)
    return -1;
  group = (ino - 1) / fs->inodes_per_group;
  index = (ino - 1) % fs->inodes_per_group;
  if (load_group_desc(fs, group, gd) < 0)
    return -1;
  inode_table = le32(gd + 8);
  if (inode_table == 0 || inode_table >= fs->blocks_count)
    return -1;
  *group_out = group;
  *index_out = index;
  *offset_out = (uint64_t)inode_table * fs->block_size +
                (uint64_t)index * fs->inode_size;
  return 0;
}

static int
store_inode_raw(struct ext2_fs *fs, uint32_t ino, const uint8_t *raw)
{
  uint32_t group, index;
  uint64_t offset;
  (void)index;
  if (inode_location(fs, ino, &group, &index, &offset) < 0)
    return -1;
  return write_exact(fs, offset, raw, fs->inode_size);
}

static int
adjust_super_counts(struct ext2_fs *fs, int32_t blocks_delta,
                    int32_t inodes_delta)
{
  uint8_t field[4];
  uint32_t value;
  uint64_t off;

  off = EXT2_SUPER_OFFSET + 12;
  if (read_exact(fs, off, field, 4) < 0)
    return -1;
  value = le32(field);
  if ((blocks_delta < 0 && value < (uint32_t)-blocks_delta) ||
      (blocks_delta > 0 && value > UINT32_MAX - (uint32_t)blocks_delta))
    return -1;
  put_le32(field, value + blocks_delta);
  if (write_exact(fs, off, field, 4) < 0)
    return -1;

  off = EXT2_SUPER_OFFSET + 16;
  if (read_exact(fs, off, field, 4) < 0)
    return -1;
  value = le32(field);
  if ((inodes_delta < 0 && value < (uint32_t)-inodes_delta) ||
      (inodes_delta > 0 && value > UINT32_MAX - (uint32_t)inodes_delta))
    return -1;
  put_le32(field, value + inodes_delta);
  return write_exact(fs, off, field, 4);
}

static int
change_group_free_count(struct ext2_fs *fs, uint32_t group, int is_inode,
                        int delta)
{
  uint8_t gd[32];
  size_t off = is_inode ? 14 : 12;
  uint16_t value;

  if (load_group_desc(fs, group, gd) < 0)
    return -1;
  value = le16(gd + off);
  if ((delta < 0 && value == 0) || (delta > 0 && value == UINT16_MAX))
    return -1;
  put_le16(gd + off, (uint16_t)(value + delta));
  return store_group_desc(fs, group, gd);
}

static int
alloc_block(struct ext2_fs *fs, uint32_t *out)
{
  uint8_t gd[32];
  uint8_t *bitmap;
  uint64_t group_count = ((uint64_t)fs->blocks_count - fs->first_data_block +
                          fs->blocks_per_group - 1) / fs->blocks_per_group;

  bitmap = malloc(fs->block_size);
  if (bitmap == NULL)
    return -1;
  for (uint32_t group = 0; group < group_count && group < fs->group_count;
       group++) {
    uint32_t bitmap_block;
    uint32_t first = fs->first_data_block + group * fs->blocks_per_group;
    uint32_t limit = first + fs->blocks_per_group;
    if (limit < first || limit > fs->blocks_count)
      limit = fs->blocks_count;
    if (load_group_desc(fs, group, gd) < 0)
      goto fail;
    if (le16(gd + 12) == 0)
      continue;
    bitmap_block = le32(gd + 0);
    if (bitmap_block >= fs->blocks_count ||
        read_block(fs, bitmap_block, bitmap) < 0)
      goto fail;
    for (uint32_t block = first; block < limit; block++) {
      uint32_t bit = block - first;
      if ((bitmap[bit / 8] & (1u << (bit % 8))) == 0) {
        bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        if (write_exact(fs, (uint64_t)bitmap_block * fs->block_size,
                        bitmap, fs->block_size) < 0 ||
            change_group_free_count(fs, group, 0, -1) < 0 ||
            adjust_super_counts(fs, -1, 0) < 0)
          goto fail;
        *out = block;
        free(bitmap);
        return 0;
      }
    }
  }
  fprintf(stderr, "no free ext2 data blocks\n");
fail:
  free(bitmap);
  return -1;
}

static int
free_block(struct ext2_fs *fs, uint32_t block)
{
  uint8_t gd[32];
  uint8_t *bitmap;
  uint32_t group, bit, bitmap_block, first;

  if (block < fs->first_data_block || block >= fs->blocks_count)
    return -1;
  group = (block - fs->first_data_block) / fs->blocks_per_group;
  first = fs->first_data_block + group * fs->blocks_per_group;
  bit = block - first;
  if (load_group_desc(fs, group, gd) < 0)
    return -1;
  bitmap_block = le32(gd + 0);
  bitmap = malloc(fs->block_size);
  if (bitmap == NULL)
    return -1;
  if (read_block(fs, bitmap_block, bitmap) < 0 ||
      (bitmap[bit / 8] & (1u << (bit % 8))) == 0) {
    free(bitmap);
    fprintf(stderr, "attempt to free an unallocated block\n");
    return -1;
  }
  bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
  if (write_exact(fs, (uint64_t)bitmap_block * fs->block_size,
                  bitmap, fs->block_size) < 0 ||
      change_group_free_count(fs, group, 0, 1) < 0 ||
      adjust_super_counts(fs, 1, 0) < 0) {
    free(bitmap);
    return -1;
  }
  free(bitmap);
  return 0;
}

static int
alloc_inode(struct ext2_fs *fs, uint32_t *out)
{
  uint8_t gd[32];
  uint8_t *bitmap;

  bitmap = malloc(fs->block_size);
  if (bitmap == NULL)
    return -1;
  for (uint32_t group = 0; group < fs->group_count; group++) {
    uint32_t bitmap_block;
    uint32_t first_ino = group * fs->inodes_per_group + 1;
    uint32_t limit = first_ino + fs->inodes_per_group;
    if (limit < first_ino || limit > fs->inodes_count + 1)
      limit = fs->inodes_count + 1;
    if (limit <= fs->first_ino)
      continue;
    if (first_ino < fs->first_ino)
      first_ino = fs->first_ino;
    if (load_group_desc(fs, group, gd) < 0)
      goto fail;
    if (le16(gd + 14) == 0)
      continue;
    bitmap_block = le32(gd + 4);
    if (bitmap_block >= fs->blocks_count ||
        read_block(fs, bitmap_block, bitmap) < 0)
      goto fail;
    for (uint32_t ino = first_ino; ino < limit; ino++) {
      uint32_t bit = ino - (group * fs->inodes_per_group + 1);
      if ((bitmap[bit / 8] & (1u << (bit % 8))) == 0) {
        bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
        if (write_exact(fs, (uint64_t)bitmap_block * fs->block_size,
                        bitmap, fs->block_size) < 0 ||
            change_group_free_count(fs, group, 1, -1) < 0 ||
            adjust_super_counts(fs, 0, -1) < 0)
          goto fail;
        *out = ino;
        free(bitmap);
        return 0;
      }
    }
  }
  fprintf(stderr, "no free ext2 inodes\n");
fail:
  free(bitmap);
  return -1;
}

static int
write_inode_record(struct ext2_fs *fs, uint32_t ino, const uint8_t *raw)
{
  return store_inode_raw(fs, ino, raw);
}

static int
read_inode_raw(struct ext2_fs *fs, uint32_t ino, uint8_t **raw_out)
{
  uint32_t group, index;
  uint64_t offset;
  uint8_t *raw;

  if (inode_location(fs, ino, &group, &index, &offset) < 0)
    return -1;
  (void)group;
  (void)index;
  raw = malloc(fs->inode_size);
  if (raw == NULL)
    return -1;
  if (read_exact(fs, offset, raw, fs->inode_size) < 0) {
    free(raw);
    return -1;
  }
  *raw_out = raw;
  return 0;
}

static int
require_simple_writable_fs(struct ext2_fs *fs)
{
  if (!fs->writable || fs->block_size != 1024 || fs->inode_size != 128 ||
      fs->feature_compat != 0 || fs->feature_incompat != 0 ||
      fs->feature_ro_compat != 0 || fs->group_count == 0) {
    fprintf(stderr,
            "writes are limited to writable 1 KiB/128-byte ext2 images "
            "with no feature flags\n");
    return -1;
  }
  return 0;
}

static uint64_t inode_size(const struct ext2_inode *ino);
static int directory_lookup(struct ext2_fs *fs, const struct ext2_inode *dir,
                            const char *name, size_t name_len,
                            uint32_t *out_ino);

static int
root_name(const char *path, const char **name, size_t *name_len)
{
  const char *p;
  size_t len;

  if (path[0] != '/')
    return -1;
  p = path + 1;
  len = strlen(p);
  if (len == 0 || len > 255 || strchr(p, '/') != NULL ||
      strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
    return -1;
  *name = p;
  *name_len = len;
  return 0;
}

static int
root_lookup(struct ext2_fs *fs, const char *name, size_t name_len,
            uint32_t *ino)
{
  struct ext2_inode root;
  if (read_inode(fs, EXT2_ROOT_INO, &root) < 0)
    return -1;
  return directory_lookup(fs, &root, name, name_len, ino);
}

static int map_file_block(struct ext2_fs *fs, const struct ext2_inode *ino,
                          uint64_t logical, uint32_t *physical);

static int
root_add_entry(struct ext2_fs *fs, const char *name, size_t name_len,
               uint32_t ino)
{
  struct ext2_inode root;
  uint8_t *block;

  if (read_inode(fs, EXT2_ROOT_INO, &root) < 0)
    return -1;
  block = malloc(fs->block_size);
  if (block == NULL)
    return -1;
  for (uint64_t base = 0; base < inode_size(&root); base += fs->block_size) {
    uint32_t physical;
    size_t valid = inode_size(&root) - base < fs->block_size ?
                   (size_t)(inode_size(&root) - base) : fs->block_size;
    size_t pos = 0;

    if (map_file_block(fs, &root, base / fs->block_size, &physical) < 0) {
      free(block);
      return -1;
    }
    if (physical == 0)
      continue;
    if (read_block(fs, physical, block) < 0) {
      free(block);
      return -1;
    }
    while (pos < valid) {
      uint32_t old_ino = le32(block + pos);
      uint16_t rec_len = le16(block + pos + 4);
      uint16_t old_name_len = le16(block + pos + 6);
      size_t need = (8 + name_len + 3) & ~(size_t)3;
      size_t used = old_ino == 0 ? 0 : (8 + old_name_len + 3) & ~(size_t)3;

      if (rec_len < 8 || (rec_len & 3) != 0 || rec_len > valid - pos ||
          old_name_len > rec_len - 8) {
        free(block);
        fprintf(stderr, "malformed root directory entry\n");
        return -1;
      }
      if ((old_ino == 0 && rec_len >= need) ||
          (old_ino != 0 && rec_len >= used + need)) {
        uint16_t new_rec_len = old_ino == 0 ? rec_len : (uint16_t)(rec_len - used);
        if (old_ino != 0)
          put_le16(block + pos + 4, (uint16_t)used);
        size_t new_pos = old_ino == 0 ? pos : pos + used;
        memset(block + new_pos, 0, new_rec_len);
        put_le32(block + new_pos, ino);
        put_le16(block + new_pos + 4, new_rec_len);
        put_le16(block + new_pos + 6, (uint16_t)name_len);
        memcpy(block + new_pos + 8, name, name_len);
        if (write_exact(fs, (uint64_t)physical * fs->block_size,
                        block, fs->block_size) < 0) {
          free(block);
          return -1;
        }
        free(block);
        return 0;
      }
      pos += rec_len;
    }
  }
  free(block);
  fprintf(stderr, "no room in root directory; directory growth is unsupported\n");
  return -1;
}

static int
release_direct_blocks(struct ext2_fs *fs, uint8_t *raw)
{
  uint32_t sectors = le32(raw + 28);
  if (le32(raw + 40 + 12 * 4) || le32(raw + 40 + 13 * 4) ||
      le32(raw + 40 + 14 * 4)) {
    fprintf(stderr, "truncate of indirect-block files is not yet supported\n");
    return -1;
  }
  for (unsigned i = 0; i < EXT2_NDIR_BLOCKS; i++) {
    uint32_t block = le32(raw + 40 + i * 4);
    if (block != 0) {
      if (free_block(fs, block) < 0)
        return -1;
      put_le32(raw + 40 + i * 4, 0);
      if (sectors < fs->block_size / 512)
        return -1;
      sectors -= fs->block_size / 512;
    }
  }
  put_le32(raw + 28, sectors);
  put_le32(raw + 4, 0);
  put_le32(raw + 108, 0);
  return 0;
}

static int
write_host_file(struct ext2_fs *fs, const char *source, const char *dest)
{
  const char *name;
  size_t name_len;
  struct stat st;
  struct ext2_inode existing;
  uint32_t ino_num;
  uint8_t *raw = NULL, *data = NULL;
  int source_fd = -1;
  int found;
  uint64_t size;
  uint32_t needed_blocks;

  if (require_simple_writable_fs(fs) < 0 || root_name(dest, &name, &name_len) < 0) {
    fprintf(stderr, "destination must be a root-level path such as /file.txt\n");
    return -1;
  }
  source_fd = open(source, O_RDONLY);
  if (source_fd < 0 || fstat(source_fd, &st) < 0 || st.st_size < 0) {
    fprintf(stderr, "cannot read source file %s: %s\n", source, strerror(errno));
    if (source_fd >= 0)
      close(source_fd);
    return -1;
  }
  size = (uint64_t)st.st_size;
  needed_blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);
  if (needed_blocks > EXT2_NDIR_BLOCKS) {
    fprintf(stderr, "write supports files up to %u bytes\n",
            EXT2_NDIR_BLOCKS * fs->block_size);
    close(source_fd);
    return -1;
  }
  data = size ? malloc((size_t)size) : NULL;
  if (size && data == NULL) {
    close(source_fd);
    return -1;
  }
  size_t got = 0;
  while (got < size) {
    ssize_t n = read(source_fd, data + got, (size_t)size - got);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      fprintf(stderr, "short read from source file\n");
      close(source_fd);
      free(data);
      return -1;
    }
    got += (size_t)n;
  }
  close(source_fd);

  found = root_lookup(fs, name, name_len, &ino_num);
  if (found < 0) {
    free(data);
    return -1;
  }
  if (found == 0) {
    if (read_inode(fs, ino_num, &existing) < 0 ||
        (existing.mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
      fprintf(stderr, "destination exists and is not a regular file\n");
      free(data);
      return -1;
    }
    if (read_inode_raw(fs, ino_num, &raw) < 0 ||
        release_direct_blocks(fs, raw) < 0) {
      free(raw);
      free(data);
      return -1;
    }
  } else {
    if (alloc_inode(fs, &ino_num) < 0) {
      free(data);
      return -1;
    }
    raw = calloc(1, fs->inode_size);
    if (raw == NULL) {
      free(data);
      return -1;
    }
    put_le16(raw, EXT2_S_IFREG | 0644);
    put_le16(raw + 26, 1);
  }

  for (uint32_t i = 0; i < needed_blocks; i++) {
    uint32_t block;
    uint8_t *block_data = calloc(1, fs->block_size);
    size_t offset = (size_t)i * fs->block_size;
    size_t chunk = size - offset < fs->block_size ?
                   (size_t)(size - offset) : fs->block_size;
    if (block_data == NULL || alloc_block(fs, &block) < 0) {
      free(block_data);
      free(raw);
      free(data);
      return -1;
    }
    memcpy(block_data, data + offset, chunk);
    if (write_exact(fs, (uint64_t)block * fs->block_size,
                    block_data, fs->block_size) < 0) {
      free(block_data);
      free(raw);
      free(data);
      return -1;
    }
    free(block_data);
    put_le32(raw + 40 + i * 4, block);
  }
  put_le32(raw + 4, (uint32_t)size);
  put_le32(raw + 28, needed_blocks * (fs->block_size / 512));
  if (write_inode_record(fs, ino_num, raw) < 0) {
    free(raw);
    free(data);
    return -1;
  }
  if (found != 0 && root_add_entry(fs, name, name_len, ino_num) < 0) {
    free(raw);
    free(data);
    return -1;
  }
  if (fsync(fs->fd) < 0) {
    fprintf(stderr, "fsync failed: %s\n", strerror(errno));
    free(raw);
    free(data);
    return -1;
  }
  printf("%s %s inode %" PRIu32 " (%" PRIu64 " bytes)\n",
         found == 0 ? "updated" : "created", dest, ino_num, size);
  free(raw);
  free(data);
  return 0;
}

static int
truncate_file_zero(struct ext2_fs *fs, const char *path)
{
  const char *name;
  size_t name_len;
  uint32_t ino_num;
  struct ext2_inode ino;
  uint8_t *raw;
  int found;

  if (require_simple_writable_fs(fs) < 0 || root_name(path, &name, &name_len) < 0) {
    fprintf(stderr, "path must be a root-level file such as /file.txt\n");
    return -1;
  }
  found = root_lookup(fs, name, name_len, &ino_num);
  if (found != 0) {
    fprintf(stderr, "file not found: %s\n", path);
    return -1;
  }
  if (read_inode(fs, ino_num, &ino) < 0 ||
      (ino.mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
      read_inode_raw(fs, ino_num, &raw) < 0)
    return -1;
  if (release_direct_blocks(fs, raw) < 0 ||
      write_inode_record(fs, ino_num, raw) < 0) {
    free(raw);
    return -1;
  }
  if (fsync(fs->fd) < 0) {
    free(raw);
    return -1;
  }
  printf("truncated %s to zero bytes\n", path);
  free(raw);
  return 0;
}

static uint64_t
inode_size(const struct ext2_inode *ino)
{
  return ((uint64_t)ino->size_hi << 32) | ino->size_lo;
}

static int
indirect_lookup(struct ext2_fs *fs, uint32_t block, uint64_t index,
                uint32_t *out)
{
  uint8_t *buf;
  uint64_t entries = fs->block_size / 4;

  if (block == 0) {
    *out = 0;
    return 0;
  }
  if (index >= entries) {
    fprintf(stderr, "indirect block index out of range\n");
    return -1;
  }
  buf = malloc(fs->block_size);
  if (buf == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  if (read_block(fs, block, buf) < 0) {
    free(buf);
    return -1;
  }
  *out = le32(buf + index * 4);
  free(buf);
  if (*out >= fs->blocks_count && *out != 0) {
    fprintf(stderr, "invalid block pointer: %" PRIu32 "\n", *out);
    return -1;
  }
  return 0;
}

static int
map_file_block(struct ext2_fs *fs, const struct ext2_inode *ino,
               uint64_t logical, uint32_t *physical)
{
  uint64_t n = fs->block_size / 4;
  uint64_t i;

  if (logical < EXT2_NDIR_BLOCKS) {
    *physical = ino->blocks[logical];
  } else if ((logical -= EXT2_NDIR_BLOCKS) < n) {
    if (indirect_lookup(fs, ino->blocks[12], logical, physical) < 0)
      return -1;
  } else if ((logical -= n) < n * n) {
    uint32_t middle;
    if (indirect_lookup(fs, ino->blocks[13], logical / n, &middle) < 0 ||
        indirect_lookup(fs, middle, logical % n, physical) < 0)
      return -1;
  } else if ((logical -= n * n) < n * n * n) {
    uint32_t first, middle;
    i = logical / (n * n);
    if (indirect_lookup(fs, ino->blocks[14], i, &first) < 0 ||
        indirect_lookup(fs, first, (logical / n) % n, &middle) < 0 ||
        indirect_lookup(fs, middle, logical % n, physical) < 0)
      return -1;
  } else {
    fprintf(stderr, "file exceeds supported ext2 block-pointer range\n");
    return -1;
  }

  if (*physical != 0 && *physical >= fs->blocks_count) {
    fprintf(stderr, "invalid file data block: %" PRIu32 "\n", *physical);
    return -1;
  }
  return 0;
}

static ssize_t
read_inode_at(struct ext2_fs *fs, const struct ext2_inode *ino,
              uint64_t offset, void *dst, size_t count)
{
  uint8_t *out = dst;
  uint64_t size = inode_size(ino);
  size_t done = 0;
  uint8_t *block_buf = NULL;

  if (offset >= size || count == 0)
    return 0;
  if ((uint64_t)count > size - offset)
    count = (size_t)(size - offset);
  block_buf = malloc(fs->block_size);
  if (block_buf == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  while (done < count) {
    uint64_t pos = offset + done;
    uint64_t logical = pos / fs->block_size;
    uint32_t within = (uint32_t)(pos % fs->block_size);
    size_t chunk = fs->block_size - within;
    uint32_t physical;

    if (chunk > count - done)
      chunk = count - done;
    if (map_file_block(fs, ino, logical, &physical) < 0) {
      free(block_buf);
      return -1;
    }
    if (physical == 0) {
      memset(out + done, 0, chunk); /* Sparse-file hole. */
    } else {
      if (read_block(fs, physical, block_buf) < 0) {
        free(block_buf);
        return -1;
      }
      memcpy(out + done, block_buf + within, chunk);
    }
    done += chunk;
  }
  free(block_buf);
  return (ssize_t)done;
}

static int
directory_lookup(struct ext2_fs *fs, const struct ext2_inode *dir,
                 const char *name, size_t name_len, uint32_t *out_ino)
{
  uint64_t size = inode_size(dir);
  uint8_t *buf = malloc(fs->block_size);

  if ((dir->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
    fprintf(stderr, "lookup target is not a directory\n");
    free(buf);
    return -1;
  }
  if (buf == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  for (uint64_t base = 0; base < size; base += fs->block_size) {
    uint32_t physical;
    size_t valid = (size - base < fs->block_size) ?
                   (size_t)(size - base) : fs->block_size;
    size_t pos = 0;

    if (map_file_block(fs, dir, base / fs->block_size, &physical) < 0) {
      free(buf);
      return -1;
    }
    if (physical == 0)
      continue;
    if (read_block(fs, physical, buf) < 0) {
      free(buf);
      return -1;
    }
    while (pos < valid) {
      uint32_t ino_num;
      uint16_t rec_len;
      uint16_t entry_name_len;
      size_t header_len = 8;

      if (valid - pos < 8) {
        fprintf(stderr, "truncated ext2 directory entry\n");
        free(buf);
        return -1;
      }
      ino_num = le32(buf + pos);
      rec_len = le16(buf + pos + 4);
      entry_name_len = fs->dirent_has_type ? buf[pos + 6] :
                                             le16(buf + pos + 6);
      if (rec_len < 8 || (rec_len & 3) != 0 || rec_len > valid - pos ||
          entry_name_len > rec_len - header_len) {
        fprintf(stderr, "malformed ext2 directory entry\n");
        free(buf);
        return -1;
      }
      if (ino_num != 0 && entry_name_len == name_len &&
          memcmp(buf + pos + header_len, name, name_len) == 0) {
        if (ino_num > fs->inodes_count) {
          fprintf(stderr, "directory contains invalid inode number\n");
          free(buf);
          return -1;
        }
        *out_ino = ino_num;
        free(buf);
        return 0;
      }
      pos += rec_len;
    }
  }
  free(buf);
  return 1; /* Not found. */
}

static int
resolve_path(struct ext2_fs *fs, const char *path, struct ext2_inode *out)
{
  char *copy, *save = NULL, *part;
  struct ext2_inode current;

  if (read_inode(fs, EXT2_ROOT_INO, &current) < 0)
    return -1;
  copy = strdup(path);
  if (copy == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  for (part = strtok_r(copy, "/", &save); part != NULL;
       part = strtok_r(NULL, "/", &save)) {
    uint32_t child;
    int found;
    if (strcmp(part, ".") == 0)
      continue;
    if ((current.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
      fprintf(stderr, "path component is not a directory: %s\n", part);
      free(copy);
      return -1;
    }
    found = directory_lookup(fs, &current, part, strlen(part), &child);
    if (found != 0) {
      if (found > 0)
        fprintf(stderr, "path not found: %s\n", path);
      free(copy);
      return -1;
    }
    if (read_inode(fs, child, &current) < 0) {
      free(copy);
      return -1;
    }
    if ((current.mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
      fprintf(stderr, "symbolic links are not supported by this reader\n");
      free(copy);
      return -1;
    }
  }
  free(copy);
  *out = current;
  return 0;
}

static const char *
inode_type(uint16_t mode)
{
  switch (mode & EXT2_S_IFMT) {
  case EXT2_S_IFREG: return "regular";
  case EXT2_S_IFDIR: return "directory";
  case EXT2_S_IFLNK: return "symlink";
  default: return "other";
  }
}

static void
print_info(const struct ext2_fs *fs)
{
  printf("magic: 0x%04x\n", EXT2_MAGIC);
  printf("image_bytes: %" PRIu64 "\n", fs->image_bytes);
  printf("block_size: %" PRIu32 "\n", fs->block_size);
  printf("blocks_count: %" PRIu32 "\n", fs->blocks_count);
  printf("inodes_count: %" PRIu32 "\n", fs->inodes_count);
  printf("first_data_block: %" PRIu32 "\n", fs->first_data_block);
  printf("blocks_per_group: %" PRIu32 "\n", fs->blocks_per_group);
  printf("inodes_per_group: %" PRIu32 "\n", fs->inodes_per_group);
  printf("inode_size: %" PRIu16 "\n", fs->inode_size);
  printf("feature_compat: 0x%08" PRIx32 "\n", fs->feature_compat);
  printf("feature_incompat: 0x%08" PRIx32 "\n", fs->feature_incompat);
  printf("feature_ro_compat: 0x%08" PRIx32 "\n", fs->feature_ro_compat);
}

static int
command_stat(struct ext2_fs *fs, const char *path)
{
  struct ext2_inode ino;
  if (resolve_path(fs, path, &ino) < 0)
    return -1;
  printf("inode: %" PRIu32 "\n", ino.number);
  printf("type: %s\n", inode_type(ino.mode));
  printf("mode: 0%o\n", ino.mode & 07777);
  printf("size: %" PRIu64 "\n", inode_size(&ino));
  printf("sectors_512: %" PRIu32 "\n", ino.sectors);
  printf("direct_blocks:");
  for (unsigned i = 0; i < EXT2_NDIR_BLOCKS; i++)
    if (ino.blocks[i] != 0)
      printf(" %" PRIu32, ino.blocks[i]);
  printf("\n");
  return 0;
}

static int
command_ls(struct ext2_fs *fs, const char *path)
{
  struct ext2_inode dir;
  uint64_t size;
  uint8_t *buf;

  if (resolve_path(fs, path, &dir) < 0)
    return -1;
  if ((dir.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
    fprintf(stderr, "not a directory: %s\n", path);
    return -1;
  }
  size = inode_size(&dir);
  buf = malloc(fs->block_size);
  if (buf == NULL) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  for (uint64_t base = 0; base < size; base += fs->block_size) {
    uint32_t physical;
    size_t valid = (size - base < fs->block_size) ?
                   (size_t)(size - base) : fs->block_size;
    size_t pos = 0;

    if (map_file_block(fs, &dir, base / fs->block_size, &physical) < 0) {
      free(buf);
      return -1;
    }
    if (physical == 0)
      continue;
    if (read_block(fs, physical, buf) < 0) {
      free(buf);
      return -1;
    }
    while (pos < valid) {
      uint32_t ino_num = le32(buf + pos);
      uint16_t rec_len = le16(buf + pos + 4);
      uint16_t name_len = fs->dirent_has_type ? buf[pos + 6] :
                                                le16(buf + pos + 6);
      if (rec_len < 8 || (rec_len & 3) != 0 || rec_len > valid - pos ||
          name_len > rec_len - 8) {
        fprintf(stderr, "malformed ext2 directory entry\n");
        free(buf);
        return -1;
      }
      if (ino_num != 0) {
        char name[256];
        size_t copy_len = name_len < sizeof(name) - 1 ? name_len :
                                                       sizeof(name) - 1;
        memcpy(name, buf + pos + 8, copy_len);
        name[copy_len] = '\0';
        printf("%8" PRIu32 " %s\n", ino_num, name);
      }
      pos += rec_len;
    }
  }
  free(buf);
  return 0;
}

static int
command_cat(struct ext2_fs *fs, const char *path)
{
  struct ext2_inode ino;
  uint8_t buf[4096];
  uint64_t offset = 0;

  if (resolve_path(fs, path, &ino) < 0)
    return -1;
  if ((ino.mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
    fprintf(stderr, "not a regular file: %s\n", path);
    return -1;
  }
  while (offset < inode_size(&ino)) {
    size_t want = sizeof(buf);
    ssize_t got;
    if ((uint64_t)want > inode_size(&ino) - offset)
      want = (size_t)(inode_size(&ino) - offset);
    got = read_inode_at(fs, &ino, offset, buf, want);
    if (got <= 0) {
      fprintf(stderr, "file read failed\n");
      return -1;
    }
    size_t written = 0;
    while (written < (size_t)got) {
      ssize_t n = write(STDOUT_FILENO, buf + written, (size_t)got - written);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        fprintf(stderr, "stdout write failed: %s\n", strerror(errno));
        return -1;
      }
      written += (size_t)n;
    }
    offset += (uint64_t)got;
  }
  return 0;
}

static int
parse_u64(const char *text, uint64_t *out)
{
  char *end;
  unsigned long long value;

  if (text[0] == '\0' || text[0] == '-')
    return -1;
  errno = 0;
  value = strtoull(text, &end, 0);
  if (errno != 0 || *end != '\0')
    return -1;
  *out = (uint64_t)value;
  return 0;
}

static int
command_read(struct ext2_fs *fs, const char *path, const char *offset_text,
             const char *count_text)
{
  struct ext2_inode ino;
  uint64_t offset, count;
  uint8_t buf[4096];

  if (parse_u64(offset_text, &offset) < 0 ||
      parse_u64(count_text, &count) < 0) {
    fprintf(stderr, "offset and count must be non-negative integers\n");
    return -1;
  }
  if (resolve_path(fs, path, &ino) < 0)
    return -1;
  if ((ino.mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
    fprintf(stderr, "not a regular file: %s\n", path);
    return -1;
  }
  while (count > 0) {
    size_t want = count < sizeof(buf) ? (size_t)count : sizeof(buf);
    ssize_t got = read_inode_at(fs, &ino, offset, buf, want);
    size_t written = 0;

    if (got < 0) {
      fprintf(stderr, "file read failed\n");
      return -1;
    }
    if (got == 0)
      break;
    while (written < (size_t)got) {
      ssize_t n = write(STDOUT_FILENO, buf + written, (size_t)got - written);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        fprintf(stderr, "stdout write failed: %s\n", strerror(errno));
        return -1;
      }
      written += (size_t)n;
    }
    offset += (uint64_t)got;
    count -= (uint64_t)got;
  }
  return 0;
}

static void
usage(const char *program)
{
  fprintf(stderr,
          "usage:\n"
          "  %s IMAGE info\n"
          "  %s IMAGE ls PATH\n"
          "  %s IMAGE stat PATH\n"
          "  %s IMAGE cat PATH\n"
          "  %s IMAGE read PATH OFFSET COUNT\n"
          "  %s IMAGE put HOST_FILE /ROOT_FILE\n"
          "  %s IMAGE truncate /ROOT_FILE\n",
          program, program, program, program, program, program, program);
}

int
main(int argc, char **argv)
{
  struct ext2_fs fs;
  int result;
  int writable;

  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  writable = (strcmp(argv[2], "put") == 0 ||
              strcmp(argv[2], "truncate") == 0);
  if (open_ext2(&fs, argv[1], writable) < 0)
    return 1;

  if (strcmp(argv[2], "info") == 0 && argc == 3) {
    print_info(&fs);
    result = 0;
  } else if (strcmp(argv[2], "ls") == 0 && argc == 4) {
    result = command_ls(&fs, argv[3]);
  } else if (strcmp(argv[2], "stat") == 0 && argc == 4) {
    result = command_stat(&fs, argv[3]);
  } else if (strcmp(argv[2], "cat") == 0 && argc == 4) {
    result = command_cat(&fs, argv[3]);
  } else if (strcmp(argv[2], "read") == 0 && argc == 6) {
    result = command_read(&fs, argv[3], argv[4], argv[5]);
  } else if (strcmp(argv[2], "put") == 0 && argc == 5) {
    result = write_host_file(&fs, argv[3], argv[4]);
  } else if (strcmp(argv[2], "truncate") == 0 && argc == 4) {
    result = truncate_file_zero(&fs, argv[3]);
  } else {
    usage(argv[0]);
    close_ext2(&fs);
    return 2;
  }
  close_ext2(&fs);
  return result == 0 ? 0 : 1;
}
