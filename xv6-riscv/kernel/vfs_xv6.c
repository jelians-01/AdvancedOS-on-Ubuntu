#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
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

static const struct vnode_ops native_ops = {
  .lock = native_lock,
  .unlock = native_unlock,
  .read_at = native_read_at,
  .getattr = native_getattr,
  .release = native_release,
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
