#ifdef FAT32_HOST_TEST
#include "../tests/fat32_host.h"
#else
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "blkdev.h"
#include "vfs.h"
#include "vfs_dirent.h"
#include "vfs_backend.h"
#include "fat32.h"
#endif

#define FAT32_EOC       0x0ffffff8U
#define FAT32_BAD       0x0ffffff7U
#define FAT32_RESERVED  0x0ffffff0U
#define FAT32_MAX_CLUST 0x0fffffeeU
#define FAT32_EOD       0x00
#define FAT32_DELETED   0xe5
#define FAT32_ATTR_LFN  0x0f
#define FAT32_LFN_MASK  0x3f
#define FAT32_LFN_LAST  0x40
#define FAT32_LFN_UNITS 13
#define FAT32_LFN_SLOTS 20
#define FAT32_ATTR_VOL  0x08
#define FAT32_ATTR_DIR  0x10
#define FAT32_ATTR_FILE 0x20
#define FAT32_ENTRY_SIZE 32
#define FAT32_FAT_MASK  0x0fffffffU
#define FAT32_UNKNOWN   0xffffffffU

struct fat32_fs {
  uint dev;
  uint bytes_per_sector;
  uint sectors_per_cluster;
  uint cluster_bytes;
  uint reserved_sectors;
  uint fat_count;
  uint fat_sectors;
  uint total_sectors;
  uint root_cluster;
  uint cluster_count;
  uint max_cluster;
  uint active_fat;
  int mirrored;
  uint64 volume_bytes;
  uint64 fat_offset;
  uint64 data_offset;
  uint next_free;
  uint fsinfo_sector; // Zero disables the optional primary FSInfo record.
  uint free_count;
  int fsinfo_dirty;
  struct spinlock nodes_lock;
  struct fat32_node *nodes;
  uint next_delete_id;
  struct sleeplock write_lock;
  int write_failed;
};

struct fat32_node {
  struct fat32_fs *fs;
  struct sleeplock lock;
  uint64 ino;
  uint64 entry_offset;
  uint first_cluster;
  uint size;
  uint parent_cluster;
  uint64 parent_ino;
  uint parent_parent_cluster;
  uint64 parent_parent_ino;
  uchar attributes;
  uint delete_id;
  uint deleted_cluster;
  int deleted;
  uint refs; // Backend references; VFS may merge several into one vnode.
  struct fat32_node *next;
};

struct fat32_entry {
  char name[13];
  uint64 offset;
  uint first_cluster;
  uint size;
  uchar attributes;
  int lfn_prefix;
  uint lfn_count;
  uint64 first_slot;
};

// Accumulate the descending slots until the following short entry.
struct fat32_lfn {
  ushort units[FAT32_LFN_SLOTS * FAT32_LFN_UNITS];
  uint slots;
  uint next;
  uchar checksum;
  int present;
};

struct fat32_name {
  ushort units[FAT32_LFN_SLOTS * FAT32_LFN_UNITS];
  uint length;
  uint count;
  uchar alias[11];
  uint64 offsets[FAT32_LFN_SLOTS + 1];
  uint64 end_marker;
  uint old_tail;
  uint added;
};

// Brent cycle detection state, bounded by the number of data clusters.
struct fat32_chain {
  uint checkpoint;
  uint power;
  uint length;
  uint steps;
};

static const struct vnode_ops fat32_node_ops;
static const struct vfs_fs_ops fat32_fs_ops;

static ushort
fat32_get16(const uchar *p)
{
  return (ushort)p[0] | ((ushort)p[1] << 8);
}

static uint
fat32_get32(const uchar *p)
{
  return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) |
         ((uint)p[3] << 24);
}

static void
fat32_put16(uchar *p, ushort value)
{
  p[0] = value;
  p[1] = value >> 8;
}

