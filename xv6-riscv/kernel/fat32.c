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

#define FAT32_EOC       0x0ffffff8U
#define FAT32_BAD       0x0ffffff7U
#define FAT32_RESERVED  0x0ffffff0U
#define FAT32_MAX_CLUST 0x0fffffeeU
#define FAT32_EOD       0x00
#define FAT32_DELETED   0xe5
#define FAT32_ATTR_LFN  0x0f
#define FAT32_ATTR_VOL  0x08
#define FAT32_ATTR_DIR  0x10
#define FAT32_ATTR_FILE 0x20
#define FAT32_ENTRY_SIZE 32

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
};

struct fat32_entry {
  char name[13];
  uint64 offset;
  uint first_cluster;
  uint size;
  uchar attributes;
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
  return blk_read(fs->dev, offset, dst, length);
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
fat32_next(struct fat32_fs *fs, uint cluster, uint *next)
{
  uchar raw[4];
  uint value;

  if(cluster < 2 || cluster > fs->max_cluster ||
     fat32_read_bytes(fs, fs->fat_offset + (uint64)cluster * 4,
                      raw, sizeof(raw)) < 0)
    return -1;
  value = fat32_get32(raw) & 0x0fffffffU;
  if(fs->mirrored){
    uint64 fat_bytes = (uint64)fs->fat_sectors * fs->bytes_per_sector;
    for(uint i = 1; i < fs->fat_count; i++){
      uint64 offset = fs->fat_offset + (uint64)i * fat_bytes +
                      (uint64)cluster * 4;
      if(fat32_read_bytes(fs, offset, raw, sizeof(raw)) < 0 ||
         (fat32_get32(raw) & 0x0fffffffU) != value)
        return -1;
    }
  }
  if(value >= FAT32_EOC)
    return 0;
  if(value == 0 || value == 1 || value == FAT32_BAD ||
     value >= FAT32_RESERVED || value > fs->max_cluster)
    return -1;
  *next = value;
  return 1;
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
fat32_load(struct fat32_fs *fs)
{
  uchar *boot;
  uint total16, total32, fat16, fat32, root_entries, ext_flags;
  uint64 overhead, fat_bytes, fat_entries, device_bytes;
  uint data_sectors;

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

// Return 0 and fill entry, 1 if absent, or -1 for malformed data/I/O.
static int
fat32_find_entry(struct fat32_node *dir, const uchar wanted[11],
                 struct fat32_entry *entry)
{
  struct fat32_fs *fs = dir->fs;
  uchar *sector = kalloc();
  struct fat32_chain chain;
  uint current = dir->first_cluster;
  int result = -1;

  if(sector == 0 || (dir->attributes & FAT32_ATTR_DIR) == 0)
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
        if(raw[0] == FAT32_EOD){
          result = 1;
          goto done;
        }
        if(raw[0] == FAT32_DELETED || raw[11] == FAT32_ATTR_LFN ||
           (raw[11] & FAT32_ATTR_VOL) || raw[0] == '.')
          continue;
        if(fat32_name_equal(raw, wanted)){
          fat32_decode_entry(raw, sector_off + pos, entry);
          result = 0;
          goto done;
        }
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

static struct fat32_node *
fat32_node_new(struct fat32_fs *fs, uint64 ino, uint64 entry_offset,
               uint first_cluster, uint size, uchar attributes,
               uint parent_cluster, uint64 parent_ino,
               uint parent_parent_cluster, uint64 parent_parent_ino)
{
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
  node->parent_cluster = parent_cluster;
  node->parent_ino = parent_ino;
  node->parent_parent_cluster = parent_parent_cluster;
  node->parent_parent_ino = parent_parent_ino;
  initsleeplock(&node->lock, "fat32node");
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
  kfree(data);
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
  uchar wanted[11];
  int r;

  *out_node = 0;
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
  if(fat32_short_name(name, len, wanted) < 0)
    return -1;
  acquiresleep(&dir->lock);
  r = fat32_find_entry(dir, wanted, &entry);
  releasesleep(&dir->lock);
  if(r != 0 || ((entry.attributes & FAT32_ATTR_DIR) &&
                (entry.first_cluster < 2 ||
                 entry.first_cluster > dir->fs->max_cluster)))
    return -1;
  uint64 ino = entry.offset / FAT32_ENTRY_SIZE + 2;
  node = fat32_node_new(dir->fs, ino, entry.offset, entry.first_cluster,
                        entry.size, entry.attributes, dir->first_cluster,
                        dir->ino, dir->parent_cluster, dir->parent_ino);
  *out_node = node;
  return node ? 0 : -1;
}

static int
fat32_readdir(void *data, uint64 *cursor, struct vfs_dirent *out)
{
  struct fat32_node *dir = data;
  struct fat32_fs *fs = dir->fs;
  struct fat32_chain chain;
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
        if(raw[0] == FAT32_DELETED || raw[11] == FAT32_ATTR_LFN ||
           (raw[11] & FAT32_ATTR_VOL) || raw[0] == '.')
          continue;
        struct fat32_entry entry;
        fat32_decode_entry(raw, sector_off + pos, &entry);
        memset(out, 0, sizeof(*out));
        out->ino = entry.offset / FAT32_ENTRY_SIZE + 2;
        out->type = (entry.attributes & FAT32_ATTR_DIR) ? T_DIR : T_FILE;
        out->namelen = strlen(entry.name);
        memmove(out->name, entry.name, out->namelen);
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
  .lookup = fat32_lookup,
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
