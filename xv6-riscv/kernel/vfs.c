#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "vfs.h"
#include "vfs_backend.h"

// Backend fields are immutable while ref > 0.
// References and slot allocation are protected by vtable.lock.
struct vfs_fs {
  int ref;
  const struct vfs_fs_ops *ops;
  const struct vnode_ops *node_ops;
  void *data;
};

struct vnode {
  int ref;
  struct vfs_fs *fs;
  void *data;
};

static struct {
  struct spinlock lock;
  struct vnode nodes[NINODE];
  struct vfs_fs filesystems[NVFS];
} vtable;

void
vfs_init(void)
{
  initlock(&vtable.lock, "vtable");
}

struct vfs_fs*
vfs_fs_create(const struct vfs_fs_ops *fs_ops,
              const struct vnode_ops *node_ops, void *data)
{
  struct vfs_fs *fs;

  if(fs_ops == 0 || fs_ops->root == 0 || node_ops == 0 ||
     node_ops->lock == 0 || node_ops->unlock == 0 ||
     node_ops->read_at == 0 || node_ops->getattr == 0 ||
     node_ops->release == 0)
    return 0;
  acquire(&vtable.lock);
  for(fs = vtable.filesystems; fs < vtable.filesystems + NVFS; fs++){
    if(fs->ref == 0){
      fs->ops = fs_ops;
      fs->node_ops = node_ops;
      fs->data = data;
      fs->ref = 1;
      release(&vtable.lock);
      return fs;
    }
  }
  release(&vtable.lock);
  return 0;
}

struct vfs_fs*
vfs_fs_dup(struct vfs_fs *fs)
{
  acquire(&vtable.lock);
  if(fs == 0 || fs->ref < 1)
    panic("vfs_fs_dup");
  fs->ref++;
  release(&vtable.lock);
  return fs;
}

void
vfs_fs_put(struct vfs_fs *fs)
{
  const struct vfs_fs_ops *ops;
  void *data;

  acquire(&vtable.lock);
  if(fs == 0 || fs->ref < 1)
    panic("vfs_fs_put");
  if(--fs->ref > 0){
    release(&vtable.lock);
    return;
  }
  ops = fs->ops;
  data = fs->data;
  fs->ops = 0;
  fs->node_ops = 0;
  fs->data = 0;
  release(&vtable.lock);

  if(ops->release)
    ops->release(data);
}

struct vnode*
vfs_fs_root(struct vfs_fs *fs)
{
  void *data = 0;
  struct vnode *vn;
  struct stat st;
  int r;

  if(fs == 0)
    return 0;
  if(fs->ops->root(fs->data, &data) < 0 || data == 0)
    return 0;
  vn = vfs_node_get(fs, data);
  if(vn == 0){
    fs->node_ops->release(data);
    return 0;
  }
  vfs_lock(vn);
  r = vfs_getattr(vn, &st);
  vfs_unlock(vn);
  if(r < 0 || st.type != T_DIR){
    vfs_put(vn);
    return 0;
  }
  return vn;
}

struct vnode*
vfs_node_get(struct vfs_fs *fs, void *data)
{
  struct vnode *vn, *empty = 0;

  if(fs == 0 || data == 0)
    return 0;
  acquire(&vtable.lock);
  if(fs->ref < 1)
    panic("vfs_node_get fs");
  for(vn = vtable.nodes; vn < vtable.nodes + NINODE; vn++){
    if(vn->ref > 0 && vn->fs == fs && vn->data == data){
      vn->ref++;
      release(&vtable.lock);
      // The existing vnode already owns a backend reference.
      // Release the extra one outside the table's spinlock.
      fs->node_ops->release(data);
      return vn;
    }
    if(vn->ref == 0 && empty == 0)
      empty = vn;
  }
  if(empty){
    fs->ref++;
    empty->fs = fs;
    empty->data = data;
    empty->ref = 1;
  }
  release(&vtable.lock);
  return empty;
}

struct vnode*
vfs_dup(struct vnode *vn)
{
  acquire(&vtable.lock);
  if(vn == 0 || vn->ref < 1)
    panic("vfs_dup");
  vn->ref++;
  release(&vtable.lock);
  return vn;
}

void
vfs_put(struct vnode *vn)
{
  struct vfs_fs *fs;
  void *data;

  acquire(&vtable.lock);
  if(vn == 0 || vn->ref < 1)
    panic("vfs_put");
  if(--vn->ref > 0){
    release(&vtable.lock);
    return;
  }
  fs = vn->fs;
  data = vn->data;
  vn->fs = 0;
  vn->data = 0;
  release(&vtable.lock);

  // The slot may now be reused. Keep the instance alive through release.
  fs->node_ops->release(data);
  vfs_fs_put(fs);
}

void
vfs_lock(struct vnode *vn)
{
  vn->fs->node_ops->lock(vn->data);
}

void
vfs_unlock(struct vnode *vn)
{
  vn->fs->node_ops->unlock(vn->data);
}

int
vfs_read_at(struct vnode *vn, int user_dst, uint64 dst, uint off, uint n)
{
  return vn->fs->node_ops->read_at(vn->data, user_dst, dst, off, n);
}