static void
fat32_put32(uchar *p, uint value)
{
  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

static int
fat32_power_of_two(uint value)
{
  return value != 0 && (value & (value - 1)) == 0;
}

static int
fat32_allowed_char(uchar c)
{
  static const char extra[] = "_$~!#%&'()-@^`{}";
  for(uint i = 0; extra[i] != 0; i++)
    if(c == (uchar)extra[i])
      return 1;
  return 0;
}

static int
fat32_read_bytes(struct fat32_fs *fs, uint64 offset, void *dst, uint length)
{
  if(offset > fs->volume_bytes || (uint64)length > fs->volume_bytes - offset)
    return -1;
  if(blk_read(fs->dev, offset, dst, length) < 0){
    fs->write_failed = 1;
    return -1;
  }
  return 0;
}

static int
fat32_write_bytes(struct fat32_fs *fs, uint64 offset, const void *src,
                  uint length)
{
  if(fs->write_failed || offset > fs->volume_bytes ||
     (uint64)length > fs->volume_bytes - offset)
    return -1;
  if(blk_write(fs->dev, offset, src, length) < 0){
    fs->write_failed = 1;
    return -1;
  }
  return 0;
}

static int
fat32_flush(struct fat32_fs *fs)
{
  uchar hints[8];
  if(fs->write_failed)
    return -1;
  // Persist FAT/data updates before publishing their allocation hints.
  if(blk_flush(fs->dev) < 0){
    fs->write_failed = 1;
    return -1;
  }
  if(fs->fsinfo_sector && fs->fsinfo_dirty){
    fat32_put32(hints, fs->free_count);
    fat32_put32(hints + 4, fs->next_free >= 2 && fs->next_free <= fs->max_cluster ?
                              fs->next_free : FAT32_UNKNOWN);
    if(fat32_write_bytes(fs, (uint64)fs->fsinfo_sector * fs->bytes_per_sector + 488,
                         hints, sizeof(hints)) < 0)
      return -1;
    if(blk_flush(fs->dev) < 0){
      fs->write_failed = 1;
      return -1;
    }
    fs->fsinfo_dirty = 0;
  }
  return 0;
}

static int
fat32_cluster_offset(struct fat32_fs *fs, uint cluster, uint64 *offset)
{
  if(cluster < 2 || cluster > fs->max_cluster)
    return -1;
  *offset = fs->data_offset + (uint64)(cluster - 2) * fs->cluster_bytes;
  if(*offset > fs->volume_bytes ||
     (uint64)fs->cluster_bytes > fs->volume_bytes - *offset)
    return -1;
  return 0;
}

static int
fat32_fat_value(struct fat32_fs *fs, uint cluster, uint *value_out)
{
  uchar raw[4];
  uint value;
  uint64 fat_bytes = (uint64)fs->fat_sectors * fs->bytes_per_sector;

  if(cluster < 2 || cluster > fs->max_cluster ||
     fat32_read_bytes(fs, fs->fat_offset + (uint64)cluster * 4,
                      raw, sizeof(raw)) < 0)
    return -1;
  value = fat32_get32(raw) & FAT32_FAT_MASK;
  if(fs->mirrored){
    for(uint i = 1; i < fs->fat_count; i++){
      uint64 offset = fs->fat_offset + (uint64)i * fat_bytes +
                      (uint64)cluster * 4;
      if(fat32_read_bytes(fs, offset, raw, sizeof(raw)) < 0 ||
         (fat32_get32(raw) & FAT32_FAT_MASK) != value)
        return -1;
    }
  }
  *value_out = value;
  return 0;
}

static int
fat32_next(struct fat32_fs *fs, uint cluster, uint *next)
{
  uint value;

  if(fat32_fat_value(fs, cluster, &value) < 0)
    return -1;
  if(value >= FAT32_EOC)
    return 0;
  if(value == 0 || value == 1 || value == FAT32_BAD ||
     value >= FAT32_RESERVED || value > fs->max_cluster)
    return -1;
  *next = value;
  return 1;
}

static int
fat32_write_fat_value(struct fat32_fs *fs, uint cluster, uint value)
{
  uchar raw[4];
  uint copies, first, old = 0;
  if(cluster < 2 || cluster > fs->max_cluster)
    return -1;
  if(fs->mirrored){
    copies = fs->fat_count;
    first = 0;
  } else {
    copies = 1;
    first = fs->active_fat;
  }
  for(uint i = 0; i < copies; i++){
    uint64 offset = ((uint64)fs->reserved_sectors +
                     (uint64)(first + i) * fs->fat_sectors) *
                    fs->bytes_per_sector + (uint64)cluster * 4;
    if(fat32_read_bytes(fs, offset, raw, sizeof(raw)) < 0){
      fs->write_failed = 1;
      return -1;
    }
    if(i == 0)
      old = fat32_get32(raw) & FAT32_FAT_MASK;
    // Only the low 28 bits belong to the cluster chain.
    fat32_put32(raw, (fat32_get32(raw) & ~FAT32_FAT_MASK) |
                     (value & FAT32_FAT_MASK));
    if(fat32_write_bytes(fs, offset, raw, sizeof(raw)) < 0)
      return -1;
  }
  // Account transitions once, only after all required FAT copies succeeded.
  // FSInfo is a hint, never an authority for ENOSPC or a cluster's ownership.
  if(fs->fsinfo_sector && ((old == 0) != ((value & FAT32_FAT_MASK) == 0))){
    if(fs->free_count != FAT32_UNKNOWN){
      if(old == 0)
        fs->free_count = fs->free_count ? fs->free_count - 1 : FAT32_UNKNOWN;
      else
        fs->free_count = fs->free_count < fs->cluster_count ?
                           fs->free_count + 1 : FAT32_UNKNOWN;
    }
    fs->fsinfo_dirty = 1;
  }
  return 0;
}

static void
fat32_chain_init(struct fat32_chain *chain, uint start)
{
  chain->checkpoint = start;
  chain->power = 1;
  chain->length = 0;
  chain->steps = 0;
}

// Return 1 with next cluster, 0 at EOC, or -1 for malformed/cyclic chains.
static int
fat32_chain_advance(struct fat32_fs *fs, struct fat32_chain *chain,
                    uint current, uint *next)
{
  uint value;
  int r = fat32_next(fs, current, &value);
  if(r <= 0)
    return r;
  if(++chain->steps > fs->cluster_count || value == chain->checkpoint)
    return -1;
  chain->length++;
  if(chain->length == chain->power){
    chain->checkpoint = value;
    if(chain->power <= fs->cluster_count / 2)
      chain->power *= 2;
    chain->length = 0;
  }
  *next = value;
  return 1;
}

static int
fat32_zero_cluster(struct fat32_fs *fs, uint cluster)
{
  uchar *buf = kalloc();
  uint64 offset;
  int r = -1;

  if(buf == 0 || fat32_cluster_offset(fs, cluster, &offset) < 0)
    goto done;
  memset(buf, 0, PGSIZE);
  for(uint pos = 0; pos < fs->cluster_bytes;){
    uint take = fs->cluster_bytes - pos;
    if(take > PGSIZE)
      take = PGSIZE;
    if(fat32_write_bytes(fs, offset + pos, buf, take) < 0)
      goto done;
    pos += take;
  }
  r = 0;
done:
  if(buf)
    kfree(buf);
  return r;
}

// Allocate one free cluster, mark it EOC, and clear its data before exposing it.
static int
fat32_alloc_cluster(struct fat32_fs *fs, uint *out_cluster)
{
  uint start = fs->next_free < 2 ? 2 : fs->next_free;
  uint count = fs->cluster_count;

  for(uint pass = 0; pass < 2; pass++){
    uint first = pass == 0 ? start : 2;
    uint last = pass == 0 ? fs->max_cluster : start - 1;
    if(first > fs->max_cluster)
      continue;
    for(uint cluster = first; cluster <= last; cluster++){
      uint value;
      if(fat32_fat_value(fs, cluster, &value) < 0)
        return -1;
      if(value != 0)
        continue;
      if(fat32_write_fat_value(fs, cluster, FAT32_EOC) < 0)
        return -1;
      if(fat32_zero_cluster(fs, cluster) < 0){
        fat32_write_fat_value(fs, cluster, 0);
        return -1;
      }
      fs->next_free = cluster == fs->max_cluster ? 2 : cluster + 1;
      *out_cluster = cluster;
      return 0;
    }
    if(pass == 1 || count == 0)
      break;
  }
  return -1;
}

static int
fat32_free_chain(struct fat32_fs *fs, uint first)
{
  uint current = first;
  uint steps = 0;

  while(current >= 2 && current <= fs->max_cluster &&
        steps++ <= fs->cluster_count){
    uint next_value, next = 0;
    if(fat32_fat_value(fs, current, &next_value) < 0)
      return -1;
    if(next_value < FAT32_EOC){
      if(next_value < 2 || next_value > fs->max_cluster)
        return -1;
      next = next_value;
    }
    if(fat32_write_fat_value(fs, current, 0) < 0)
      return -1;
    current = next;
  }
  if(current != 0)
    return -1;
  if(first >= 2){
    fs->next_free = first;
    if(fs->fsinfo_sector)
      fs->fsinfo_dirty = 1;
  }
  return 0;
}

static int
fat32_chain_length(struct fat32_fs *fs, uint first, uint *tail, uint *count)
{
  struct fat32_chain chain;
  uint current, length;

  *tail = 0;
  *count = 0;
  if(first == 0)
    return 0;
  if(first < 2 || first > fs->max_cluster)
    return -1;
  current = first;
  length = 1;
  fat32_chain_init(&chain, current);
  for(;;){
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r < 0)
      return -1;
    if(r == 0)
      break;
    current = next;
    if(++length > fs->cluster_count)
      return -1;
  }
  *tail = current;
  *count = length;
  return 0;
}

// Check the whole chain before changing it, including that it can hold size.
static int
fat32_file_chain(struct fat32_fs *fs, uint first, uint size,
                 uint *tail, uint *count)
{
  if(fat32_chain_length(fs, first, tail, count) < 0 ||
     (uint64)*count * fs->cluster_bytes < size)
    return -1;
  return 0;
}

static int
fat32_append_cluster(struct fat32_fs *fs, uint tail, uint *out_cluster)
{
  uint cluster;

  if(fat32_alloc_cluster(fs, &cluster) < 0)
    return -1;
  if(tail != 0 && fat32_write_fat_value(fs, tail, cluster) < 0){
    fat32_write_fat_value(fs, cluster, 0);
    return -1;
  }
  *out_cluster = cluster;
  return 0;
}

// Invalid/absent optional hints are ignored, not repaired during mounting.
static int
fat32_load_fsinfo(struct fat32_fs *fs, uint sector)
{
  uchar *info;
  fs->next_free = 2;
  fs->free_count = FAT32_UNKNOWN;
  fs->fsinfo_sector = 0;
  fs->fsinfo_dirty = 0;
  if(sector == 0 || sector == 0xffff || sector >= fs->reserved_sectors)
    return 0;
  info = kalloc();
  if(info == 0)
    return -1;
  if(fat32_read_bytes(fs, (uint64)sector * fs->bytes_per_sector, info, 512) < 0){
    kfree(info);
    return -1;
  }
  if(fat32_get32(info) == 0x41615252U &&
     fat32_get32(info + 484) == 0x61417272U &&
     fat32_get32(info + 508) == 0xaa550000U){
    uint count = fat32_get32(info + 488);
    uint next = fat32_get32(info + 492);
    fs->fsinfo_sector = sector;
    if(count <= fs->cluster_count)
      fs->free_count = count;
    if(next >= 2 && next <= fs->max_cluster)
      fs->next_free = next;
  }
  kfree(info);
  return 0;
}

