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

struct vfs_mount {
  int ref;
  struct vnode *root; // owns a vnode reference, which retains its instance
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
  struct vfs_mount mounts[NVMOUNT];
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

struct vfs_mount*
vfs_mount_create(struct vfs_fs *fs)
{
  struct vnode *root = vfs_fs_root(fs);
  struct vfs_mount *mnt;

  if(root == 0)
    return 0;
  acquire(&vtable.lock);
  for(mnt = vtable.mounts; mnt < vtable.mounts + NVMOUNT; mnt++){
    if(mnt->ref == 0){
      mnt->root = root;
      mnt->ref = 1;
      release(&vtable.lock);
      return mnt;
    }
  }
  release(&vtable.lock);
  vfs_put(root);
  return 0;
}

struct vfs_mount*
vfs_mount_dup(struct vfs_mount *mnt)
{
  acquire(&vtable.lock);
  if(mnt == 0 || mnt->ref < 1)
    panic("vfs_mount_dup");
  mnt->ref++;
  release(&vtable.lock);
  return mnt;
}

void
vfs_mount_put(struct vfs_mount *mnt)
{
  struct vnode *root;

  acquire(&vtable.lock);
  if(mnt == 0 || mnt->ref < 1)
    panic("vfs_mount_put");
  if(--mnt->ref > 0){
    release(&vtable.lock);
    return;
  }
  root = mnt->root;
  mnt->root = 0;
  release(&vtable.lock);
  vfs_put(root);
}

int
vfs_mount_root(struct vfs_mount *mnt, struct vfs_path *out)
{
  *out = (struct vfs_path){0};
  if(mnt == 0)
    return -1;
  out->mnt = vfs_mount_dup(mnt);
  out->vn = vfs_dup(mnt->root);
  return 0;
}

struct vfs_path
vfs_path_dup(const struct vfs_path *path)
{
  struct vfs_path copy;

  if(path->mnt == 0 || path->vn == 0)
    panic("vfs_path_dup");
  copy.mnt = vfs_mount_dup(path->mnt);
  copy.vn = vfs_dup(path->vn);
  return copy;
}

void
vfs_path_put(struct vfs_path *path)
{
  struct vfs_path old = *path;

  *path = (struct vfs_path){0};
  if(old.vn)
    vfs_put(old.vn);
  if(old.mnt)
    vfs_mount_put(old.mnt);
}

int
vfs_path_lookup(const struct vfs_path *dir, const char *name, uint len,
                struct vfs_path *out)
{
  *out = (struct vfs_path){0};
  if(dir->mnt == 0 || dir->vn == 0)
    return -1;
  out->vn = vfs_lookup(dir->vn, name, len);
  if(out->vn == 0)
    return -1;
  out->mnt = vfs_mount_dup(dir->mnt);
  return 0;
}

int
vfs_path_create(const struct vfs_path *dir, const char *name, uint len,
                struct vfs_path *out)
{
  *out = (struct vfs_path){0};
  if(dir->mnt == 0 || dir->vn == 0)
    return -1;
  out->vn = vfs_create_file(dir->vn, name, len);
  if(out->vn == 0)
    return -1;
  out->mnt = vfs_mount_dup(dir->mnt);
  return 0;
}

static int
vfs_walk_path(const struct vfs_path *start, const char *path,
              struct vfs_path *out, const char **last, uint *lastlen)
{
  struct vfs_path current, next = {0};
  const char *name;
  uint len;

  *out = (struct vfs_path){0};
  if(start == 0 || start->mnt == 0 || start->vn == 0 || path == 0)
    return -1;
  for(len = 0; len < MAXPATH && path[len] != 0; len++)
    ;
  if(len == MAXPATH)
    return -1;

  current = vfs_path_dup(start);
  for(;;){
    while(*path == '/')
      path++;
    if(*path == 0){
      if(last){
        vfs_path_put(&current);
        return -1;
      }
      *out = current; // Transfer both owned references.
      return 0;
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

      vfs_lock(current.vn);
      r = vfs_getattr(current.vn, &st);
      vfs_unlock(current.vn);
      if(r < 0 || st.type != T_DIR){
        vfs_path_put(&current);
        return -1;
      }
      *last = name;
      *lastlen = len;
      *out = current;
      return 0;
    }
    // Every component retains its mount, even when two mounts share a vnode.
    int r = vfs_path_lookup(&current, name, len, &next);
    vfs_path_put(&current);
    if(r < 0)
      return -1;
    current = next;
    next = (struct vfs_path){0};
  }
}

int
vfs_path_walk(const struct vfs_path *start, const char *path,
              struct vfs_path *out)
{
  return vfs_walk_path(start, path, out, 0, 0);
}

int
vfs_path_parent(const struct vfs_path *start, const char *path,
                struct vfs_path *out, const char **name, uint *len)
{
  *name = 0;
  *len = 0;
  return vfs_walk_path(start, path, out, name, len);
}
