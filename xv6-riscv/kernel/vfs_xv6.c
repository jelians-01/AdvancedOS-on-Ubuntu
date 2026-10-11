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
native_link(void *dir, const char *name, uint len, void *target)
{
  struct inode *dp = dir, *ip = target;
  char native_name[DIRSIZ];

  if(len == 0 || dp->dev != ip->dev)
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
  .link = native_link,
  .unlink = native_unlink,
};

// fs.c/log.c still own singleton state. This instance describes ROOTDEV only;
// it does not make a second native filesystem safe.
static uint native_dev = ROOTDEV;
static struct vfs_mount *native_mount;

static int
native_root(void *data, void **out_node)
{
  *out_node = 0;
  if(*(uint *)data != ROOTDEV)
    return -1;
  *out_node = iroot();
  return 0;
}

static const struct vfs_fs_ops native_fs_ops = {
  .root = native_root,
  // Static private data needs no destructor; the root mount retains it.
};

void
vfs_xv6_init(void)
{
  struct vfs_fs *fs;

  if(native_mount != 0)
    panic("vfs_xv6_init twice");
  fs = vfs_fs_create(&native_fs_ops, &native_ops, &native_dev);
  if(fs == 0)
    panic("native instance");
  native_mount = vfs_mount_create(fs);
  vfs_fs_put(fs); // The mount's root now keeps the instance alive.
  if(native_mount == 0)
    panic("native root mount");
}

int
vfs_xv6_root(struct vfs_path *out)
{
  return vfs_mount_root(native_mount, out);
}

static int
native_start(const char *path, struct vfs_path *out)
{
  *out = (struct vfs_path){0};
  if(path == 0)
    return -1;
  if(*path == '/')
    return vfs_xv6_root(out);
  if(myproc()->cwd.vn == 0)
    return -1;
  *out = vfs_path_dup(&myproc()->cwd);
  return 0;
}

int
vfs_xv6_lookup_path(const char *path, struct vfs_path *out)
{
  struct vfs_path start = {0};
  int r;

  *out = (struct vfs_path){0};
  if(native_start(path, &start) < 0)
    return -1;
  r = vfs_path_walk(&start, path, out);
  vfs_path_put(&start);
  return r;
}

static int
native_parent(const char *path, struct vfs_path *out,
              const char **name, uint *len)
{
  struct vfs_path start = {0};
  int r;

  *out = (struct vfs_path){0};
  *name = 0;
  *len = 0;
  if(native_start(path, &start) < 0)
    return -1;
  r = vfs_path_parent(&start, path, out, name, len);
  vfs_path_put(&start);
  return r;
}

int
vfs_xv6_create_file(const char *path, struct vfs_path *out)
{
  struct vfs_path parent = {0};
  const char *name;
  uint len;
  int r;

  *out = (struct vfs_path){0};
  if(native_parent(path, &parent, &name, &len) < 0)
    return -1;
  r = vfs_path_create(&parent, name, len, out);
  vfs_path_put(&parent);
  return r;
}

int
vfs_xv6_mkdir(const char *path)
{
  struct vfs_path parent = {0};
  const char *name;
  uint len;
  int r;

  if(native_parent(path, &parent, &name, &len) < 0)
    return -1;
  r = vfs_mkdir(parent.vn, name, len);
  vfs_path_put(&parent);
  return r;
}

int
vfs_xv6_mknod(const char *path, int major, int minor)
{
  struct vfs_path parent = {0};
  const char *name;
  uint len;
  int r;

  if(native_parent(path, &parent, &name, &len) < 0)
    return -1;
  r = vfs_mknod(parent.vn, name, len, major, minor);
  vfs_path_put(&parent);
  return r;
}

int
vfs_xv6_link(const char *oldpath, const char *newpath)
{
  struct vfs_path target = {0}, parent = {0};
  const char *name;
  uint len;
  int r = -1;

  if(vfs_xv6_lookup_path(oldpath, &target) < 0)
    return -1;
  if(native_parent(newpath, &parent, &name, &len) == 0){
    r = vfs_link(parent.vn, name, len, target.vn);
    vfs_path_put(&parent);
  }
  vfs_path_put(&target);
  return r;
}

int
vfs_xv6_unlink(const char *path)
{
  struct vfs_path parent = {0};
  const char *name;
  uint len;
  int r;

  if(native_parent(path, &parent, &name, &len) < 0)
    return -1;
  r = vfs_unlink(parent.vn, name, len);
  vfs_path_put(&parent);
  return r;
}