static int
fat32_load(struct fat32_fs *fs)
{
  uchar *boot;
  uint total16, total32, fat16, fat32, root_entries, ext_flags;
  uint64 overhead, fat_bytes, fat_entries, device_bytes;
  uint data_sectors;
  uint root_tail, root_clusters;
  uint info_sector;

  if(blk_size(fs->dev, &device_bytes) < 0 || device_bytes < 512)
    return -1;
  boot = kalloc();
  if(boot == 0)
    return -1;
  if(blk_read(fs->dev, 0, boot, 512) < 0 ||
     boot[510] != 0x55 || boot[511] != 0xaa){
    kfree(boot);
    return -1;
  }
  fs->bytes_per_sector = fat32_get16(boot + 11);
  fs->sectors_per_cluster = boot[13];
  fs->reserved_sectors = fat32_get16(boot + 14);
  fs->fat_count = boot[16];
  root_entries = fat32_get16(boot + 17);
  total16 = fat32_get16(boot + 19);
  fat16 = fat32_get16(boot + 22);
  total32 = fat32_get32(boot + 32);
  fat32 = fat32_get32(boot + 36);
  ext_flags = fat32_get16(boot + 40);
  fs->root_cluster = fat32_get32(boot + 44) & 0x0fffffffU;
  info_sector = fat32_get16(boot + 48);
  if(fat32_get16(boot + 42) != 0 || (ext_flags & 0xff70U) != 0 ||
     !fat32_power_of_two(fs->bytes_per_sector) ||
     fs->bytes_per_sector < 512 || fs->bytes_per_sector > PGSIZE ||
     !fat32_power_of_two(fs->sectors_per_cluster) ||
     fs->sectors_per_cluster > 128 || fs->reserved_sectors == 0 ||
     fs->fat_count == 0 || fs->fat_count > 8 || root_entries != 0 ||
     fat16 != 0 || fat32 == 0 || fat32_get32(boot + 28) != 0 ||
     (total16 != 0 && total32 != 0)){
    kfree(boot);
    return -1;
  }
  fs->total_sectors = total16 != 0 ? total16 : total32;
  fs->fat_sectors = fat32;
  if(fs->total_sectors == 0){
    kfree(boot);
    return -1;
  }
  fs->mirrored = (ext_flags & 0x0080U) == 0;
  if(!fs->mirrored){
    fs->active_fat = ext_flags & 0x000fU;
    if(fs->active_fat >= fs->fat_count){
      kfree(boot);
      return -1;
    }
  }
  kfree(boot);

  fs->volume_bytes = (uint64)fs->total_sectors * fs->bytes_per_sector;
  overhead = (uint64)fs->reserved_sectors +
             (uint64)fs->fat_count * fs->fat_sectors;
  if(fs->volume_bytes > device_bytes || overhead >= fs->total_sectors)
    return -1;
  data_sectors = fs->total_sectors - overhead;
  fs->cluster_count = data_sectors / fs->sectors_per_cluster;
  if(fs->cluster_count < 65525 || fs->cluster_count > FAT32_MAX_CLUST)
    return -1;
  fs->max_cluster = fs->cluster_count + 1;
  if(fs->root_cluster < 2 || fs->root_cluster > fs->max_cluster)
    return -1;
  fs->cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
  fat_bytes = (uint64)fs->fat_sectors * fs->bytes_per_sector;
  fat_entries = fat_bytes / 4;
  if(fat_entries < (uint64)fs->cluster_count + 2)
    return -1;
  fs->fat_offset = ((uint64)fs->reserved_sectors +
                    (uint64)fs->active_fat * fs->fat_sectors) *
                   fs->bytes_per_sector;
  fs->data_offset = ((uint64)fs->reserved_sectors +
                     (uint64)fs->fat_count * fs->fat_sectors) *
                    fs->bytes_per_sector;
  if(fat32_chain_length(fs, fs->root_cluster, &root_tail, &root_clusters) < 0)
    return -1;
  if(fat32_load_fsinfo(fs, info_sector) < 0)
    return -1;
  initsleeplock(&fs->write_lock, "fat32write");
  return 0;
}

