#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "blkdev.h"
#include "vfs.h"
#include "vfs_dirent.h"
#include "vfs_backend.h"
#include "ext2.h"

#define EXT2_MAGIC 0xef53
#define EXT2_ROOT_INO 2
#define EXT2_SUPER_OFFSET 1024
#define EXT2_SUPER_SIZE 1024
#define EXT2_FEATURE_COMPAT_EXT_ATTR 0x0008
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT2_S_IFMT 0170000
#define EXT2_S_IFREG 0100000
#define EXT2_S_IFDIR 0040000
#define EXT2_S_IFLNK 0120000
#define EXT2_NDIR_BLOCKS 12
#define EXT2_N_BLOCKS 15
#define EXT2_DIRENT_HEADER 8
// Keep positive link counts representable in the shared signed-short stat ABI.
#define EXT2_LINK_MAX 0x7fff

struct ext2_fs {
  uint dev;
  uint block_size;
  uint blocks_count;
  uint inodes_count;
  uint first_data_block;
  uint blocks_per_group;
  uint inodes_per_group;
  uint group_count;
  uint feature_compat;
  uint feature_incompat;
  uint feature_ro_compat;
  ushort inode_size;
  int dirent_has_type;
  uint first_ino;
  int writable;
  int write_failed;
  uint orphans; // Unlinked inodes pinned by open references; keep the volume dirty.
  // Mutations take this before any inode lock; readers take inode locks only.
  struct sleeplock write_lock;
};

struct ext2_inode {
  struct ext2_fs *fs;
  uint inum;
  int refs;
  int valid;
  int orphan;
  struct sleeplock lock;
  ushort mode;
  ushort links;
  uint size_lo;
  uint size_hi;
  uint sectors;
  uint flags;
  uint file_acl;
  uint blocks[EXT2_N_BLOCKS];
};

static struct {
  struct spinlock lock;
  struct ext2_inode nodes[NINODE];
  int initialized;
} ext2_cache;

static const struct vnode_ops ext2_node_ops;
static const struct vfs_fs_ops ext2_fs_ops;
static int ext2_reclaim_inode(struct ext2_inode *ip);

static ushort
get16(const uchar *p)
{
  return (ushort)p[0] | ((ushort)p[1] << 8);
}

static uint
get32(const uchar *p)
{
  return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) |
         ((uint)p[3] << 24);
}

static void
put16(uchar *p, ushort v)
{
  p[0] = v;
  p[1] = v >> 8;
}

static void
put32(uchar *p, uint v)
{
  for(int i = 0; i < 4; i++)
    p[i] = v >> (8 * i);
}

static int
ext2_read_bytes(struct ext2_fs *fs, uint64 off, void *dst, uint n)
{
  uint64 bytes = (uint64)fs->blocks_count * fs->block_size;
  if(off > bytes || (uint64)n > bytes - off)
    return -1;
  return blk_read(fs->dev, off, dst, n);
}

static int
ext2_read_block(struct ext2_fs *fs, uint block, void *dst)
{
  if(block < fs->first_data_block || block >= fs->blocks_count)
    return -1;
  return ext2_read_bytes(fs, (uint64)block * fs->block_size,
                         dst, fs->block_size);
}

