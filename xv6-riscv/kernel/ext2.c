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
};

struct ext2_inode {
  struct ext2_fs *fs;
  uint inum;
  int refs;
  int valid;
  struct sleeplock lock;
  ushort mode;
  ushort links;
  uint size_lo;
  uint size_hi;
  uint sectors;
  uint blocks[EXT2_N_BLOCKS];
};

static struct {
  struct spinlock lock;
  struct ext2_inode nodes[NINODE];
  int initialized;
} ext2_cache;

static const struct vnode_ops ext2_node_ops;
static const struct vfs_fs_ops ext2_fs_ops;

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
  uchar sb[EXT2_SUPER_SIZE];
  uint log_block_size;
  uint compat_supported = EXT2_FEATURE_COMPAT_EXT_ATTR;
  uint incompat_supported = EXT2_FEATURE_INCOMPAT_FILETYPE;
  uint ro_supported = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                      EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
  uint64 device_bytes;

  if(blk_read(fs->dev, EXT2_SUPER_OFFSET, sb, sizeof(sb)) < 0 ||
     get16(sb + 56) != EXT2_MAGIC)
    return -1;
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
  if(get32(sb + 76) == 0)
    fs->inode_size = 128;
  if(log_block_size > 2 || fs->inodes_count == 0 || fs->blocks_count == 0 ||
     fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
     fs->inode_size < 128 || fs->inode_size > (1024u << log_block_size))
    return -1;
  fs->block_size = 1024u << log_block_size;
  fs->group_count = (fs->inodes_count + fs->inodes_per_group - 1) /
                    fs->inodes_per_group;
  fs->dirent_has_type =
      (fs->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
  if((fs->feature_compat & ~compat_supported) != 0 ||
     (fs->feature_incompat & ~incompat_supported) != 0 ||
     (fs->feature_ro_compat & ~ro_supported) != 0 ||
     (uint64)fs->group_count * 32 > fs->block_size ||
     (uint64)fs->blocks_count * fs->block_size > 0xffffffffUL ||
     blk_size(fs->dev, &device_bytes) < 0 ||
     (uint64)fs->blocks_count * fs->block_size > device_bytes)
    return -1;
  return 0;
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
  if(!ip->valid && ext2_read_inode(ip) < 0){
    releasesleep(&ip->lock);
    panic("ext2 inode read");
  }
}

static void
ext2_unlock(void *data)
{
  releasesleep(&((struct ext2_inode *)data)->lock);
}

static int
ext2_read_at(void *data, int user_dst, uint64 dst, uint off, uint n)
{
  return ext2_read_at_locked(data, user_dst, dst, off, n);
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
  if(type == 0)
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
  ext2_iput(data);
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
                uint *inum, uchar *type)
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
        if(type)
          *type = fs->dirent_has_type ? buf[pos + 7] : 0;
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
  acquiresleep(&dir->lock);
  if(!dir->valid && ext2_read_inode(dir) < 0){
    releasesleep(&dir->lock);
    return -1;
  }
  found = ext2_find_entry(dir, name, len, &inum, 0);
  releasesleep(&dir->lock);
  if(found != 0)
    return -1;
  child = ext2_iget(dir->fs, inum);
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
    if(ext2_inode_size(dir) - off < valid)
      valid = ext2_inode_size(dir) - off;
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

static const struct vnode_ops ext2_node_ops = {
  .lock = ext2_lock,
  .unlock = ext2_unlock,
  .read_at = ext2_read_at,
  .getattr = ext2_getattr,
  .release = ext2_release,
  .lookup = ext2_lookup,
  .readdir = ext2_readdir,
};

static const struct vfs_fs_ops ext2_fs_ops = {
  .root = ext2_root,
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