int
vfs_getattr(struct vnode *vn, struct stat *st)
{
  return vn->fs->node_ops->getattr(vn->data, st);
}

int
vfs_device_major(struct vnode *vn)
{
  if(vn->fs->node_ops->device_major == 0)
    return -1;
  return vn->fs->node_ops->device_major(vn->data);
}

int
vfs_write_at(struct vnode *vn, int user_src, uint64 src, uint off, uint n)
{
  if((user_src != 0 && user_src != 1) ||
     n > 0x7fffffffU || off + n < off)
    return -1;
  if(vn->fs->node_ops->write_at == 0)
    return -1;
  if(n == 0)
    return 0;
  return vn->fs->node_ops->write_at(vn->data, user_src, src, off, n);
}

int
vfs_truncate_zero(struct vnode *vn)
{
  if(vn->fs->node_ops->truncate_zero == 0)
    return -1;
  return vn->fs->node_ops->truncate_zero(vn->data);
}

static int
valid_component(const char *name, uint len)
{
  uint i;

  if(name == 0 || len == 0 || len >= MAXPATH)
    return 0;
  for(i = 0; i < len; i++)
    if(name[i] == '/' || name[i] == 0)
      return 0;
  return 1;
}

struct vnode*
vfs_lookup(struct vnode *dir, const char *name, uint len)
{
  void *data = 0;
  struct vnode *vn;

  if(!valid_component(name, len) || dir->fs->node_ops->lookup == 0)
    return 0;
  if(dir->fs->node_ops->lookup(dir->data, name, len, &data) < 0 || data == 0)
    return 0;
  vn = vfs_node_get(dir->fs, data);
  if(vn == 0)
    dir->fs->node_ops->release(data);
  return vn;
}

struct vnode*
vfs_create_file(struct vnode *dir, const char *name, uint len)
{
  void *data = 0;
  struct vnode *vn;

  if(!valid_component(name, len) || dir->fs->node_ops->create_file == 0)
    return 0;
  if(dir->fs->node_ops->create_file(dir->data, name, len, &data) < 0 || data == 0)
    return 0;
  vn = vfs_node_get(dir->fs, data);
  if(vn == 0)
    dir->fs->node_ops->release(data);
  return vn;
}

int
vfs_mkdir(struct vnode *dir, const char *name, uint len)
{
  if(!valid_component(name, len) || dir->fs->node_ops->mkdir == 0)
    return -1;
  return dir->fs->node_ops->mkdir(dir->data, name, len);
}

int
vfs_mknod(struct vnode *dir, const char *name, uint len, int major, int minor)
{
  if(!valid_component(name, len) || dir->fs->node_ops->mknod == 0)
    return -1;
  return dir->fs->node_ops->mknod(dir->data, name, len, major, minor);
}

int
vfs_link(struct vnode *dir, const char *name, uint len, struct vnode *target)
{
  if(!valid_component(name, len) || dir->fs != target->fs ||
     dir->fs->node_ops->link == 0)
    return -1;
  return dir->fs->node_ops->link(dir->data, name, len, target->data);
}

int
vfs_unlink(struct vnode *dir, const char *name, uint len)
{
  if(!valid_component(name, len) || dir->fs->node_ops->unlink == 0)
    return -1;
  if((len == 1 && name[0] == '.') ||
     (len == 2 && name[0] == '.' && name[1] == '.'))
    return -1;
  return dir->fs->node_ops->unlink(dir->data, name, len);
}

static struct vnode*
vfs_walk_path(struct vnode *start, const char *path, const char **last, uint *lastlen)
{
  struct vnode *vn, *next;
  const char *name;
  uint len;

  if(start == 0 || path == 0)
    return 0;
  for(len = 0; len < MAXPATH && path[len] != 0; len++)
    ;
  if(len == MAXPATH)
    return 0;

  vn = vfs_dup(start);
  for(;;){
    while(*path == '/')
      path++;
    if(*path == 0){
      if(last){
        vfs_put(vn);
        return 0;
      }
      return vn;
    }
    name = path;
    while(*path != 0 && *path != '/')
      path++;
    len = path - name;
    while(*path == '/')
      path++;
    if(last && *path == 0){
      struct stat st;
      int r;

      vfs_lock(vn);
      r = vfs_getattr(vn, &st);
      vfs_unlock(vn);
      if(r < 0 || st.type != T_DIR){
        vfs_put(vn);
        return 0;
      }
      *last = name;
      *lastlen = len;
      return vn;
    }
    // Pass the full component; name limits belong to the backend.
    next = vfs_lookup(vn, name, len);
    vfs_put(vn);
    if(next == 0)
      return 0;
    vn = next;
  }
}

struct vnode*
vfs_walk(struct vnode *start, const char *path)
{
  return vfs_walk_path(start, path, 0, 0);
}

struct vnode*
vfs_walk_parent(struct vnode *start, const char *path,
                const char **name, uint *len)
{
  *name = 0;
  *len = 0;
  return vfs_walk_path(start, path, name, len);
}