static int
ext2_load_super(struct ext2_fs *fs)
{
  uchar *sb;
  uint log_block_size;
  uint compat_supported = EXT2_FEATURE_COMPAT_EXT_ATTR;
  uint incompat_supported = EXT2_FEATURE_INCOMPAT_FILETYPE;
  uint ro_supported = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                      EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
  uint64 device_bytes;

  sb = kalloc();
  if(sb == 0){
    fs->write_failed = 1;
    return -1;
  }

  if(blk_read(fs->dev, EXT2_SUPER_OFFSET, sb, EXT2_SUPER_SIZE) < 0){
    kfree(sb);
    return -1;
  }
  if(get16(sb + 56) != EXT2_MAGIC){
    kfree(sb);
    return -1;
  }
  fs->inodes_count = get32(sb + 0);
  fs->blocks_count = get32(sb + 4);
  fs->first_data_block = get32(sb + 20);
  log_block_size = get32(sb + 24);
  fs->blocks_per_group = get32(sb + 32);
  fs->inodes_per_group = get32(sb + 40);
  fs->inode_size = get16(sb + 88);
  fs->feature_compat = get32(sb + 92);
  fs->feature_incompat = get32(sb + 96);
  fs->feature_ro_compat = get32(sb + 100);
  fs->first_ino = get32(sb + 84);
  if(get32(sb + 76) == 0)
    fs->inode_size = 128;
  if(log_block_size > 2 || fs->inodes_count == 0 || fs->blocks_count == 0 ||
     fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
     fs->inode_size < 128 || fs->inode_size > (1024u << log_block_size))
    goto bad;
  fs->block_size = 1024u << log_block_size;
  fs->group_count = (fs->inodes_count + fs->inodes_per_group - 1) /
                    fs->inodes_per_group;
  // Deliberately narrower than the read path. Unclean/unsupported volumes
  // remain readable, but none of the mutation callbacks may modify them.
  fs->writable = fs->block_size == 1024 && fs->inode_size == 128 &&
    fs->first_data_block == 1 && fs->blocks_per_group <= 8192 &&
    fs->inodes_per_group <= 8192 && fs->first_ino >= 11 &&
    fs->first_ino <= fs->inodes_count && get16(sb + 58) == 1 &&
    fs->feature_compat == 0 && fs->feature_incompat == 0 &&
    fs->feature_ro_compat == 0 &&
    fs->group_count == (fs->blocks_count - 1 + fs->blocks_per_group - 1) /
                       fs->blocks_per_group;
  fs->dirent_has_type =
      (fs->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
  if((fs->feature_compat & ~compat_supported) != 0 ||
     (fs->feature_incompat & ~incompat_supported) != 0 ||
     (fs->feature_ro_compat & ~ro_supported) != 0 ||
     (uint64)fs->group_count * 32 > fs->block_size ||
     (uint64)fs->blocks_count * fs->block_size > 0xffffffffUL ||
     blk_size(fs->dev, &device_bytes) < 0 ||
     (uint64)fs->blocks_count * fs->block_size > device_bytes)
    goto bad;
  kfree(sb);
  return 0;

bad:
  kfree(sb);
  return -1;
}

static struct ext2_inode *
ext2_iget(struct ext2_fs *fs, uint inum)
{
  struct ext2_inode *ip, *empty = 0;

  if(inum == 0 || inum > fs->inodes_count)
    return 0;
  acquire(&ext2_cache.lock);
  for(ip = ext2_cache.nodes; ip < ext2_cache.nodes + NINODE; ip++){
    if(ip->refs > 0 && ip->fs == fs && ip->inum == inum){
      ip->refs++;
      release(&ext2_cache.lock);
      return ip;
    }
    if(ip->refs == 0 && empty == 0)
      empty = ip;
  }
  if(empty){
    empty->fs = fs;
    empty->inum = inum;
    empty->refs = 1;
    empty->valid = 0;
    empty->orphan = 0;
  }
  release(&ext2_cache.lock);
  return empty;
}

static void
ext2_iput(struct ext2_inode *ip)
{
  acquire(&ext2_cache.lock);
  if(ip == 0 || ip->refs < 1)
    panic("ext2_iput");
  ip->refs--;
  if(ip->refs == 0){
    ip->fs = 0;
    ip->inum = 0;
    ip->valid = 0;
  }
  release(&ext2_cache.lock);
}

static int
ext2_read_inode(struct ext2_inode *ip)
{
  struct ext2_fs *fs = ip->fs;
  uchar gd[32];
  uchar *raw;
  uint group = (ip->inum - 1) / fs->inodes_per_group;
  uint index = (ip->inum - 1) % fs->inodes_per_group;
  uint gdt_block = fs->first_data_block + 1;
  uint inode_table;
  uint64 gd_off = (uint64)gdt_block * fs->block_size + (uint64)group * 32;
  uint64 inode_off;

  if(group >= fs->group_count || blk_read(fs->dev, gd_off, gd, sizeof(gd)) < 0)
    return -1;
  inode_table = get32(gd + 8);
  if(inode_table == 0 || inode_table >= fs->blocks_count)
    return -1;
  inode_off = (uint64)inode_table * fs->block_size +
              (uint64)index * fs->inode_size;
  raw = kalloc();
  if(raw == 0)
    return -1;
  if(ext2_read_bytes(fs, inode_off, raw, fs->inode_size) < 0){
    kfree(raw);
    return -1;
  }
  ip->mode = get16(raw + 0);
  ip->links = get16(raw + 26);
  ip->size_lo = get32(raw + 4);
  ip->sectors = get32(raw + 28);
  ip->flags = get32(raw + 32);
  ip->file_acl = get32(raw + 104);
  for(uint i = 0; i < EXT2_N_BLOCKS; i++)
    ip->blocks[i] = get32(raw + 40 + i * 4);
  ip->size_hi = ((ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG &&
                 (fs->feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) ?
                get32(raw + 108) : 0;
  ip->valid = 1;
  kfree(raw);
  return 0;
}

static uint64
ext2_inode_size(const struct ext2_inode *ip)
{
  return ((uint64)ip->size_hi << 32) | ip->size_lo;
}

static int
ext2_indirect(struct ext2_fs *fs, uint block, uint index, uint *out)
{
  uchar *buf;
  if(block == 0){
    *out = 0;
    return 0;
  }
  if(index >= fs->block_size / 4)
    return -1;
  buf = kalloc();
  if(buf == 0)
    return -1;
  if(ext2_read_block(fs, block, buf) < 0){
    kfree(buf);
    return -1;
  }
  *out = get32(buf + index * 4);
  kfree(buf);
  if(*out >= fs->blocks_count && *out != 0)
    return -1;
  return 0;
}

static int
ext2_map_block(struct ext2_inode *ip, uint logical, uint *physical)
{
  struct ext2_fs *fs = ip->fs;
  uint n = fs->block_size / 4;
  uint x, y;

  if(logical < EXT2_NDIR_BLOCKS)
    *physical = ip->blocks[logical];
  else if((logical -= EXT2_NDIR_BLOCKS) < n){
    if(ext2_indirect(fs, ip->blocks[12], logical, physical) < 0)
      return -1;
  } else if((logical -= n) < n * n){
    if(ext2_indirect(fs, ip->blocks[13], logical / n, &x) < 0 ||
       ext2_indirect(fs, x, logical % n, physical) < 0)
      return -1;
  } else if((logical -= n * n) < n * n * n){
    if(ext2_indirect(fs, ip->blocks[14], logical / (n * n), &x) < 0 ||
       ext2_indirect(fs, x, (logical / n) % n, &y) < 0 ||
       ext2_indirect(fs, y, logical % n, physical) < 0)
      return -1;
  } else
    return -1;
  return *physical == 0 || *physical < fs->blocks_count ? 0 : -1;
}

static int
ext2_copy_out(int user_dst, uint64 dst, uchar *src, uint n)
{
  if(user_dst)
    return copyout(myproc()->pagetable, dst, (char *)src, n);
  memmove((void *)dst, src, n);
  return 0;
}

static int
ext2_read_at_locked(struct ext2_inode *ip, int user_dst, uint64 dst,
                    uint off, uint n)
{
  struct ext2_fs *fs = ip->fs;
  uint64 size = ext2_inode_size(ip);
  uint total = 0;
  uchar *buf = kalloc();

  if(buf == 0)
    return -1;
  if((uint64)off >= size || n == 0){
    kfree(buf);
    return 0;
  }
  if((uint64)n > size - off)
    n = (uint)(size - off);
  while(total < n){
    uint pos = off + total;
    uint logical = pos / fs->block_size;
    uint within = pos % fs->block_size;
    uint take = fs->block_size - within;
    uint physical;
    if(take > n - total)
      take = n - total;
    if(ext2_map_block(ip, logical, &physical) < 0)
      break;
    if(physical == 0)
      memset(buf + within, 0, take);
    else if(ext2_read_block(fs, physical, buf) < 0)
      break;
    if(ext2_copy_out(user_dst, dst + total, buf + within, take) < 0)
      break;
    total += take;
  }
  kfree(buf);
  return total ? (int)total : (n == 0 ? 0 : -1);
}

static void
ext2_lock(void *data)
{
  struct ext2_inode *ip = data;
  acquiresleep(&ip->lock);
  // The lock callback has no error return. Keep the lock held and leave
  // valid clear; read/getattr report an ordinary error to VFS instead.
  if(!ip->valid)
    ext2_read_inode(ip);
}

static void
ext2_unlock(void *data)
{
  releasesleep(&((struct ext2_inode *)data)->lock);
}

static int
ext2_read_at(void *data, int user_dst, uint64 dst, uint off, uint n)
{
  struct ext2_inode *ip = data;
  if(!ip->valid)
    return -1;
  return ext2_read_at_locked(ip, user_dst, dst, off, n);
}

static int
ext2_type(ushort mode)
{
  switch(mode & EXT2_S_IFMT){
  case EXT2_S_IFDIR: return T_DIR;
  case EXT2_S_IFREG: return T_FILE;
  default: return 0;
  }
}

static int
ext2_getattr(void *data, struct stat *st)
{
  struct ext2_inode *ip = data;
  int type = ext2_type(ip->mode);
  if(!ip->valid || type == 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->dev = ip->fs->dev;
  st->ino = ip->inum;
  st->type = type;
  st->nlink = ip->links;
  st->size = ext2_inode_size(ip);
  return 0;
}

static void
ext2_release(void *data)
{
  struct ext2_inode *ip = data;
  struct ext2_fs *fs = ip->fs;
  int last;
  acquiresleep(&fs->write_lock);
  acquire(&ext2_cache.lock);
  last = ip->refs == 1;
  release(&ext2_cache.lock);
  if(last && ip->orphan && !fs->write_failed){
    acquiresleep(&ip->lock);
    if(ext2_reclaim_inode(ip) < 0)
      fs->write_failed = 1;
    releasesleep(&ip->lock);
  }
  ext2_iput(ip);
  releasesleep(&fs->write_lock);
}

static int
ext2_root(void *data, void **out_node)
{
  struct ext2_fs *fs = data;
  *out_node = ext2_iget(fs, EXT2_ROOT_INO);
  return *out_node ? 0 : -1;
}

static int
ext2_find_entry(struct ext2_inode *dir, const char *name, uint len,
                uint *inum, uint64 *offset)
{
  struct ext2_fs *fs = dir->fs;
  uint64 size = ext2_inode_size(dir);
  uchar *buf = kalloc();
  int result = -1;

  if(buf == 0 || ext2_type(dir->mode) != T_DIR)
    goto done;
  for(uint64 off = 0; off < size;){
    uint logical = off / fs->block_size;
    uint within = off % fs->block_size;
    uint physical, valid, pos = 0;
    if(ext2_map_block(dir, logical, &physical) < 0 || physical == 0 ||
       ext2_read_block(fs, physical, buf) < 0)
      goto done;
    valid = fs->block_size;
    if(size - off < valid)
      valid = size - off;
    while(pos < valid){
      uint entry_ino;
      ushort rec_len, name_len;
      if(valid - pos < EXT2_DIRENT_HEADER)
        goto done;
      entry_ino = get32(buf + pos);
      rec_len = get16(buf + pos + 4);
      name_len = fs->dirent_has_type ? buf[pos + 6] : get16(buf + pos + 6);
      if(rec_len < EXT2_DIRENT_HEADER || (rec_len & 3) || rec_len > valid - pos ||
         name_len > rec_len - EXT2_DIRENT_HEADER || name_len > VFS_NAME_MAX)
        goto done;
      if(entry_ino && name_len == len &&
         memcmp(buf + pos + EXT2_DIRENT_HEADER, name, len) == 0){
        *inum = entry_ino;
        if(offset)
          *offset = off + pos;
        result = 0;
        goto done;
      }
      pos += rec_len;
    }
    off += valid;
    if(within != 0)
      goto done;
  }
  result = 1;
done:
  if(buf)
    kfree(buf);
  return result;
}

static int
ext2_lookup(void *data, const char *name, uint len, void **out_node)
{
  struct ext2_inode *dir = data, *child;
  uint inum;
  int found;

  *out_node = 0;
  // Pin the inode before unlink can remove its name and recycle its number.
  acquiresleep(&dir->fs->write_lock);
  acquiresleep(&dir->lock);
  if(!dir->valid && ext2_read_inode(dir) < 0){
    releasesleep(&dir->lock);
    releasesleep(&dir->fs->write_lock);
    return -1;
  }
  found = ext2_find_entry(dir, name, len, &inum, 0);
  releasesleep(&dir->lock);
  if(found != 0){
    releasesleep(&dir->fs->write_lock);
    return -1;
  }
  child = ext2_iget(dir->fs, inum);
  releasesleep(&dir->fs->write_lock);
  if(child == 0)
    return -1;
  *out_node = child;
  return 0;
}

static int
ext2_readdir(void *data, uint64 *cursor, struct vfs_dirent *out)
{
  struct ext2_inode *dir = data;
  struct ext2_fs *fs = dir->fs;
  uchar *buf;
  uint64 off = *cursor;
  int result = -1;

  acquiresleep(&dir->lock);
  if(!dir->valid && ext2_read_inode(dir) < 0)
    goto done_unlock;
  if(ext2_type(dir->mode) != T_DIR || off > ext2_inode_size(dir))
    goto done_unlock;
  buf = kalloc();
  if(buf == 0)
    goto done_unlock;
  while(off < ext2_inode_size(dir)){
    uint logical = off / fs->block_size;
    uint within = off % fs->block_size;
    uint physical, valid, pos = 0;
    if(ext2_map_block(dir, logical, &physical) < 0 || physical == 0 ||
       ext2_read_block(fs, physical, buf) < 0)
      goto done_free;
    valid = fs->block_size;
    if(ext2_inode_size(dir) - logical * fs->block_size < valid)
      valid = ext2_inode_size(dir) - logical * fs->block_size;
    if(within >= valid)
      goto done_free;
    pos = within;
    while(pos < valid){
      uint inum;
      ushort rec_len, name_len;
      uchar file_type;
      if(valid - pos < EXT2_DIRENT_HEADER)
        goto done_free;
      inum = get32(buf + pos);
      rec_len = get16(buf + pos + 4);
      name_len = fs->dirent_has_type ? buf[pos + 6] : get16(buf + pos + 6);
      file_type = fs->dirent_has_type ? buf[pos + 7] : 0;
      if(rec_len < EXT2_DIRENT_HEADER || (rec_len & 3) || rec_len > valid - pos ||
         name_len > rec_len - EXT2_DIRENT_HEADER || name_len > VFS_NAME_MAX)
        goto done_free;
      off += rec_len;
      if(inum != 0){
        memset(out, 0, sizeof(*out));
        out->ino = inum;
        out->type = file_type == 2 ? T_DIR : file_type == 1 ? T_FILE :
                    file_type == 3 ? T_DEVICE : VFS_DT_UNKNOWN;
        out->namelen = name_len;
        memmove(out->name, buf + pos + EXT2_DIRENT_HEADER, name_len);
        *cursor = off;
        result = 1;
        goto done_free;
      }
      pos += rec_len;
    }
  }
  result = 0;
done_free:
  kfree(buf);
done_unlock:
  releasesleep(&dir->lock);
  return result;
}

// All mutation helpers run under fs->write_lock. Page buffers also keep
// aligned sector DMA away from kernel stack virtual addresses.
static int
ext2_write_bytes(struct ext2_fs *fs, uint64 off, const void *buf, uint n)
{
  uint64 bytes = (uint64)fs->blocks_count * fs->block_size;
  if(!fs->writable || fs->write_failed || off > bytes || n > bytes - off)
    return -1;
  if(blk_write(fs->dev, off, buf, n) < 0){
    fs->write_failed = 1;
    return -1;
  }
  return 0;
}

static int
ext2_write_block(struct ext2_fs *fs, uint block, uchar *buf)
{
  if(block < fs->first_data_block || block >= fs->blocks_count)
    return -1;
  return ext2_write_bytes(fs, (uint64)block * fs->block_size, buf, fs->block_size);
}

// Dirty before mutation, flush data/metadata before restoring clean state.
// This is a repair marker, not a journal or atomic crash recovery protocol.
static int
ext2_write_state(struct ext2_fs *fs, int clean)
{
  uchar *sb;
  int r = -1;
  if(!fs->writable || fs->write_failed)
    return -1;
  if(blk_flush(fs->dev) < 0){
    fs->write_failed = 1;
    return -1;
  }
  sb = kalloc();
  if(sb == 0){
    fs->write_failed = 1;
    return -1;
  }
  if(ext2_read_bytes(fs, EXT2_SUPER_OFFSET, sb, EXT2_SUPER_SIZE) == 0){
    put16(sb + 58, clean && fs->orphans == 0 ? 1 : 0);
    r = ext2_write_bytes(fs, EXT2_SUPER_OFFSET, sb, EXT2_SUPER_SIZE);
    if(r == 0)
      r = blk_flush(fs->dev);
  }
  if(r < 0)
    fs->write_failed = 1;
  kfree(sb);
  return r;
}

static int
ext2_inode_offset(struct ext2_fs *fs, uint inum, uint64 *off)
{
  uchar gd[32];
  uint group = (inum - 1) / fs->inodes_per_group;
  uint index = (inum - 1) % fs->inodes_per_group;
  if(inum == 0 || inum > fs->inodes_count ||
     ext2_read_bytes(fs, (uint64)(fs->first_data_block + 1) * fs->block_size +
                     group * 32, gd, sizeof(gd)) < 0 ||
     get32(gd + 8) < fs->first_data_block || get32(gd + 8) >= fs->blocks_count)
    return -1;
  *off = (uint64)get32(gd + 8) * fs->block_size + index * fs->inode_size;
  return 0;
}

static int
ext2_store_inode(struct ext2_inode *ip, uchar *raw)
{
  uint64 off;
  if(ext2_inode_offset(ip->fs, ip->inum, &off) < 0 ||
     ext2_read_bytes(ip->fs, off, raw, ip->fs->inode_size) < 0)
    return -1;
  put32(raw + 4, ip->size_lo);
  put16(raw + 26, ip->links);
  put32(raw + 28, ip->sectors);
  for(int i = 0; i < EXT2_N_BLOCKS; i++)
    put32(raw + 40 + 4 * i, ip->blocks[i]);
  put32(raw + 108, 0);
  return ext2_write_bytes(ip->fs, off, raw, ip->fs->inode_size);
}

// Change one bitmap bit and both primary free counters. 1 means exhausted;
// -1 means error. Allocation failure before writes leaves metadata unchanged.
static int
ext2_bitmap(struct ext2_fs *fs, int inode, uint *number, int allocate)
{
  uchar *map = kalloc(), *sb = kalloc();
  uchar gd[32];
  int r = -1;
  uint per = inode ? fs->inodes_per_group : fs->blocks_per_group;
  uint start = allocate ? 0 : (inode ? (*number - 1) / per :
                                 (*number - fs->first_data_block) / per);
  if(map == 0 || sb == 0)
    goto done;
  for(uint group = start; group < fs->group_count; group++){
    uint64 gd_off = (uint64)(fs->first_data_block + 1) * fs->block_size + group * 32;
    uint first = inode ? group * per + 1 : fs->first_data_block + group * per;
    uint limit = inode ? fs->inodes_count + 1 : fs->blocks_count;
    uint count_off = inode ? 14 : 12;
    uint super_off = inode ? 16 : 12;
    uint bitmap, free_group, free_super;
    if(ext2_read_bytes(fs, gd_off, gd, sizeof(gd)) < 0)
      goto done;
    free_group = get16(gd + count_off);
    if(allocate && free_group == 0)
      continue;
    bitmap = get32(gd + (inode ? 4 : 0));
    if(ext2_read_block(fs, bitmap, map) < 0 ||
       ext2_read_bytes(fs, EXT2_SUPER_OFFSET, sb, EXT2_SUPER_SIZE) < 0)
      goto done;
    free_super = get32(sb + super_off);
    if((allocate && free_super == 0) || (!allocate && free_group >= per))
      goto done;
    if(limit > first + per)
      limit = first + per;
    uint begin = allocate ? first : *number;
    if(inode && begin < fs->first_ino)
      begin = fs->first_ino;
    for(uint item = begin; item < limit; item++){
      uint bit = item - first, mask = 1u << (bit % 8);
      if(allocate && (map[bit / 8] & mask))
        continue;
      if(!allocate && !(map[bit / 8] & mask))
        goto done;
      if(allocate)
        map[bit / 8] |= mask;
      else
        map[bit / 8] &= ~mask;
      put16(gd + count_off, free_group + (allocate ? -1 : 1));
      put32(sb + super_off, free_super + (allocate ? -1 : 1));
      if(ext2_write_block(fs, bitmap, map) < 0 ||
         ext2_write_bytes(fs, gd_off, gd, sizeof(gd)) < 0 ||
         ext2_write_bytes(fs, EXT2_SUPER_OFFSET, sb, EXT2_SUPER_SIZE) < 0)
        goto done;
      *number = item;
      r = 0;
      goto done;
    }
    if(!allocate)
      goto done;
  }
  r = allocate ? 1 : -1;
done:
  if(map) kfree(map);
  if(sb) kfree(sb);
  return r;
}

static int
ext2_simple_file(struct ext2_inode *ip)
{
  uint sectors = 0;
  uchar *indirect = 0;
  uint n = ip->fs->block_size / 4;
  if((ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
     ip->flags || ip->file_acl || ip->size_hi ||
     ip->size_lo > (EXT2_NDIR_BLOCKS + n) * ip->fs->block_size ||
     ip->blocks[13] || ip->blocks[14])
    return 0;
  for(int i = 0; i < EXT2_NDIR_BLOCKS; i++){
    if(ip->blocks[i]){
      if(ip->blocks[i] < ip->fs->first_data_block ||
         ip->blocks[i] >= ip->fs->blocks_count)
        return 0;
      for(int j = 0; j < i; j++)
        if(ip->blocks[i] == ip->blocks[j])
          return 0;
      sectors += ip->fs->block_size / 512;
    }
  }
  if(ip->blocks[12]){
    if(ip->blocks[12] < ip->fs->first_data_block ||
       ip->blocks[12] >= ip->fs->blocks_count)
      return 0;
    for(int i = 0; i < EXT2_NDIR_BLOCKS; i++)
      if(ip->blocks[i] == ip->blocks[12])
        return 0;
    indirect = kalloc();
    if(indirect == 0 || ext2_read_block(ip->fs, ip->blocks[12], indirect) < 0){
      if(indirect) kfree(indirect);
      return 0;
    }
    sectors += ip->fs->block_size / 512;
    for(uint i = 0; i < n; i++){
      uint block = get32(indirect + 4 * i);
      if(block == 0)
        continue;
      if(block < ip->fs->first_data_block || block >= ip->fs->blocks_count ||
         block == ip->blocks[12])
        goto bad;
      for(int j = 0; j < EXT2_NDIR_BLOCKS; j++)
        if(block == ip->blocks[j]) goto bad;
      for(uint j = 0; j < i; j++)
        if(block == get32(indirect + 4 * j)) goto bad;
      sectors += ip->fs->block_size / 512;
    }
    kfree(indirect);
  }
  return sectors == ip->sectors;
bad:
  kfree(indirect);
  return 0;
}

static int
ext2_write_at(void *data, int user_src, uint64 src, uint off, uint n)
{
  struct ext2_inode *ip = data;
  struct ext2_fs *fs = ip->fs;
  uchar *buf = 0, *raw = 0;
  uint nindirect = fs->block_size / 4;
  // The write boundary follows the number of data pointers, not including
  // the single indirect metadata block itself.
  uint total = 0, max = (EXT2_NDIR_BLOCKS + nindirect) * fs->block_size;
  uchar *indirect = 0;
  int r = -1, started = 0;

  acquiresleep(&fs->write_lock);
  acquiresleep(&ip->lock);
  if(!fs->writable || fs->write_failed ||
     (!ip->valid && ext2_read_inode(ip) < 0) || ip->links == 0 || !ext2_simple_file(ip) ||
     off > ip->size_lo || off > max || n > max - off)
    goto done;
  if(n == 0){ r = 0; goto done; }
  buf = kalloc(); raw = kalloc();
  if(buf == 0 || raw == 0 || ext2_write_state(fs, 0) < 0)
    goto done;
  started = 1;
  while(total < n){
    uint pos = off + total, logical = pos / fs->block_size;
    uint within = pos % fs->block_size, take = fs->block_size - within;
    uint block, new_table = 0;
    uint indirect_index = 0;
    if(logical < EXT2_NDIR_BLOCKS)
      block = ip->blocks[logical];
    else {
      indirect_index = logical - EXT2_NDIR_BLOCKS;
      if(indirect_index >= nindirect)
        break;
      if(indirect == 0){
        indirect = kalloc();
        if(indirect == 0) break;
        if(ip->blocks[12]){
          if(ext2_read_block(fs, ip->blocks[12], indirect) < 0) break;
        } else {
          memset(indirect, 0, fs->block_size);
        }
      }
      block = get32(indirect + indirect_index * 4);
    }
    int fresh = block == 0;
    if(take > n - total) take = n - total;
    if(fresh)
      memset(buf, 0, fs->block_size);
    else if(ext2_read_block(fs, block, buf) < 0)
      break;
    // Copy before allocation: a bad user address must not leak a data block.
    if(user_src){
      if(copyin(myproc()->pagetable, (char *)buf + within, src + total, take) < 0)
        break;
    } else
      memmove(buf + within, (void *)(src + total), take);
    if(fresh){
      int ar = ext2_bitmap(fs, 0, &block, 1);
      if(ar != 0){
        if(ar < 0) fs->write_failed = 1;
        break;
      }
      if(logical >= EXT2_NDIR_BLOCKS && ip->blocks[12] == 0){
        // Reserve the index block before publishing either new reference.
        ar = ext2_bitmap(fs, 0, &new_table, 1);
        if(ar != 0){
          // Neither reservation has been published in the inode. If only
          // one block remained, release it and leave the file unchanged.
          if(ar < 0 || ext2_bitmap(fs, 0, &block, 0) != 0)
            fs->write_failed = 1;
          break;
        }
      }
    }
    if(ext2_write_block(fs, block, buf) < 0)
      break;
    if(fresh){
      if(logical < EXT2_NDIR_BLOCKS)
        ip->blocks[logical] = block;
      else {
        put32(indirect + indirect_index * 4, block);
        uint table = new_table ? new_table : ip->blocks[12];
        if(ext2_write_block(fs, table, indirect) < 0)
          break;
        if(new_table){
          ip->blocks[12] = new_table;
          ip->sectors += fs->block_size / 512;
        }
      }
      ip->sectors += fs->block_size / 512;
    }
    if(pos + take > ip->size_lo)
      ip->size_lo = pos + take;
    if(ext2_store_inode(ip, raw) < 0){
      fs->write_failed = 1;
      break;
    }
    total += take;
  }
  r = total ? (int)total : -1;
done:
  if(started && ext2_write_state(fs, 1) < 0)
    r = -1;
  if(buf) kfree(buf);
  if(raw) kfree(raw);
  if(indirect) kfree(indirect);
  releasesleep(&ip->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
ext2_truncate_zero(void *data)
{
  struct ext2_inode *ip = data;
  struct ext2_fs *fs = ip->fs;
  uchar *raw, *indirect = 0;
  int r = -1, started = 0;
  acquiresleep(&fs->write_lock);
  acquiresleep(&ip->lock);
  raw = kalloc();
  if(raw == 0 || !fs->writable || fs->write_failed ||
     (!ip->valid && ext2_read_inode(ip) < 0) || ip->links == 0 || !ext2_simple_file(ip) ||
     ext2_write_state(fs, 0) < 0)
    goto done;
  started = 1;
  if(ip->blocks[12]){
    indirect = kalloc();
    if(indirect == 0 || ext2_read_block(fs, ip->blocks[12], indirect) < 0){
      fs->write_failed = 1;
      goto done;
    }
  }
  ip->size_lo = 0;
  for(int i = 0; i < EXT2_NDIR_BLOCKS; i++){
    uint block = ip->blocks[i];
    if(!block) continue;
    ip->blocks[i] = 0;
    ip->sectors -= fs->block_size / 512;
    // Detach before freeing; crashes may leak, but never expose reallocated data.
    if(ext2_store_inode(ip, raw) < 0 || ext2_bitmap(fs, 0, &block, 0) != 0){
      fs->write_failed = 1;
      goto done;
    }
  }
  if(ip->blocks[12]){
    uint table = ip->blocks[12];
    for(uint i = 0; i < fs->block_size / 4; i++){
      uint block = get32(indirect + i * 4);
      if(block == 0) continue;
      put32(indirect + i * 4, 0);
      if(ext2_write_block(fs, table, indirect) < 0){
        fs->write_failed = 1;
        goto done;
      }
      ip->sectors -= fs->block_size / 512;
      if(ext2_store_inode(ip, raw) < 0 || ext2_bitmap(fs, 0, &block, 0) != 0){
        fs->write_failed = 1;
        goto done;
      }
    }
    ip->blocks[12] = 0;
    ip->sectors -= fs->block_size / 512;
    if(ext2_store_inode(ip, raw) < 0 || ext2_bitmap(fs, 0, &table, 0) != 0){
      fs->write_failed = 1;
      goto done;
    }
  }
  r = ext2_store_inode(ip, raw);
  if(r < 0) fs->write_failed = 1;
done:
  if(started && ext2_write_state(fs, 1) < 0) r = -1;
  if(raw) kfree(raw);
  if(indirect) kfree(indirect);
  releasesleep(&ip->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
ext2_simple_dir(struct ext2_inode *dir)
{
  struct ext2_fs *fs = dir->fs;
  if((dir->mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
     dir->flags || dir->file_acl || dir->size_hi || dir->size_lo == 0 ||
     dir->size_lo % fs->block_size ||
     dir->size_lo > EXT2_NDIR_BLOCKS * fs->block_size)
    return 0;
  uint count = dir->size_lo / fs->block_size;
  if(dir->sectors != count * (fs->block_size / 512))
    return 0;
  for(uint i = 0; i < count; i++){
    if(dir->blocks[i] < fs->first_data_block || dir->blocks[i] >= fs->blocks_count)
      return 0;
    for(uint j = 0; j < i; j++)
      if(dir->blocks[i] == dir->blocks[j])
        return 0;
  }
  for(uint i = count; i < EXT2_N_BLOCKS; i++)
    if(dir->blocks[i]) return 0;
  return 1;
}

// Prepare a directory slot entirely in memory before allocating resources.
// Return 1 for an additional direct block, 0 for an existing slot, -1 on error.
static int
ext2_dir_slot(struct ext2_inode *dir, uint len, uchar *buf, uint *block, uint *slot)
{
  struct ext2_fs *fs = dir->fs;
  uint need = (EXT2_DIRENT_HEADER + len + 3) & ~3u;
  if(dir->links < 2 || !ext2_simple_dir(dir))
    return -1;
  uint count = dir->size_lo / fs->block_size;
  for(uint base = 0; base < dir->size_lo; base += fs->block_size){
    if(ext2_map_block(dir, base / fs->block_size, block) < 0 || *block == 0 ||
       ext2_read_block(fs, *block, buf) < 0)
      return -1;
    for(uint pos = 0; pos < fs->block_size;){
      if(fs->block_size - pos < EXT2_DIRENT_HEADER)
        return -1;
      uint ino = get32(buf + pos), rec = get16(buf + pos + 4);
      uint namelen = get16(buf + pos + 6);
      uint used = ino ? (EXT2_DIRENT_HEADER + namelen + 3) & ~3u : 0;
      if(rec < 8 || rec % 4 || rec > fs->block_size - pos ||
         namelen > rec - 8 || namelen > VFS_NAME_MAX)
        return -1;
      if(rec - used >= need){
        if(ino) put16(buf + pos + 4, used);
        *slot = pos + used;
        memset(buf + *slot, 0, rec - used);
        put16(buf + *slot + 4, rec - used);
        return 0;
      }
      pos += rec;
    }
  }
  if(count >= EXT2_NDIR_BLOCKS)
    return -1;
  memset(buf, 0, fs->block_size);
  *block = 0;
  *slot = 0;
  put16(buf + 4, fs->block_size);
  return 1;
}

// Maintain bg_used_dirs_count when allocating or finally reclaiming a directory.
static int
ext2_directory_count(struct ext2_fs *fs, uint inum, int delta)
{
  uchar gd[32];
  uint group = (inum - 1) / fs->inodes_per_group;
  uint64 off = (uint64)(fs->first_data_block + 1) * fs->block_size + group * 32;
  if(ext2_read_bytes(fs, off, gd, sizeof(gd)) < 0 ||
     (delta > 0 && get16(gd + 16) == 0xffff) ||
     (delta < 0 && get16(gd + 16) == 0)){
    fs->write_failed = 1;
    return -1;
  }
  put16(gd + 16, get16(gd + 16) + delta);
  return ext2_write_bytes(fs, off, gd, sizeof(gd));
}

static int
ext2_create_node(void *data, const char *name, uint len, int is_dir,
                   void **out_node)
{
  struct ext2_inode *dir = data, *child = 0;
  struct ext2_fs *fs = dir->fs;
  uchar *buf = 0, *raw = 0, *dots = 0;
  uint inum = 0, block, slot, child_block = 0, parent_block = 0;
  uint64 inode_off;
  int r = -1, started = 0, found, grow, ar;
  *out_node = 0;
  if(len == 0 || len > VFS_NAME_MAX || (len == 1 && name[0] == '.') ||
     (len == 2 && name[0] == '.' && name[1] == '.'))
    return -1;
  for(uint i = 0; i < len; i++)
    if(name[i] == '/' || name[i] == 0) return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  if(!fs->writable || fs->write_failed ||
     (!dir->valid && ext2_read_inode(dir) < 0) || dir->links < 2)
    goto done;
  found = ext2_find_entry(dir, name, len, &inum, 0);
  if(found == 0){
    if(is_dir || inum == dir->inum) goto done;
    child = ext2_iget(fs, inum);
    if(child == 0) goto done;
    acquiresleep(&child->lock);
    if((child->valid || ext2_read_inode(child) == 0) &&
       (child->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
      r = 0;
    releasesleep(&child->lock);
    goto done;
  }
  if(found < 0) goto done;
  if(is_dir && dir->links == 0xffff)
    goto done;
  inum = 0;
  buf = kalloc(); raw = kalloc();
  if(is_dir) dots = kalloc();
  if(buf == 0 || raw == 0 || (is_dir && dots == 0))
    goto done;
  grow = ext2_dir_slot(dir, len, buf, &block, &slot);
  if(grow < 0 || ext2_write_state(fs, 0) < 0)
    goto done;
  started = 1;
  ar = ext2_bitmap(fs, 1, &inum, 1);
  if(ar != 0){
    if(ar < 0) fs->write_failed = 1;
    goto done;
  }
  child = ext2_iget(fs, inum);
  if(child == 0)
    goto rollback;
  if(is_dir){
    ar = ext2_bitmap(fs, 0, &child_block, 1);
    if(ar != 0){
      if(ar < 0) fs->write_failed = 1;
      goto rollback;
    }
  }
  if(grow){
    ar = ext2_bitmap(fs, 0, &parent_block, 1);
    if(ar != 0){
      if(ar < 0) fs->write_failed = 1;
      goto rollback;
    }
    block = parent_block;
  }
  if(is_dir){
    memset(dots, 0, fs->block_size);
    put32(dots, inum);
    put16(dots + 4, 12);
    put16(dots + 6, 1);
    dots[8] = '.';
    put32(dots + 12, dir->inum);
    put16(dots + 16, fs->block_size - 12);
    put16(dots + 18, 2);
    dots[20] = dots[21] = '.';
    if(ext2_write_block(fs, child_block, dots) < 0)
      goto rollback;
  }
  memset(raw, 0, fs->inode_size);
  put16(raw, is_dir ? EXT2_S_IFDIR | 0755 : EXT2_S_IFREG | 0644);
  put16(raw + 26, is_dir ? 2 : 1);
  if(is_dir){
    put32(raw + 4, fs->block_size);
    put32(raw + 28, fs->block_size / 512);
    put32(raw + 40, child_block);
  }
  if(ext2_inode_offset(fs, inum, &inode_off) < 0 ||
     ext2_write_bytes(fs, inode_off, raw, fs->inode_size) < 0){
    fs->write_failed = 1;
    goto done;
  }
  if(is_dir && ext2_directory_count(fs, inum, 1) < 0)
    goto done;
  put32(buf + slot, inum);
  put16(buf + slot + 6, len);
  memmove(buf + slot + EXT2_DIRENT_HEADER, name, len);
  r = ext2_write_block(fs, block, buf);
  if(r == 0 && (grow || is_dir)){
    uint old_size = dir->size_lo, old_sectors = dir->sectors;
    ushort old_links = dir->links;
    uint new_index = old_size / fs->block_size;
    // Publish a fully initialized directory block while holding both locks.
    // Readers cannot see a larger directory until its inode is stored.
    if(grow){
      dir->blocks[new_index] = block;
      dir->size_lo += fs->block_size;
      dir->sectors += fs->block_size / 512;
    }
    if(is_dir)
      dir->links++;
    r = ext2_store_inode(dir, raw);
    if(r < 0){
      // A failed device write leaves the volume dirty, but keep cached state
      // aligned with the last metadata state we know was fully written.
      if(grow)
        dir->blocks[new_index] = 0;
      dir->size_lo = old_size;
      dir->sectors = old_sectors;
      dir->links = old_links;
      fs->write_failed = 1;
    }
  }
  goto done;

rollback:
  // Only unpublished reservations can be reclaimed. Device errors may have
  // partially stored metadata, so leave the volume dirty for offline repair.
  if(!fs->write_failed){
    if((parent_block && ext2_bitmap(fs, 0, &parent_block, 0) != 0) ||
       (child_block && ext2_bitmap(fs, 0, &child_block, 0) != 0) ||
       (inum && ext2_bitmap(fs, 1, &inum, 0) != 0))
      fs->write_failed = 1;
  }
done:
  if(started && ext2_write_state(fs, 1) < 0) r = -1;
  if(r == 0) *out_node = child;
  else if(child) ext2_iput(child);
  if(buf) kfree(buf);
  if(raw) kfree(raw);
  if(dots) kfree(dots);
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
ext2_create_file(void *data, const char *name, uint len, void **out_node)
{
  return ext2_create_node(data, name, len, 0, out_node);
}

static int
ext2_mkdir(void *data, const char *name, uint len)
{
  void *child = 0;
  int r = ext2_create_node(data, name, len, 1, &child);
  if(child)
    ext2_iput(child);
  return r;
}

// Called only at the last backend reference, with write_lock and inode lock.
// Detach and flush the inode before freeing any of its blocks.
static int
ext2_reclaim_inode(struct ext2_inode *ip)
{
  struct ext2_fs *fs = ip->fs;
  uchar *raw = 0, *indirect = 0;
  uint old[EXT2_N_BLOCKS], inum = ip->inum;
  uint64 inode_off;
  int is_dir = (ip->mode & EXT2_S_IFMT) == EXT2_S_IFDIR, r = -1;

  if(!fs->writable || fs->write_failed || !ip->valid || ip->links != 0 ||
     !ip->orphan || fs->orphans == 0 ||
     (is_dir ? !ext2_simple_dir(ip) : !ext2_simple_file(ip)))
    return -1;
  raw = kalloc();
  if(raw == 0)
    goto done;
  memmove(old, ip->blocks, sizeof(old));
  if(old[12]){
    indirect = kalloc();
    if(indirect == 0 || ext2_read_block(fs, old[12], indirect) < 0)
      goto done;
  }
  if(ext2_write_state(fs, 0) < 0)
    goto done;
  memset(ip->blocks, 0, sizeof(ip->blocks));
  ip->size_lo = ip->size_hi = ip->sectors = 0;
  if(ext2_store_inode(ip, raw) < 0 ||
     ext2_inode_offset(fs, inum, &inode_off) < 0)
    goto done;
  // Return a zeroed inode slot. Small synthetic i_dtime values can be
  // interpreted as orphan-chain inode numbers by ext filesystem checkers.
  memset(raw, 0, fs->inode_size);
  if(ext2_write_bytes(fs, inode_off, raw, fs->inode_size) < 0 ||
     blk_flush(fs->dev) < 0)
    goto done;
  for(uint i = 0; i < EXT2_NDIR_BLOCKS; i++)
    if(old[i] && ext2_bitmap(fs, 0, &old[i], 0) != 0)
      goto done;
  if(old[12]){
    for(uint i = 0; i < fs->block_size / 4; i++){
      uint block = get32(indirect + i * 4);
      if(block && ext2_bitmap(fs, 0, &block, 0) != 0)
        goto done;
    }
    if(ext2_bitmap(fs, 0, &old[12], 0) != 0)
      goto done;
  }
  if((is_dir && ext2_directory_count(fs, inum, -1) < 0) ||
     ext2_bitmap(fs, 1, &inum, 0) != 0)
    goto done;
  fs->orphans--;
  ip->orphan = 0;
  r = ext2_write_state(fs, 1);
done:
  if(raw) kfree(raw);
  if(indirect) kfree(indirect);
  return r;
}

// Empty means exactly one valid "." and "..", allowing unused records.
static int
ext2_directory_empty(struct ext2_inode *ip, uint parent)
{
  struct ext2_fs *fs = ip->fs;
  uchar *buf = kalloc();
  int dot = 0, dotdot = 0, r = -1;
  if(buf == 0 || !ext2_simple_dir(ip))
    goto done;
  for(uint off = 0; off < ip->size_lo; off += fs->block_size){
    if(ext2_read_block(fs, ip->blocks[off / fs->block_size], buf) < 0)
      goto done;
    for(uint pos = 0; pos < fs->block_size;){
      if(fs->block_size - pos < EXT2_DIRENT_HEADER)
        goto done;
      uint ino = get32(buf + pos), rec = get16(buf + pos + 4);
      uint len = get16(buf + pos + 6);
      if(rec < 8 || rec % 4 || rec > fs->block_size - pos ||
         len > rec - 8 || len > VFS_NAME_MAX)
        goto done;
      if(ino){
        if(len == 1 && buf[pos + 8] == '.'){
          if(dot++ || ino != ip->inum)
            goto done;
        } else if(len == 2 && buf[pos + 8] == '.' && buf[pos + 9] == '.'){
          if(dotdot++ || ino != parent)
            goto done;
        } else {
          r = 0;
          goto done;
        }
      }
      pos += rec;
    }
  }
  r = dot == 1 && dotdot == 1 ? 1 : -1;
done:
  if(buf) kfree(buf);
  return r;
}

static int
ext2_link(void *data, const char *name, uint len, void *target)
{
  struct ext2_inode *dir = data, *ip = target;
  struct ext2_fs *fs = dir->fs;
  uchar *buf = 0, *raw = 0;
  uint inum, block, slot, new_block = 0;
  int r = -1, started = 0, reserved = 0, grow;

  if(ip->fs != fs || ip == dir || len == 0 || len > VFS_NAME_MAX ||
     (len == 1 && name[0] == '.') ||
     (len == 2 && name[0] == '.' && name[1] == '.'))
    return -1;
  for(uint i = 0; i < len; i++)
    if(name[i] == 0 || name[i] == '/') return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  acquiresleep(&ip->lock);
  if(!fs->writable || fs->write_failed ||
     (!dir->valid && ext2_read_inode(dir) < 0) ||
     (!ip->valid && ext2_read_inode(ip) < 0) ||
     dir->links < 2 || !ext2_simple_dir(dir) ||
     ip->orphan || ip->links == 0 || ip->links >= EXT2_LINK_MAX ||
     !ext2_simple_file(ip) ||
     ext2_find_entry(dir, name, len, &inum, 0) != 1)
    goto done;
  buf = kalloc(); raw = kalloc();
  if(buf == 0 || raw == 0)
    goto done;
  grow = ext2_dir_slot(dir, len, buf, &block, &slot);
  if(grow < 0 || ext2_write_state(fs, 0) < 0)
    goto done;
  started = 1;
  if(grow){
    int ar = ext2_bitmap(fs, 0, &new_block, 1);
    if(ar != 0){
      if(ar < 0) fs->write_failed = 1;
      goto done;
    }
    block = new_block;
  }
  // Reserve the link before exposing its name; no new inode or file data.
  ip->links++;
  reserved = 1;
  if(ext2_store_inode(ip, raw) < 0 || blk_flush(fs->dev) < 0){
    fs->write_failed = 1;
    goto done;
  }
  put32(buf + slot, ip->inum);
  put16(buf + slot + 6, len);
  memmove(buf + slot + EXT2_DIRENT_HEADER, name, len);
  if(ext2_write_block(fs, block, buf) < 0)
    goto done;
  if(grow){
    uint index = dir->size_lo / fs->block_size;
    dir->blocks[index] = block;
    dir->size_lo += fs->block_size;
    dir->sectors += fs->block_size / 512;
    if(ext2_store_inode(dir, raw) < 0){
      dir->blocks[index] = 0;
      dir->size_lo -= fs->block_size;
      dir->sectors -= fs->block_size / 512;
      fs->write_failed = 1;
      goto done;
    }
  }
  r = 0;
done:
  // Only an unpublished reservation with no I/O error can be undone safely.
  if(r < 0 && !fs->write_failed){
    if(reserved){
      ip->links--;
      if(ext2_store_inode(ip, raw) < 0)
        fs->write_failed = 1;
    }
    if(!fs->write_failed && new_block &&
       ext2_bitmap(fs, 0, &new_block, 0) != 0)
      fs->write_failed = 1;
  }
  if(started && ext2_write_state(fs, 1) < 0)
    r = -1;
  if(buf) kfree(buf);
  if(raw) kfree(raw);
  releasesleep(&ip->lock);
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static int
ext2_unlink(void *data, const char *name, uint len)
{
  struct ext2_inode *dir = data, *child = 0;
  struct ext2_fs *fs = dir->fs;
  uchar *buf = 0, *raw = 0;
  uint inum, block, slot;
  uint64 offset;
  int r = -1, started = 0, locked = 0, is_dir;

  if(len == 0 || len > VFS_NAME_MAX ||
     (len == 1 && name[0] == '.') ||
     (len == 2 && name[0] == '.' && name[1] == '.'))
    return -1;
  acquiresleep(&fs->write_lock);
  acquiresleep(&dir->lock);
  if(!fs->writable || fs->write_failed ||
     (!dir->valid && ext2_read_inode(dir) < 0) ||
     dir->links < 2 || !ext2_simple_dir(dir) ||
     ext2_find_entry(dir, name, len, &inum, &offset) != 0 ||
     inum == dir->inum || inum < fs->first_ino)
    goto done;
  child = ext2_iget(fs, inum);
  if(child == 0)
    goto done;
  acquiresleep(&child->lock);
  locked = 1;
  if((!child->valid && ext2_read_inode(child) < 0) ||
     child->links == 0 || child->orphan)
    goto done;
  is_dir = (child->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  if(is_dir){
    if(child->links != 2 || ext2_directory_empty(child, dir->inum) != 1)
      goto done;
  } else if(!ext2_simple_file(child))
    goto done;
  buf = kalloc(); raw = kalloc();
  block = dir->blocks[offset / fs->block_size];
  slot = offset % fs->block_size;
  if(buf == 0 || raw == 0 || ext2_read_block(fs, block, buf) < 0 ||
     get32(buf + slot) != inum || ext2_write_state(fs, 0) < 0)
    goto done;
  started = 1;
  uint rec = get16(buf + slot + 4);
  put32(buf + slot, 0);
  put16(buf + slot + 6, 0);
  memset(buf + slot + 8, 0, rec - 8);
  if(ext2_write_block(fs, block, buf) < 0)
    goto done;
  child->links = is_dir ? 0 : child->links - 1;
  if(child->links == 0){
    child->orphan = 1;
    fs->orphans++;
  }
  if(ext2_store_inode(child, raw) < 0){
    fs->write_failed = 1;
    goto done;
  }
  if(is_dir){
    dir->links--;
    if(ext2_store_inode(dir, raw) < 0){
      fs->write_failed = 1;
      goto done;
    }
  }
  r = 0;
done:
  if(started && ext2_write_state(fs, 1) < 0)
    r = -1;
  if(locked) releasesleep(&child->lock);
  // The VFS unlink bridge pins a lookup node across this callback.
  if(child) ext2_iput(child);
  if(buf) kfree(buf);
  if(raw) kfree(raw);
  releasesleep(&dir->lock);
  releasesleep(&fs->write_lock);
  return r;
}

static const struct vnode_ops ext2_node_ops = {
  .lock = ext2_lock,
  .unlock = ext2_unlock,
  .read_at = ext2_read_at,
  .getattr = ext2_getattr,
  .release = ext2_release,
  .lookup = ext2_lookup,
  .readdir = ext2_readdir,
  .write_at = ext2_write_at,
  .truncate_zero = ext2_truncate_zero,
  .create_file = ext2_create_file,
  .mkdir = ext2_mkdir,
  .unlink = ext2_unlink,
  .link = ext2_link,
};

static int
ext2_sync(void *data)
{
  struct ext2_fs *fs = data;
  acquiresleep(&fs->write_lock);
  int r = fs->write_failed ? -1 : blk_flush(fs->dev);
  if(r < 0)
    fs->write_failed = 1;
  releasesleep(&fs->write_lock);
  return r;
}

static const struct vfs_fs_ops ext2_fs_ops = {
  .root = ext2_root,
  .sync = ext2_sync,
  .release = kfree,
};

int
ext2_mount(uint dev, const char *mountpoint)
{
  struct ext2_fs *fs;
  struct vfs_fs *vfsfs;
  struct vfs_mount *mnt;
  struct vfs_path at = {0};
  uint64 device_bytes;

  if(!ext2_cache.initialized){
    initlock(&ext2_cache.lock, "ext2cache");
    for(struct ext2_inode *ip = ext2_cache.nodes;
        ip < ext2_cache.nodes + NINODE; ip++)
      initsleeplock(&ip->lock, "ext2inode");
    ext2_cache.initialized = 1;
  }
  if(blk_size(dev, &device_bytes) < 0 || device_bytes < EXT2_SUPER_OFFSET + EXT2_SUPER_SIZE)
    return -1;
  fs = kalloc();
  if(fs == 0)
    return -1;
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  initsleeplock(&fs->write_lock, "ext2write");
  if(ext2_load_super(fs) < 0){
    kfree(fs);
    return -1;
  }
  vfsfs = vfs_fs_create(&ext2_fs_ops, &ext2_node_ops, dev, fs);
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
  // The boot mount is permanent; VFS paths retain it for the kernel lifetime.
  return 0;
}