static int
fat32_short_name(const char *name, uint len, uchar out[11])
{
  uint base = 0, ext = 0;
  int dot = 0;
  memset(out, ' ', 11);
  if(len == 0)
    return -1;
  for(uint i = 0; i < len; i++){
    uchar c = name[i];
    if(c == '.'){
      if(dot || base == 0)
        return -1;
      dot = 1;
      continue;
    }
    if(c >= 0x80 || !((c >= 'A' && c <= 'Z') ||
       (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
       fat32_allowed_char(c)))
      return -1;
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    if(!dot){
      if(base >= 8)
        return -1;
      out[base++] = c;
    } else {
      if(ext >= 3)
        return -1;
      out[8 + ext++] = c;
    }
  }
  return base == 0 ? -1 : 0;
}

static void
fat32_format_name(const uchar raw[11], char name[13])
{
  uint base = 8, ext = 3, pos = 0;
  while(base && raw[base - 1] == ' ')
    base--;
  while(ext && raw[8 + ext - 1] == ' ')
    ext--;
  for(uint i = 0; i < base; i++){
    uchar c = raw[i];
    if(i == 0 && c == 0x05)
      c = 0xe5;
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    name[pos++] = c;
  }
  if(ext){
    name[pos++] = '.';
    for(uint i = 0; i < ext; i++){
      uchar c = raw[8 + i];
      if(c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
      name[pos++] = c;
    }
  }
  name[pos] = 0;
}

static void
fat32_decode_entry(const uchar raw[FAT32_ENTRY_SIZE], uint64 offset,
                   struct fat32_entry *entry)
{
  fat32_format_name(raw, entry->name);
  entry->attributes = raw[11];
  entry->first_cluster = ((uint)fat32_get16(raw + 20) << 16) |
                         fat32_get16(raw + 26);
  entry->first_cluster &= 0x0fffffffU;
  entry->size = fat32_get32(raw + 28);
  entry->offset = offset;
}

static int
fat32_name_equal(const uchar raw[11], const uchar wanted[11])
{
  for(uint i = 0; i < 11; i++){
    uchar c = raw[i];
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    if(c != wanted[i])
      return 0;
  }
  return 1;
}

static int
fat32_lfn_slot(const uchar *raw)
{
  return raw[0] != FAT32_DELETED &&
         (raw[11] & FAT32_LFN_MASK) == FAT32_ATTR_LFN;
}

static void
fat32_lfn_push(struct fat32_lfn *lfn, const uchar *raw)
{
  static const uchar offsets[FAT32_LFN_UNITS] =
    {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  uint ordinal = raw[0] & 0x1f;

  lfn->present = 1;
  if((raw[0] & 0xa0) || ordinal == 0 || ordinal > FAT32_LFN_SLOTS ||
     raw[12] != 0 || fat32_get16(raw + 26) != 0){
    lfn->slots = 0;
    return;
  }
  if(raw[0] & FAT32_LFN_LAST){
    lfn->slots = lfn->next = ordinal;
    lfn->checksum = raw[13];
  }
  if(!lfn->slots || ordinal != lfn->next || raw[13] != lfn->checksum){
    lfn->slots = 0;
    return;
  }
  for(uint i = 0; i < FAT32_LFN_UNITS; i++)
    lfn->units[(ordinal - 1) * FAT32_LFN_UNITS + i] =
      fat32_get16(raw + offsets[i]);
  lfn->next--;
}

static int
fat32_lfn_forbidden(uint cp)
{
  static const char forbidden[] = "\"*/:<>?\\|";
  if(cp < 0x20 || cp == 0xfffe || cp == 0xffff)
    return 1;
  for(uint i = 0; forbidden[i]; i++)
    if(cp == (uchar)forbidden[i])
      return 1;
  return 0;
}

// Return UTF-8 length, or zero to fall back to the short alias. Never emit
// a truncated name, an invalid surrogate pair, or a path separator.
static uint
fat32_lfn_name(const struct fat32_lfn *lfn, const uchar *alias, char *out)
{
  uchar sum = 0;
  uint length = 0, units = lfn->slots * FAT32_LFN_UNITS;
  uint end = units;

  if(!lfn->slots || lfn->next != 0)
    return 0;
  for(uint i = 0; i < 11; i++)
    sum = ((sum >> 1) | ((sum & 1) << 7)) + alias[i];
  if(sum != lfn->checksum)
    return 0;
  for(uint i = 0; i < units; i++){
    if(lfn->units[i] == 0){
      if(i <= units - FAT32_LFN_UNITS)
        return 0;
      end = i;
      for(uint j = i + 1; j < units; j++)
        if(lfn->units[j] != 0xffff)
          return 0;
      break;
    }
  }
  if(end > 255)
    return 0;
  for(uint i = 0; i < end; i++){
    uint cp = lfn->units[i];
    if(cp >= 0xd800 && cp <= 0xdbff){
      if(i + 1 == end || lfn->units[i + 1] < 0xdc00 ||
         lfn->units[i + 1] > 0xdfff)
        return 0;
      cp = 0x10000 + ((cp - 0xd800) << 10) + (lfn->units[++i] - 0xdc00);
    } else if((cp >= 0xdc00 && cp <= 0xdfff) || fat32_lfn_forbidden(cp)){
      return 0;
    }
    uint bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if(length + bytes > VFS_NAME_MAX)
      return 0;
    if(bytes == 1){
      out[length] = cp;
    } else {
      static const uchar lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
      for(uint j = bytes - 1; j > 0; j--){
        out[length + j] = 0x80 | (cp & 0x3f);
        cp >>= 6;
      }
      out[length] = lead[bytes] | cp;
    }
    length += bytes;
  }
  if(length != 0 && out[length - 1] != '.' && out[length - 1] != ' '){
    out[length] = 0;
    return length;
  }
  return 0;
}

static int
fat32_lfn_equal(const char *a, const char *b, uint len)
{
  for(uint i = 0; i < len; i++){
    uchar x = a[i], y = b[i];
    if(x >= 'a' && x <= 'z')
      x -= 'a' - 'A';
    if(y >= 'a' && y <= 'z')
      y -= 'a' - 'A';
    if(x != y)
      return 0;
  }
  return 1;
}

// Return 0 and fill entry, 1 if absent, or -1 for malformed data/I/O.
static int
fat32_find_entry(struct fat32_node *dir, const char *name, uint len,
                 struct fat32_entry *entry)
{
  struct fat32_fs *fs = dir->fs;
  uchar *sector = kalloc();
  uchar wanted[11];
  char long_name[VFS_NAME_MAX + 1];
  struct fat32_lfn lfn = {0};
  struct fat32_chain chain;
  uint current = dir->first_cluster;
  uint64 slot_index = 0;
  int short_ok = fat32_short_name(name, len, wanted) == 0;
  int result = -1;

  if(sector == 0 || len == 0 || len > VFS_NAME_MAX ||
     (dir->attributes & FAT32_ATTR_DIR) == 0)
    goto done;
  if(current < 2 || current > fs->max_cluster)
    goto done;
  fat32_chain_init(&chain, current);
  for(;;){
    uint64 cluster_off;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      goto done;
    for(uint in_cluster = 0; in_cluster < fs->cluster_bytes;
        in_cluster += fs->bytes_per_sector){
      uint64 sector_off = cluster_off + in_cluster;
      if(fat32_read_bytes(fs, sector_off, sector, fs->bytes_per_sector) < 0)
        goto done;
      for(uint pos = 0; pos < fs->bytes_per_sector;
          pos += FAT32_ENTRY_SIZE){
        uchar *raw = sector + pos;
        uint64 this_slot = slot_index++;
        if(raw[0] == FAT32_EOD){
          result = 1;
          goto done;
        }
        if(fat32_lfn_slot(raw)){
          fat32_lfn_push(&lfn, raw);
          continue;
        }
        if(raw[0] != FAT32_DELETED && !(raw[11] & FAT32_ATTR_VOL) &&
           raw[0] != '.'){
          uint long_len = fat32_lfn_name(&lfn, raw, long_name);
          if((short_ok && fat32_name_equal(raw, wanted)) ||
             (long_len == len &&
              fat32_lfn_equal(long_name, name, len))){
            fat32_decode_entry(raw, sector_off + pos, entry);
            entry->lfn_prefix = lfn.present;
            entry->lfn_count = long_len ? lfn.slots : 0;
            entry->first_slot = this_slot - entry->lfn_count;
            result = 0;
            goto done;
          }
        }
        lfn.present = lfn.slots = 0;
      }
    }
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r < 0)
      goto done;
    if(r == 0){
      result = 1;
      goto done;
    }
    current = next;
  }
done:
  if(sector)
    kfree(sector);
  return result;
}

// Find a reusable directory entry slot.  A zero entry is an end marker and an
// E5 entry is a deleted slot; both may be reused.  If a directory chain is
// full, return its tail so the caller can append a zeroed cluster.
static int
fat32_slot_deferred(struct fat32_fs *fs, uint64 offset)
{
  struct fat32_node *node;
  int deferred = 0;

  acquire(&fs->nodes_lock);
  for(node = fs->nodes; node; node = node->next)
    if(node->deleted && node->entry_offset == offset){
      deferred = 1;
      break;
    }
  release(&fs->nodes_lock);
  return deferred;
}

// Reserve a run of deleted/end slots, growing the directory if necessary.
// The caller holds write_lock and has not published any entry yet.
static int
fat32_name_slots(struct fat32_node *dir, struct fat32_name *name)
{
  struct fat32_fs *fs = dir->fs;
  uchar *sector = kalloc();
  uint current = dir->first_cluster;
  uint clusters, run = 0, needed = name->count + 1;
  int result = -1, end_seen = 0;

  if(sector == 0 ||
     fat32_chain_length(fs, current, &name->old_tail, &clusters) < 0)
    goto done;
  for(;;){
    uint64 cluster_off;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      goto done;
    for(uint in_cluster = 0; in_cluster < fs->cluster_bytes;
        in_cluster += fs->bytes_per_sector){
      uint64 sector_off = cluster_off + in_cluster;
      if(fat32_read_bytes(fs, sector_off, sector, fs->bytes_per_sector) < 0)
        goto done;
      for(uint pos = 0; pos < fs->bytes_per_sector;
          pos += FAT32_ENTRY_SIZE){
        uint64 candidate = sector_off + pos;
        if(sector[pos] == FAT32_EOD)
          end_seen = 1;
        if((end_seen || sector[pos] == FAT32_DELETED) &&
           !fat32_slot_deferred(fs, candidate)){
          name->offsets[run++] = candidate;
          if(run == needed){
            // Keep a terminator after newly consumed end slots, even if
            // that terminator lives in the next physical cluster.
            if(end_seen){
              if(in_cluster + pos + FAT32_ENTRY_SIZE < fs->cluster_bytes)
                name->end_marker = candidate + FAT32_ENTRY_SIZE;
              else {
                uint next;
                int more = fat32_next(fs, current, &next);
                if(more < 0)
                  goto done;
                if(more && fat32_cluster_offset(fs, next, &name->end_marker) < 0)
                  goto done;
              }
            }
            result = 0;
            goto done;
          }
        } else
          run = 0;
      }
    }
    uint next;
    int r = fat32_next(fs, current, &next);
    if(r < 0)
      goto done;
    if(r == 0){
      if(fat32_append_cluster(fs, current, &next) < 0)
        goto done;
      if(name->added == 0)
        name->added = next;
    }
    current = next;
  }
done:
  if(sector)
    kfree(sector);
  return result;
}

// Decode only canonical UTF-8. Keep creation and lookup in the same UTF-16
// domain; never store separators, overlong encodings, or lone surrogates.
static int
fat32_plan_name(struct fat32_node *dir, const char *text, uint len,
                 struct fat32_name *name)
{
  uint count = 0;
  for(uint i = 0; i < len;){
    uint cp = (uchar)text[i++], rest = 0, minimum = 0;
    if(cp >= 0x80){
      if(cp >= 0xc2 && cp <= 0xdf){
        rest = 1; minimum = 0x80; cp &= 0x1f;
      } else if(cp >= 0xe0 && cp <= 0xef){
        rest = 2; minimum = 0x800; cp &= 0x0f;
      } else if(cp >= 0xf0 && cp <= 0xf4){
        rest = 3; minimum = 0x10000; cp &= 7;
      } else
        return -1;
      if(i + rest > len)
        return -1;
      while(rest--){
        uint next = (uchar)text[i++];
        if((next & 0xc0) != 0x80)
          return -1;
        cp = (cp << 6) | (next & 0x3f);
      }
      if(cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return -1;
    }
    if(fat32_lfn_forbidden(cp))
      return -1;
    if(count + (cp > 0xffff ? 2 : 1) > 255)
      return -1;
    if(cp > 0xffff){
      cp -= 0x10000;
      name->units[count++] = 0xd800 | (cp >> 10);
      name->units[count++] = 0xdc00 | (cp & 0x3ff);
    } else
      name->units[count++] = cp;
  }
  if(!count || text[len - 1] == '.' || text[len - 1] == ' ')
    return -1;
  name->length = count;
  if(fat32_short_name(text, len, name->alias) == 0)
    return 0;
  name->count = (count + FAT32_LFN_UNITS - 1) / FAT32_LFN_UNITS;
  if(count % FAT32_LFN_UNITS){
    name->units[count++] = 0;
    while(count % FAT32_LFN_UNITS)
      name->units[count++] = 0xffff;
  }

  // Generate a deterministic ASCII alias, shortening its prefix as the
  // decimal suffix grows. Check against both aliases and existing long names.
  uchar base[8], ext[3];
  uint base_len = 0, dot = len;
  for(uint i = 0; i < len; i++)
    if(text[i] == '.')
      dot = i;
  for(uint i = 0; i < dot && base_len < sizeof(base); i++){
    uchar c = text[i];
    if(c == '.' || c == ' ')
      continue;
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    base[base_len++] = ((c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || fat32_allowed_char(c)) ? c : '_';
  }
  if(!base_len)
    base[base_len++] = '_';
  memset(ext, ' ', sizeof(ext));
  for(uint i = dot + 1, j = 0; i < len && j < sizeof(ext); i++, j++){
    uchar c = text[i];
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    ext[j] = ((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || fat32_allowed_char(c)) ? c : '_';
  }
  for(uint serial = 1; serial <= 999999; serial++){
    char digits[6], alias_text[13];
    uint n = 0, value = serial;
    struct fat32_entry existing;
    do {
      digits[n++] = '0' + value % 10;
      value /= 10;
    } while(value);
    uint prefix = 7 - n;
    if(prefix > base_len)
      prefix = base_len;
    memset(name->alias, ' ', sizeof(name->alias));
    memmove(name->alias, base, prefix);
    name->alias[prefix] = '~';
    for(uint j = 0; j < n; j++)
      name->alias[prefix + 1 + j] = digits[n - 1 - j];
    memmove(name->alias + 8, ext, sizeof(ext));
    fat32_format_name(name->alias, alias_text);
    int found = fat32_find_entry(dir, alias_text, strlen(alias_text), &existing);
    if(found == 1)
      return 0;
    if(found < 0)
      return -1;
  }
  return -1;
}

static void
fat32_cancel_name(struct fat32_fs *fs, struct fat32_name *name)
{
  if(!fs->write_failed && name->added){
    if(fat32_write_fat_value(fs, name->old_tail, FAT32_EOC) < 0 ||
       fat32_free_chain(fs, name->added) < 0)
      fs->write_failed = 1;
  }
}

// Return 1 if a directory contains only its required dot entries, 0 if it
// contains another live entry, or -1 on malformed data or I/O failure.
static int
fat32_directory_empty(struct fat32_fs *fs, uint first_cluster,
                      uint parent_cluster)
{
  uchar dot[11], dotdot[11];
  uchar *sector = kalloc();
  struct fat32_chain chain;
  uint current = first_cluster;
  int found_dot = 0, found_dotdot = 0;
  int result = -1;

  if(sector == 0 || current < 2 || current > fs->max_cluster)
    goto done;
  memset(dot, ' ', sizeof(dot));
  memset(dotdot, ' ', sizeof(dotdot));
  dot[0] = '.';
  dotdot[0] = '.';
  dotdot[1] = '.';
  fat32_chain_init(&chain, current);
  for(;;){
    uint64 cluster_off;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      goto done;
    for(uint in_cluster = 0; in_cluster < fs->cluster_bytes;
        in_cluster += fs->bytes_per_sector){
      uint64 sector_off = cluster_off + in_cluster;
      if(fat32_read_bytes(fs, sector_off, sector,
                          fs->bytes_per_sector) < 0)
        goto done;
      for(uint pos = 0; pos < fs->bytes_per_sector;
          pos += FAT32_ENTRY_SIZE){
        uchar *raw = sector + pos;
        if(raw[0] == FAT32_EOD){
          result = found_dot && found_dotdot ? 1 : -1;
          goto done;
        }
        if(raw[0] == FAT32_DELETED || raw[11] == FAT32_ATTR_LFN)
          continue;
        uint entry_cluster = ((uint)fat32_get16(raw + 20) << 16) |
                             fat32_get16(raw + 26);
        entry_cluster &= FAT32_FAT_MASK;
        if(memcmp(raw, dot, sizeof(dot)) == 0){
          if(found_dot || (raw[11] & FAT32_ATTR_DIR) == 0 ||
             entry_cluster != first_cluster)
            goto done;
          found_dot = 1;
          continue;
        }
        if(memcmp(raw, dotdot, sizeof(dotdot)) == 0){
          if(found_dotdot || (raw[11] & FAT32_ATTR_DIR) == 0 ||
             entry_cluster != parent_cluster)
            goto done;
          found_dotdot = 1;
          continue;
        }
        result = 0;
        goto done;
      }
    }
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r < 0)
      goto done;
    if(r == 0){
      result = 1;
      goto done;
    }
    current = next;
  }
done:
  if(sector)
    kfree(sector);
  return result;
}

static int
fat32_store_entry(struct fat32_fs *fs, uint64 offset, uint first_cluster,
                  uint size, const uchar *name, uchar attributes)
{
  uchar raw[FAT32_ENTRY_SIZE];

  if(offset > fs->volume_bytes ||
     (uint64)sizeof(raw) > fs->volume_bytes - offset)
    return -1;
  if(name != 0){
    memset(raw, 0, sizeof(raw));
    memmove(raw, name, 11);
  } else if(fat32_read_bytes(fs, offset, raw, sizeof(raw)) < 0)
    return -1;
  raw[11] = attributes;
  fat32_put16(raw + 20, first_cluster >> 16);
  fat32_put16(raw + 26, first_cluster);
  fat32_put32(raw + 28, size);
  return fat32_write_bytes(fs, offset, raw, sizeof(raw));
}

// Publish the short entry last: incomplete long slots cannot expose a file.
static int
fat32_publish_name(struct fat32_node *dir, struct fat32_name *name,
                    uint cluster, uchar attributes)
{
  struct fat32_fs *fs = dir->fs;
  static const uchar offsets[FAT32_LFN_UNITS] =
    {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  uchar raw[FAT32_ENTRY_SIZE], sum = 0;
  if(fat32_name_slots(dir, name) < 0){
    fat32_cancel_name(fs, name);
    return -1;
  }
  if(name->end_marker){
    memset(raw, 0, sizeof(raw));
    if(fat32_write_bytes(fs, name->end_marker, raw, sizeof(raw)) < 0)
      return -1;
  }
  for(uint i = 0; i < 11; i++)
    sum = ((sum >> 1) | ((sum & 1) << 7)) + name->alias[i];
  for(uint i = 0; i < name->count; i++){
    uint ordinal = name->count - i;
    memset(raw, 0, sizeof(raw));
    raw[0] = ordinal | (i == 0 ? FAT32_LFN_LAST : 0);
    raw[11] = FAT32_ATTR_LFN;
    raw[13] = sum;
    for(uint j = 0; j < FAT32_LFN_UNITS; j++)
      fat32_put16(raw + offsets[j], name->units[(ordinal - 1) * FAT32_LFN_UNITS + j]);
    if(fat32_write_bytes(fs, name->offsets[i], raw, sizeof(raw)) < 0)
      return -1;
  }
  if(fat32_store_entry(fs, name->offsets[name->count], cluster, 0,
                       name->alias, attributes) < 0 || fat32_flush(fs) < 0){
    fs->write_failed = 1;
    return -1;
  }
  return 0;
}

static struct fat32_node *
fat32_node_new(struct fat32_fs *fs, uint64 ino, uint64 entry_offset,
               uint first_cluster, uint size, uchar attributes,
               uint parent_cluster, uint64 parent_ino,
               uint parent_parent_cluster, uint64 parent_parent_ino)
{
  // Regular-file callers hold write_lock from directory lookup through node
  // acquisition. Never revive an unlinked incarnation at the same offset.
  // Directory wrappers retain their path-specific ancestor information.
  if((attributes & FAT32_ATTR_DIR) == 0){
    acquire(&fs->nodes_lock);
    for(struct fat32_node *cached = fs->nodes; cached; cached = cached->next){
      if(!cached->deleted && cached->entry_offset == entry_offset &&
         (cached->attributes & FAT32_ATTR_DIR) == 0){
        cached->refs++;
        release(&fs->nodes_lock);
        return cached;
      }
    }
    release(&fs->nodes_lock);
  }
  struct fat32_node *node = kalloc();
  if(node == 0)
    return 0;
  memset(node, 0, sizeof(*node));
  node->fs = fs;
  node->ino = ino;
  node->entry_offset = entry_offset;
  node->first_cluster = first_cluster;
  node->size = size;
  node->attributes = attributes;
  node->refs = 1;
  node->parent_cluster = parent_cluster;
  node->parent_ino = parent_ino;
  node->parent_parent_cluster = parent_parent_cluster;
  node->parent_parent_ino = parent_parent_ino;
  initsleeplock(&node->lock, "fat32node");
  acquire(&fs->nodes_lock);
  node->next = fs->nodes;
  fs->nodes = node;
  release(&fs->nodes_lock);
  return node;
}

static void
fat32_lock(void *data)
{
  acquiresleep(&((struct fat32_node *)data)->lock);
}

static void
fat32_unlock(void *data)
{
  releasesleep(&((struct fat32_node *)data)->lock);
}

static int
fat32_copy_out(int user_dst, uint64 dst, void *src, uint length)
{
  if(user_dst)
    return copyout(myproc()->pagetable, dst, src, length);
  memmove((void *)dst, src, length);
  return 0;
}

static int
fat32_read_at(void *data, int user_dst, uint64 dst, uint off, uint n)
{
  struct fat32_node *node = data;
  struct fat32_fs *fs = node->fs;
  struct fat32_chain chain;
  uint current = node->first_cluster;
  uint64 position;
  uint total = 0;
  uchar *buf;

  if((node->attributes & FAT32_ATTR_DIR) != 0)
    return -1;
  if(n == 0 || (uint64)off >= node->size)
    return 0;
  if((uint64)n > node->size - off)
    n = node->size - off;
  if(current < 2 || current > fs->max_cluster)
    return -1;
  buf = kalloc();
  if(buf == 0)
    return -1;
  position = off;
  fat32_chain_init(&chain, current);
  for(uint skip = position / fs->cluster_bytes; skip > 0; skip--){
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r != 1){
      kfree(buf);
      return -1;
    }
    current = next;
  }
  while(total < n){
    uint64 cluster_off;
    uint within = position % fs->cluster_bytes;
    uint take = fs->cluster_bytes - within;
    if(take > n - total)
      take = n - total;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      break;
    uint copied = 0;
    while(copied < take){
      uint amount = take - copied;
      if(amount > PGSIZE)
        amount = PGSIZE;
      if(fat32_read_bytes(fs, cluster_off + within + copied,
                          buf, amount) < 0 ||
         fat32_copy_out(user_dst, dst + total + copied, buf, amount) < 0)
        goto finished;
      copied += amount;
    }
    total += take;
    position += take;
    if(total < n){
      uint next;
      int r = fat32_chain_advance(fs, &chain, current, &next);
      if(r != 1)
        break;
      current = next;
    }
  }
finished:
  kfree(buf);
  return total != 0 ? total : (n == 0 ? 0 : -1);
}

static int
fat32_copy_in(int user_src, void *dst, uint64 src, uint length)
{
  if(user_src)
    return copyin(myproc()->pagetable, dst, src, length);
  memmove(dst, (void *)src, length);
  return 0;
}

// Extend a file's cluster chain to at least needed clusters.  The newly
// appended chain is returned through added_first so a caller can roll it back
// before publishing a new directory-entry size/first-cluster pair.
static int
fat32_extend_chain(struct fat32_node *node, uint needed,
                   uint *added_first, uint *old_tail)
{
  struct fat32_fs *fs = node->fs;
  uint tail, count, cluster;

  *added_first = 0;
  *old_tail = 0;
  if(fat32_file_chain(fs, node->first_cluster, node->size, &tail, &count) < 0)
    return -1;
  if(count >= needed)
    return 0;
  *old_tail = tail;
  if(count == 0){
    if(fat32_append_cluster(fs, 0, &cluster) < 0)
      return -1;
    node->first_cluster = cluster;
    *added_first = cluster;
    tail = cluster;
    count = 1;
  }
  while(count < needed){
    if(fat32_append_cluster(fs, tail, &cluster) < 0)
      return -1;
    if(*added_first == 0)
      *added_first = cluster;
    tail = cluster;
    count++;
  }
  return 0;
}

static int
fat32_rollback_extension(struct fat32_node *node, uint added_first,
                         uint old_tail)
{
  struct fat32_fs *fs = node->fs;

  // An I/O error may mean that metadata was already persisted. Preserve all
  // allocations rather than freeing a chain still referenced on disk.
  if(fs->write_failed)
    return -1;
  if(added_first == 0)
    return 0;
  if((old_tail != 0 &&
      fat32_write_fat_value(fs, old_tail, FAT32_EOC) < 0) ||
     fat32_free_chain(fs, added_first) < 0){
    fs->write_failed = 1;
    return -1;
  }
  if(old_tail == 0)
    node->first_cluster = 0;
  return 0;
}

static int
fat32_write_at(void *data, int user_src, uint64 src, uint off, uint n)
{
  struct fat32_node *node = data;
  struct fat32_fs *fs = node->fs;
  struct fat32_chain chain;
  uchar *buf = 0;
  uint old_size, old_first, added_first, old_tail;
  uint64 end;
  uint current, position, total = 0;
  int result = -1, fatal = 0;

  if((node->attributes & FAT32_ATTR_DIR) != 0 || n == 0)
    return n == 0 ? 0 : -1;
  end = (uint64)off + n;
  if(end > 0xffffffffULL)
    return -1;

  acquiresleep(&fs->write_lock);
  acquiresleep(&node->lock);
  old_size = node->size;
  old_first = node->first_cluster;
  added_first = 0;
  old_tail = 0;
  if(fs->write_failed || node->deleted || off > old_size)
    goto done;
  if(fat32_extend_chain(node,
       (uint)((end + fs->cluster_bytes - 1) / fs->cluster_bytes),
       &added_first, &old_tail) < 0){
    if(fat32_rollback_extension(node, added_first, old_tail) == 0)
      node->first_cluster = old_first;
    goto done;
  }
  buf = kalloc();
  if(buf == 0)
    goto rollback;

  current = node->first_cluster;
  position = off;
  fat32_chain_init(&chain, current);
  for(uint skip = position / fs->cluster_bytes; skip > 0; skip--){
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r != 1)
      goto rollback;
    current = next;
  }
  while(total < n){
    uint64 cluster_off;
    uint within = position % fs->cluster_bytes;
    uint take = fs->cluster_bytes - within;
    if(take > PGSIZE)
      take = PGSIZE;
    if(take > n - total)
      take = n - total;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      goto rollback;
    if(fat32_copy_in(user_src, buf, src + total, take) < 0)
      goto rollback;
    // blk_write preserves neighboring bytes in partial sectors.
    if(fat32_write_bytes(fs, cluster_off + within, buf, take) < 0){
      fatal = 1;
      goto rollback;
    }
    total += take;
    position += take;
    if(total < n && within + take == fs->cluster_bytes){
      uint next;
      int r = fat32_chain_advance(fs, &chain, current, &next);
      if(r != 1)
        goto rollback;
      current = next;
    }
  }
  if(fat32_store_entry(fs, node->entry_offset, node->first_cluster,
                       end > old_size ? (uint)end : old_size, 0,
                       node->attributes) < 0){
    fatal = 1;
    goto rollback;
  }
  if(fat32_flush(fs) < 0){
    fatal = 1;
    goto rollback;
  }
  node->size = end > old_size ? (uint)end : old_size;
  result = (int)total;
  goto done;

rollback:
  if(fatal)
    fs->write_failed = 1;
  if(fat32_rollback_extension(node, added_first, old_tail) == 0)
    node->first_cluster = old_first;
done:
  if(buf)
    kfree(buf);
  releasesleep(&node->lock);
  releasesleep(&fs->write_lock);
  return result;
}

static int
fat32_truncate_zero(void *data)
{
  struct fat32_node *node = data;
  struct fat32_fs *fs = node->fs;
  uint first, tail, count;
  int r = -1;

  if((node->attributes & FAT32_ATTR_DIR) != 0)
    return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&node->lock);
  first = node->first_cluster;
  if(fs->write_failed || node->deleted ||
     fat32_file_chain(fs, first, node->size, &tail, &count) < 0)
    goto done;
  if(fat32_store_entry(fs, node->entry_offset, 0, 0, 0,
                       node->attributes) < 0){
    fs->write_failed = 1;
    goto done;
  }
  if(fat32_flush(fs) < 0){
    fs->write_failed = 1;
    goto done;
  }
  node->first_cluster = 0;
  node->size = 0;
  if(first != 0 &&
     (fat32_free_chain(fs, first) < 0 || fat32_flush(fs) < 0)){
    fs->write_failed = 1;
    goto done;
  }
  r = 0;
done:
  releasesleep(&node->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
fat32_create_file(void *data, const char *name, uint len, void **out_node)
{
  struct fat32_node *dir = data, *child = 0;
  struct fat32_fs *fs = dir->fs;
  struct fat32_entry entry;
  struct fat32_name *plan = 0;
  int found;

  *out_node = 0;
  if(len == 0 || len > VFS_NAME_MAX)
    return -1;
  if((dir->attributes & FAT32_ATTR_DIR) == 0)
    return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  if(fs->write_failed || dir->deleted)
    goto done;
  found = fat32_find_entry(dir, name, len, &entry);
  if(found == 0){
    if((entry.attributes & FAT32_ATTR_DIR) != 0)
      goto done;
    child = fat32_node_new(fs, entry.offset / FAT32_ENTRY_SIZE + 2,
                           entry.offset, entry.first_cluster, entry.size,
                           entry.attributes, dir->first_cluster, dir->ino,
                           dir->parent_cluster, dir->parent_ino);
    if(child)
      *out_node = child;
    goto done;
  }
  if(found < 0)
    goto done;
  plan = kalloc();
  if(plan == 0)
    goto done;
  memset(plan, 0, sizeof(*plan));
  if(fat32_plan_name(dir, name, len, plan) < 0 ||
     fat32_publish_name(dir, plan, 0, FAT32_ATTR_FILE) < 0)
    goto done;
  uint64 slot_offset = plan->offsets[plan->count];
  child = fat32_node_new(fs, slot_offset / FAT32_ENTRY_SIZE + 2,
                         slot_offset, 0, 0, FAT32_ATTR_FILE,
                         dir->first_cluster, dir->ino,
                         dir->parent_cluster, dir->parent_ino);
  if(child)
    *out_node = child;
done:
  if(plan)
    kfree(plan);
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return *out_node ? 0 : -1;
}

// Initialize one of the mandatory short-name directory entries in a newly
// allocated directory cluster.  The cluster is zeroed by fat32_alloc_cluster,
// so the remaining fields can stay zero.
static int
fat32_store_dot_entry(struct fat32_fs *fs, uint64 offset, const char *name,
                      uint first_cluster)
{
  uchar short_name[11];

  memset(short_name, ' ', sizeof(short_name));
  short_name[0] = name[0];
  if(name[1] != 0)
    short_name[1] = name[1];
  return fat32_store_entry(fs, offset, first_cluster, 0, short_name,
                           FAT32_ATTR_DIR);
}

static int
fat32_mkdir(void *data, const char *name, uint len)
{
  struct fat32_node *dir = data;
  struct fat32_fs *fs = dir->fs;
  struct fat32_entry entry;
  struct fat32_name *plan = 0;
  uint64 cluster_offset;
  uint child_cluster = 0;
  uint parent_cluster;
  int found, r = -1;

  if(len == 0 || len > VFS_NAME_MAX ||
     (dir->attributes & FAT32_ATTR_DIR) == 0 || dir->deleted)
    return -1;

  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  if(fs->write_failed || dir->deleted)
    goto done;

  found = fat32_find_entry(dir, name, len, &entry);
  if(found != 1)
    goto done;
  plan = kalloc();
  if(plan == 0)
    goto done;
  memset(plan, 0, sizeof(*plan));
  if(fat32_plan_name(dir, name, len, plan) < 0)
    goto done;

  // FAT32 uses cluster 0 in the root directory's ".." entry.  Other
  // directories point to their actual parent cluster.
  parent_cluster = dir->first_cluster == fs->root_cluster ? 0 :
                   dir->first_cluster;

  // Allocate and initialize the child before publishing its parent entry.
  if(fat32_append_cluster(fs, 0, &child_cluster) < 0 ||
     fat32_cluster_offset(fs, child_cluster, &cluster_offset) < 0 ||
     fat32_store_dot_entry(fs, cluster_offset, ".", child_cluster) < 0 ||
     fat32_store_dot_entry(fs, cluster_offset + FAT32_ENTRY_SIZE, "..",
                           parent_cluster) < 0)
    goto rollback;

  if(fat32_publish_name(dir, plan, child_cluster, FAT32_ATTR_DIR) < 0)
    goto rollback;
  r = 0;
  goto done;

rollback:
  if(fs->write_failed)
    goto done;
  if(child_cluster != 0 && fat32_free_chain(fs, child_cluster) < 0)
    fs->write_failed = 1;
done:
  if(plan)
    kfree(plan);
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
fat32_slot_offset(struct fat32_node *dir, uint64 slot, uint64 *out)
{
  struct fat32_fs *fs = dir->fs;
  uint per_cluster = fs->cluster_bytes / FAT32_ENTRY_SIZE;
  uint64 skip = slot / per_cluster;
  uint current = dir->first_cluster;
  if(skip >= fs->cluster_count)
    return -1;
  while(skip--){
    uint next;
    if(fat32_next(fs, current, &next) != 1)
      return -1;
    current = next;
  }
  if(fat32_cluster_offset(fs, current, out) < 0)
    return -1;
  *out += (slot % per_cluster) * FAT32_ENTRY_SIZE;
  return 0;
}

static int
fat32_unlink(void *data, const char *name, uint len)
{
  struct fat32_node *dir = data, *node;
  struct fat32_fs *fs = dir->fs;
  struct fat32_entry entry;
  uchar deleted = FAT32_DELETED;
  uint tail, clusters;
  uint64 entry_offset;
  uint64 offsets[FAT32_LFN_SLOTS + 1];
  uint delete_id;
  int found, empty, marked = 0, r = -1;

  if(len == 0 || len > VFS_NAME_MAX ||
     (dir->attributes & FAT32_ATTR_DIR) == 0)
    return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  if(fs->write_failed || dir->deleted)
    goto done;
  found = fat32_find_entry(dir, name, len, &entry);
  // A corrupt prefix is not safe to associate with the alias for deletion.
  if(found != 0 || entry.offset == 0 ||
     (entry.lfn_prefix && entry.lfn_count == 0) ||
     (entry.attributes & FAT32_ATTR_VOL))
    goto done;
  if(entry.attributes & FAT32_ATTR_DIR){
    if(entry.first_cluster < 2 || entry.first_cluster > fs->max_cluster)
      goto done;
    uint parent_cluster = dir->first_cluster == fs->root_cluster ? 0 :
                          dir->first_cluster;
    empty = fat32_directory_empty(fs, entry.first_cluster, parent_cluster);
    if(empty != 1)
      goto done;
    if(fat32_chain_length(fs, entry.first_cluster, &tail, &clusters) < 0)
      goto done;
  } else if(fat32_file_chain(fs, entry.first_cluster, entry.size,
                             &tail, &clusters) < 0){
    goto done;
  }

  entry_offset = entry.offset;
  for(uint i = 0; i <= entry.lfn_count; i++)
    if(fat32_slot_offset(dir, entry.first_slot + i, &offsets[i]) < 0)
      goto done;
  if(offsets[entry.lfn_count] != entry_offset)
    goto done;
  // Delete the long slots before their alias. Short aliases remain usable
  // if an I/O failure interrupts removal of the prefix.
  for(uint i = 0; i <= entry.lfn_count; i++)
    if(fat32_write_bytes(fs, offsets[i], &deleted, sizeof(deleted)) < 0)
      goto done;
  if(fat32_flush(fs) < 0){
    fs->write_failed = 1;
    goto done;
  }

  // Keep data clusters allocated while any open vnode still refers to this
  // name. The final backend-node release will reclaim them.
  acquire(&fs->nodes_lock);
  delete_id = ++fs->next_delete_id;
  if(delete_id == 0)
    delete_id = ++fs->next_delete_id;
  for(node = fs->nodes; node; node = node->next){
    if(node->entry_offset == entry_offset && !node->deleted){
      node->deleted = 1;
      node->delete_id = delete_id;
      node->deleted_cluster = entry.first_cluster;
      marked++;
    }
  }
  release(&fs->nodes_lock);
  if(marked == 0){
    // vfs_path_unlink normally holds one lookup node through this callback.
    // If that invariant is violated, keep the chain allocated for repair.
    fs->write_failed = 1;
    goto done;
  }
  r = 0;
done:
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
fat32_getattr(void *data, struct stat *st)
{
  struct fat32_node *node = data;
  memset(st, 0, sizeof(*st));
  st->dev = node->fs->dev;
  st->ino = (uint)node->ino;
  st->type = (node->attributes & FAT32_ATTR_DIR) ? T_DIR : T_FILE;
  st->nlink = 1;
  st->size = node->size;
  return 0;
}

static void
fat32_release(void *data)
{
  struct fat32_node *node = data, **link, *other;
  struct fat32_fs *fs = node->fs;
  uint delete_id;
  int last = 0;

  // Serialize final release with unlink, lookup and creation. The VFS calls
  // release both for merged references and when a vnode's last handle closes.
  acquiresleep(&fs->write_lock);
  acquire(&fs->nodes_lock);
  if(node->refs == 0)
    panic("fat32_release refs");
  if(--node->refs != 0){
    release(&fs->nodes_lock);
    releasesleep(&fs->write_lock);
    return;
  }
  delete_id = node->delete_id;
  for(link = &fs->nodes; *link && *link != node; link = &(*link)->next)
    ;
  if(*link == 0)
    panic("fat32_release node");
  *link = node->next;
  if(delete_id){
    last = 1;
    for(other = fs->nodes; other; other = other->next)
      if(other->delete_id == delete_id){
        last = 0;
        break;
      }
  }
  release(&fs->nodes_lock);
  if(delete_id){
    if(last && node->deleted_cluster != 0 && !fs->write_failed &&
       (fat32_free_chain(fs, node->deleted_cluster) < 0 ||
        fat32_flush(fs) < 0))
      fs->write_failed = 1;
  }
  releasesleep(&fs->write_lock);
  kfree(node);
}

static int
fat32_root(void *data, void **out_node)
{
  struct fat32_fs *fs = data;
  struct fat32_node *root = fat32_node_new(fs, 1, 0, fs->root_cluster, 0,
                                            FAT32_ATTR_DIR,
                                            fs->root_cluster, 1,
                                            fs->root_cluster, 1);
  *out_node = root;
  return root ? 0 : -1;
}

static int
fat32_lookup(void *data, const char *name, uint len, void **out_node)
{
  struct fat32_node *dir = data, *node;
  struct fat32_entry entry;
  int r;

  *out_node = 0;
  if((dir->attributes & FAT32_ATTR_DIR) == 0)
    return -1;
  if(len == 1 && name[0] == '.'){
    node = fat32_node_new(dir->fs, dir->ino, dir->entry_offset,
                          dir->first_cluster, dir->size, dir->attributes,
                          dir->parent_cluster, dir->parent_ino,
                          dir->parent_parent_cluster,
                          dir->parent_parent_ino);
    *out_node = node;
    return node ? 0 : -1;
  }
  if(len == 2 && name[0] == '.' && name[1] == '.'){
    node = fat32_node_new(dir->fs, dir->parent_ino, 0,
                          dir->parent_cluster, 0, FAT32_ATTR_DIR,
                          dir->parent_parent_cluster,
                          dir->parent_parent_ino,
                          dir->parent_parent_cluster,
                          dir->parent_parent_ino);
    *out_node = node;
    return node ? 0 : -1;
  }
  // A directory entry snapshot must not race file writes, truncation, unlink
  // or slot reuse before its shared backend reference has been acquired.
  acquiresleep(&dir->fs->write_lock);
  acquiresleep(&dir->lock);
  r = fat32_find_entry(dir, name, len, &entry);
  if(r != 0 || ((entry.attributes & FAT32_ATTR_DIR) &&
                (entry.first_cluster < 2 ||
                 entry.first_cluster > dir->fs->max_cluster))){
    releasesleep(&dir->lock);
    releasesleep(&dir->fs->write_lock);
    return -1;
  }
  uint64 ino = entry.offset / FAT32_ENTRY_SIZE + 2;
  node = fat32_node_new(dir->fs, ino, entry.offset, entry.first_cluster,
                        entry.size, entry.attributes, dir->first_cluster,
                        dir->ino, dir->parent_cluster, dir->parent_ino);
  releasesleep(&dir->lock);
  releasesleep(&dir->fs->write_lock);
  *out_node = node;
  return node ? 0 : -1;
}

static int
fat32_readdir(void *data, uint64 *cursor, struct vfs_dirent *out)
{
  struct fat32_node *dir = data;
  struct fat32_fs *fs = dir->fs;
  struct fat32_chain chain;
  struct fat32_lfn lfn = {0};
  uchar *sector = 0;
  uint current = dir->first_cluster;
  uint64 slot = *cursor;
  uint slots_per_cluster = fs->cluster_bytes / FAT32_ENTRY_SIZE;
  uint64 cluster_index = slot / slots_per_cluster;
  uint slot_in_cluster = slot % slots_per_cluster;
  int result = -1;

  acquiresleep(&dir->lock);
  if((dir->attributes & FAT32_ATTR_DIR) == 0 || current < 2 ||
     current > fs->max_cluster)
    goto done;
  sector = kalloc();
  if(sector == 0)
    goto done;
  fat32_chain_init(&chain, current);
  for(uint64 i = 0; i < cluster_index; i++){
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r < 0)
      goto done;
    if(r == 0){
      result = 0;
      goto done;
    }
    current = next;
  }
  for(;;){
    uint64 cluster_off;
    if(fat32_cluster_offset(fs, current, &cluster_off) < 0)
      goto done;
    uint start_offset = slot_in_cluster * FAT32_ENTRY_SIZE;
    for(uint in_cluster = (start_offset / fs->bytes_per_sector) *
                           fs->bytes_per_sector;
        in_cluster < fs->cluster_bytes; in_cluster += fs->bytes_per_sector){
      uint start = in_cluster == (start_offset / fs->bytes_per_sector) *
                                 fs->bytes_per_sector ?
                   start_offset % fs->bytes_per_sector : 0;
      uint64 sector_off = cluster_off + in_cluster;
      if(fat32_read_bytes(fs, sector_off, sector, fs->bytes_per_sector) < 0)
        goto done;
      for(uint pos = start; pos < fs->bytes_per_sector;
          pos += FAT32_ENTRY_SIZE){
        uchar *raw = sector + pos;
        uint64 this_slot = slot;
        slot++;
        if(raw[0] == FAT32_EOD){
          result = 0;
          goto done;
        }
        if(fat32_lfn_slot(raw)){
          fat32_lfn_push(&lfn, raw);
          continue;
        }
        if(raw[0] == FAT32_DELETED || (raw[11] & FAT32_ATTR_VOL) ||
           raw[0] == '.'){
          lfn.present = lfn.slots = 0;
          continue;
        }
        struct fat32_entry entry;
        fat32_decode_entry(raw, sector_off + pos, &entry);
        memset(out, 0, sizeof(*out));
        out->ino = entry.offset / FAT32_ENTRY_SIZE + 2;
        out->type = (entry.attributes & FAT32_ATTR_DIR) ? T_DIR : T_FILE;
        out->namelen = fat32_lfn_name(&lfn, raw, out->name);
        if(out->namelen == 0){
          out->namelen = strlen(entry.name);
          memmove(out->name, entry.name, out->namelen + 1);
        }
        *cursor = this_slot + 1;
        result = 1;
        goto done;
      }
    }
    slot_in_cluster = 0;
    uint next;
    int r = fat32_chain_advance(fs, &chain, current, &next);
    if(r < 0)
      goto done;
    if(r == 0){
      result = 0;
      goto done;
    }
    current = next;
  }
done:
  if(sector)
    kfree(sector);
  releasesleep(&dir->lock);
  return result;
}

static const struct vnode_ops fat32_node_ops = {
  .lock = fat32_lock,
  .unlock = fat32_unlock,
  .read_at = fat32_read_at,
  .getattr = fat32_getattr,
  .release = fat32_release,
  .write_at = fat32_write_at,
  .truncate_zero = fat32_truncate_zero,
  .lookup = fat32_lookup,
  .create_file = fat32_create_file,
  .mkdir = fat32_mkdir,
  .unlink = fat32_unlink,
  .readdir = fat32_readdir,
};

static const struct vfs_fs_ops fat32_fs_ops = {
  .root = fat32_root,
  .release = kfree,
};

int
fat32_mount(uint dev, const char *mountpoint)
{
  struct fat32_fs *fs = kalloc();
  struct vfs_fs *vfsfs;
  struct vfs_mount *mnt;
  struct vfs_path at = {0};

  if(fs == 0)
    return -1;
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  initlock(&fs->nodes_lock, "fat32nodes");
  if(fat32_load(fs) < 0){
    kfree(fs);
    return -1;
  }
  vfsfs = vfs_fs_create(&fat32_fs_ops, &fat32_node_ops, dev, fs);
  if(vfsfs == 0){
    kfree(fs);
    return -1;
  }
  if(vfs_xv6_lookup_path(mountpoint, &at) < 0){
    if(vfs_xv6_mkdir(mountpoint) < 0 ||
       vfs_xv6_lookup_path(mountpoint, &at) < 0){
      vfs_fs_put(vfsfs);
      return -1;
    }
  }
  mnt = vfs_mount_attach(&at, vfsfs);
  vfs_path_put(&at);
  vfs_fs_put(vfsfs);
  if(mnt == 0)
    return -1;
  return 0;
}
