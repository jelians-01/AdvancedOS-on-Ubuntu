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

static int
native_device_major(void *data)
{
  struct inode *ip = data;

  return ip->type == T_DEVICE ? ip->major : -1;
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

static int
native_same_fs(void *dir, void *target)
{
  struct inode *dp = dir, *ip = target;

  // The native backend has one filesystem identity per device, no mounts yet.
  return dp->dev == ip->dev;
}

static int
native_link(void *dir, const char *name, uint len, void *target)
{
  struct inode *dp = dir, *ip = target;
  char native_name[DIRSIZ];

  if(len == 0 || !native_same_fs(dir, target))
    return -1;
  native_component(native_name, name, len);

  begin_op();
  // Reserve the link before taking the parent lock. Never hold the target
  // lock while waiting for dp: unlink locks parent then child.
  ilock(ip);
  if(ip->type == T_DIR || ip->nlink < 1 || ip->nlink == 32767){
    iunlock(ip);
    end_op();
    return -1;
  }
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  ilock(dp);
  if(dp->type != T_DIR || dp->nlink == 0 ||
     dirlink(dp, native_name, ip->inum) < 0){
    iunlock(dp);
    // Roll back within this transaction, without holding the parent lock.
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlock(ip);
    end_op();
    return -1;
  }
  iunlock(dp);
  end_op();
  return 0;
}

// Caller holds the directory lock; preserve the native directory layout.
static int
native_isdirempty(struct inode *dp)
{
  uint off;
  struct dirent de;

  for(off = 2*sizeof(de); off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("native_isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

static int
native_unlink(void *dir, const char *name, uint len)
{
  struct inode *dp = dir, *ip;
  struct dirent de;
  char native_name[DIRSIZ];
  uint off;

  if(len == 0)
    return -1;
  native_component(native_name, name, len);
  // Reject aliases before locking a child; "." would lock dp twice.
  if(namecmp(native_name, ".") == 0 || namecmp(native_name, "..") == 0)
    return -1;

  begin_op();
  ilock(dp);
  if(dp->type != T_DIR || dp->nlink == 0)
    goto bad_parent;
  if((ip = dirlookup(dp, native_name, &off)) == 0)
    goto bad_parent;
  // Native tree order: parent before child. Directory hard links are forbidden.
  ilock(ip);
  if(ip->nlink < 1)
    panic("native_unlink: nlink");
  if(ip->type == T_DIR && !native_isdirempty(ip)){
    iunlockput(ip);
    goto bad_parent;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("native_unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlock(dp);
  ip->nlink--;
  iupdate(ip);
  // Only the lookup's reference is consumed. Open files/cwd retain theirs.
  iunlockput(ip);
  end_op();
  return 0;

bad_parent:
  iunlock(dp);
  end_op();
  return -1;
}

static const struct vnode_ops native_ops = {
  .lock = native_lock,
  .unlock = native_unlock,
  .read_at = native_read_at,
  .getattr = native_getattr,
  .device_major = native_device_major,
  .release = native_release,
  .write_at = native_write_at,
  .truncate_zero = native_truncate_zero,
  .lookup = native_lookup,
  .create_file = native_create_file,
  .mkdir = native_mkdir,
  .mknod = native_mknod,
  .same_fs = native_same_fs,
  .link = native_link,
  .unlink = native_unlink,
};

struct vnode*
vfs_xv6_root(void)
{
  struct inode *ip = iroot();
  struct vnode *vn;

  // Hand our owned inode reference directly to the vnode table.
  vn = vfs_node_get(&native_ops, ip);
  if(vn == 0)
    native_release(ip);
  return vn;
}

static struct vnode*
native_start(const char *path)
{
  if(path == 0)
    return 0;
  if(*path == '/')
    return vfs_xv6_root();
  if(myproc()->cwd == 0)
    return 0;
  return vfs_dup(myproc()->cwd);
}

struct vnode*
vfs_xv6_lookup_path(const char *path)
{
  struct vnode *start, *vn;

  start = native_start(path);
  if(start == 0)
    return 0;
  vn = vfs_walk(start, path);
  vfs_put(start);
  return vn;
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

struct vnode*
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
  return vn;
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

int
vfs_xv6_link(const char *oldpath, const char *newpath)
{
  struct vnode *start, *target, *parent;
  const char *name;
  uint len;
  int r = -1;

  start = native_start(oldpath);
  if(start == 0)
    return -1;
  target = vfs_walk(start, oldpath);
  vfs_put(start);
  if(target == 0)
    return -1;
  parent = native_parent(newpath, &name, &len);
  if(parent){
    r = vfs_link(parent, name, len, target);
    vfs_put(parent);
  }
  vfs_put(target);
  return r;
}

int
vfs_xv6_unlink(const char *path)
{
  struct vnode *parent;
  const char *name;
  uint len;
  int r;

  parent = native_parent(path, &name, &len);
  if(parent == 0)
    return -1;
  r = vfs_unlink(parent, name, len);
  vfs_put(parent);
  return r;
}
