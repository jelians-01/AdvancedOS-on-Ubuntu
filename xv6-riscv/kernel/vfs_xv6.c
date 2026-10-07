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

static void
native_component(char *dst, const char *name, uint len)
{
  // Preserve rev5's first-DIRSIZ-byte matching, only in this backend.
  memset(dst, 0, DIRSIZ);
  if(len > DIRSIZ)
    len = DIRSIZ;
  memmove(dst, name, len);
}

static int
native_lookup(void *data, const char *name, uint len, void **out_node)
{
  struct inode *dp = data, *ip = 0;
  char native_name[DIRSIZ];

  *out_node = 0;
  if(len == 0)
    return -1;
  native_component(native_name, name, len);

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

static int
native_create_node(void *data, const char *name, uint len,
                   short type, short major, short minor, void **out_node)
{
  struct inode *dp = data, *ip;
  char native_name[DIRSIZ];

  *out_node = 0;
  if(len == 0)
    return -1;
  native_component(native_name, name, len);

  begin_op();
  ilock(dp);
  // A removed parent must not gain unreachable children.
  if(dp->type != T_DIR || dp->nlink == 0)
    goto bad_parent;

  if((ip = dirlookup(dp, native_name, 0)) != 0){
    // "." or ".." may alias dp or an ancestor; unlock dp first.
    iunlock(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)){
      iunlock(ip);
      end_op();
      *out_node = ip;
      return 0;
    }
    iunlockput(ip);
    end_op();
    return -1;
  }

  // Keep dp locked from the absence check through publishing the new entry.
  if((ip = ialloc(dp->dev, type)) == 0)
    goto bad_parent;
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);
  if(type == T_DIR){
    // Do not count "." in nlink; count the child's ".." in its parent.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto bad_node;
  }
  if(dirlink(dp, native_name, ip->inum) < 0)
    goto bad_node;
  if(type == T_DIR){
    // Only increment the parent's link count after publication succeeds.
    dp->nlink++;
    iupdate(dp);
  }
  iunlock(ip);
  iunlock(dp);
  end_op();
  *out_node = ip;
  return 0;

bad_node:
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
bad_parent:
  iunlock(dp);
  end_op();
  return -1;
}

static int
native_create_file(void *data, const char *name, uint len, void **out_node)
{
  return native_create_node(data, name, len, T_FILE, 0, 0, out_node);
}

static int
native_mkdir(void *data, const char *name, uint len)
{
  void *node;

  if(native_create_node(data, name, len, T_DIR, 0, 0, &node) < 0)
    return -1;
  native_release(node);
  return 0;
}

static int
native_mknod(void *data, const char *name, uint len, int major, int minor)
{
  void *node;

  // Preserve rev5's short device fields; open validates the stored major.
  if(native_create_node(data, name, len, T_DEVICE, major, minor, &node) < 0)
    return -1;
  native_release(node);
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
  .create_file = native_create_file,
  .mkdir = native_mkdir,
  .mknod = native_mknod,
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

static struct vnode*
native_start(const char *path)
{
  struct inode *ip;
  struct vnode *start;

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
  return start;
}

// Consume the vnode reference and return an independent inode reference.
static struct inode*
native_inode(struct vnode *vn)
{
  struct inode *ip;

  if(vn == 0)
    return 0;
  ip = vfs_node_data(vn, &native_ops);
  if(ip)
    idup(ip);
  vfs_put(vn);
  return ip;
}

struct inode*
vfs_xv6_namei(const char *path)
{
  struct vnode *start, *vn;

  start = native_start(path);
  if(start == 0)
    return 0;
  vn = vfs_walk(start, path);
  vfs_put(start);
  return native_inode(vn);
}

static struct vnode*
native_parent(const char *path, const char **name, uint *len)
{
  struct vnode *start, *parent;

  start = native_start(path);
  if(start == 0)
    return 0;
  parent = vfs_walk_parent(start, path, name, len);
  vfs_put(start);
  return parent;
}

struct inode*
vfs_xv6_create_file(const char *path)
{
  struct vnode *parent, *vn;
  const char *name;
  uint len;

  parent = native_parent(path, &name, &len);
  if(parent == 0)
    return 0;
  vn = vfs_create_file(parent, name, len);
  vfs_put(parent);
  return native_inode(vn);
}

int
vfs_xv6_mkdir(const char *path)
{
  struct vnode *parent;
  const char *name;
  uint len;
  int r;

  parent = native_parent(path, &name, &len);
  if(parent == 0)
    return -1;
  r = vfs_mkdir(parent, name, len);
  vfs_put(parent);
  return r;
}

int
vfs_xv6_mknod(const char *path, int major, int minor)
{
  struct vnode *parent;
  const char *name;
  uint len;
  int r;

  parent = native_parent(path, &name, &len);
  if(parent == 0)
    return -1;
  r = vfs_mknod(parent, name, len, major, minor);
  vfs_put(parent);
  return r;
}
