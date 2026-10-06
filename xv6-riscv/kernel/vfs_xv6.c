#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "vfs.h"
#include "vfs_backend.h"

static void
native_lock(void *data)
{
  ilock(data);
}

static void
native_unlock(void *data)
{
  iunlock(data);
}

static int
native_read_at(void *data, int user_dst, uint64 dst, uint off, uint n)
{
  struct inode *ip = data;

  // Device I/O continues to use devsw, not disk inode reads.
  if(ip->type != T_FILE && ip->type != T_DIR)
    return -1;
  return readi(ip, user_dst, dst, off, n);
}

static int
native_getattr(void *data, struct stat *st)
{
  stati(data, st);
  return 0;
}

static void
native_release(void *data)
{
  begin_op();
  iput(data);
  end_op();
}

static int
native_write_at(void *data, int user_src, uint64 src, uint off, uint n)
{
  struct inode *ip = data;
  uint done = 0;
  uint max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;

  // Keep each chunk within the native log's capacity.
  while(done < n){
    uint count = n - done;
    int r;

    if(count > max)
      count = max;
    begin_op();
    ilock(ip);
    if(ip->type == T_FILE)
      r = writei(ip, user_src, src + done, off + done, count);
    else
      r = -1;
    iunlock(ip);
    end_op();

    if(r > 0)
      done += r;
    if(r != (int)count)
      return done ? (int)done : -1;
  }
  return (int)done;
}

static int
native_truncate_zero(void *data)
{
  struct inode *ip = data;

  begin_op();
  ilock(ip);
  if(ip->type != T_FILE){
    iunlock(ip);
    end_op();
    return -1;
  }
  itrunc(ip);
  iunlock(ip);
  end_op();
  return 0;
}

static int
native_lookup(void *data, const char *name, uint len, void **out_node)
{
  struct inode *dp = data, *ip = 0;
  char native_name[DIRSIZ];

  *out_node = 0;
  if(len == 0)
    return -1;
  // Preserve rev5's first-DIRSIZ-byte matching, only in this backend.
  memset(native_name, 0, sizeof(native_name));
  if(len > DIRSIZ)
    len = DIRSIZ;
  memmove(native_name, name, len);

  // Lookup only reads and acquires a reference; no iput or log writes.
  // Never lock the child here: "." can refer to dp itself.
  ilock(dp);
  if(dp->type == T_DIR)
    ip = dirlookup(dp, native_name, 0);
  iunlock(dp);
  if(ip == 0)
    return -1;
  *out_node = ip;
  return 0;
}

static const struct vnode_ops native_ops = {
  .lock = native_lock,
  .unlock = native_unlock,
  .read_at = native_read_at,
  .getattr = native_getattr,
  .release = native_release,
  .write_at = native_write_at,
  .truncate_zero = native_truncate_zero,
  .lookup = native_lookup,
};

struct vnode*
vfs_xv6_get(struct inode *ip)
{
  struct vnode *vn;

  if(ip == 0)
    return 0;

  // Borrow the caller's inode and acquire our own backend reference.
  idup(ip);
  vn = vfs_node_get(&native_ops, ip);
  if(vn == 0)
    native_release(ip);
  return vn;
}

struct inode*
vfs_xv6_namei(const char *path)
{
  struct inode *ip;
  struct vnode *start, *vn;

  if(path == 0)
    return 0;
  if(*path == '/'){
    // Bootstrap only the native root; VFS walks the actual input path.
    begin_op();
    ip = namei("/");
    end_op();
  } else {
    ip = idup(myproc()->cwd);
  }
  if(ip == 0)
    return 0;
  start = vfs_xv6_get(ip);
  native_release(ip);
  if(start == 0)
    return 0;

  vn = vfs_walk(start, path);
  vfs_put(start);
  if(vn == 0)
    return 0;
  ip = vfs_node_data(vn, &native_ops);
  if(ip)
    idup(ip);
  vfs_put(vn);
  return ip;
}
