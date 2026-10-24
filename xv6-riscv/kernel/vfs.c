#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "vfs.h"
#include "vfs_dirent.h"
#include "vfs_backend.h"

// Backend fields are immutable while ref > 0.
// References and slot allocation are protected by vtable.lock.
struct vfs_fs {
  int ref;
  const struct vfs_fs_ops *ops;
  const struct vnode_ops *node_ops;
  uint dev;
  void *data;
};

struct vfs_mount {
  int ref;
  struct vnode *root; // owns a vnode reference, which retains its instance
  struct vfs_path covered; // owns the parent context and covered directory
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

// Order: namespace lock -> backend transaction/node locks. No backend may
// acquire this lock. vtable.lock is never held across backend calls.
static struct sleeplock namespace_lock;

void
vfs_init(void)
{
  initlock(&vtable.lock, "vtable");
  initsleeplock(&namespace_lock, "vfsnamespace");
}

struct vfs_fs*
vfs_fs_create(const struct vfs_fs_ops *fs_ops,
              const struct vnode_ops *node_ops, uint dev, void *data)
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
      fs->dev = dev;
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

// Pin each instance while dropping the table lock for sleeping callbacks.
// All callbacks run even after one fails, so another volume can still flush.
int
vfs_sync(void)
{
  struct vfs_fs *snapshot[NVFS];
  int count = 0, result = 0;
  acquire(&vtable.lock);
  for(int i = 0; i < NVFS; i++){
    struct vfs_fs *fs = &vtable.filesystems[i];
    if(fs->ref > 0){
      fs->ref++;
      snapshot[count++] = fs;
    }
  }
  release(&vtable.lock);
  for(int i = 0; i < count; i++){
    struct vfs_fs *fs = snapshot[i];
    if(fs->ops->sync == 0 || fs->ops->sync(fs->data) < 0)
      result = -1;
    vfs_fs_put(fs);
  }
  return result;
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

int
vfs_readdir(struct vnode *dir, uint64 *cursor, struct vfs_dirent *out)
{
  struct vfs_dirent entry;
  uint64 next;
  int r;

  if(dir == 0 || cursor == 0 || out == 0 ||
     dir->fs->node_ops->readdir == 0)
    return -1;
  next = *cursor;
  memset(&entry, 0, sizeof(entry));
  r = dir->fs->node_ops->readdir(dir->data, &next, &entry);
  if(r == 0)
    return 0;
  if(r != 1 || next == *cursor || entry.namelen == 0 ||
     entry.namelen > VFS_NAME_MAX || entry.name[entry.namelen] != 0 ||
     (entry.type != VFS_DT_UNKNOWN && entry.type != T_DIR &&
      entry.type != T_FILE && entry.type != T_DEVICE))
    return -1;
  for(uint i = 0; i < entry.namelen; i++)
    if(entry.name[i] == 0 || entry.name[i] == '/')
      return -1;
  entry.reserved = 0;
  memset(entry.name + entry.namelen, 0, sizeof(entry.name) - entry.namelen);
  *out = entry;
  *cursor = next;
  return 1;
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
      mnt->covered = (struct vfs_path){0};
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
  struct vfs_path covered;

  acquire(&vtable.lock);
  if(mnt == 0 || mnt->ref < 1)
    panic("vfs_mount_put");
  if(--mnt->ref > 0){
    release(&vtable.lock);
    return;
  }
  root = mnt->root;
  covered = mnt->covered;
  mnt->root = 0;
  mnt->covered = (struct vfs_path){0};
  release(&vtable.lock);
  vfs_put(root);
  vfs_path_put(&covered);
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

// Return 1 if ancestor contains node in this backend tree, 0 if not,
// or -1 for malformed/deeper-than-MAXPATH ancestry. Caller holds namespace_lock.
static int
mount_ancestor(struct vnode *root, struct vnode *ancestor, struct vnode *node)
{
  struct vnode *cur = vfs_dup(node), *next;
  int result = -1;

  for(int depth = 0; depth < MAXPATH; depth++){
    if(cur == ancestor){
      result = 1;
      break;
    }
    if(cur == root){
      result = 0;
      break;
    }
    next = vfs_lookup(cur, "..", 2);
    if(next == 0)
      break;
    if(next == cur){
      vfs_put(next);
      break;
    }
    vfs_put(cur);
    cur = next;
  }
  vfs_put(cur);
  return result;
}

struct vfs_mount*
vfs_mount_attach(const struct vfs_path *at, struct vfs_fs *fs)
{
  struct vfs_mount *mnt, *other;
  struct vfs_path existing[NVMOUNT];
  struct stat st;
  int count = 0, bad = 0, r;

  if(at == 0 || at->mnt == 0 || at->vn == 0 || fs == 0 ||
     at->vn->fs != at->mnt->root->fs)
    return 0;
  mnt = vfs_mount_create(fs);
  if(mnt == 0)
    return 0;
  acquiresleep(&namespace_lock);
  // Only a root context can receive a mount. No root overlay or nesting.
  if(at->mnt->covered.vn || at->vn == at->mnt->root)
    bad = 1;
  vfs_lock(at->vn);
  r = vfs_getattr(at->vn, &st);
  vfs_unlock(at->vn);
  if(r < 0 || st.type != T_DIR || st.nlink < 1)
    bad = 1;

  acquire(&vtable.lock);
  for(other = vtable.mounts; other < vtable.mounts + NVMOUNT; other++){
    if(other->ref == 0 || other == mnt)
      continue;
    // Conservative first version: all device sharing is rejected, including
    // different instances describing the same device and detached contexts.
    if(other->root->fs->dev == fs->dev)
      bad = 1;
    if(other->covered.vn && other->covered.mnt == at->mnt){
      existing[count] = other->covered;
      existing[count].mnt->ref++;
      existing[count].vn->ref++;
      count++;
    }
  }
  release(&vtable.lock);

  // Also reject mounting an ancestor over an already attached descendant.
  // This check handles stale/raw paths into a subtree already mounted over.
  for(int i = 0; i < count; i++){
    if(!bad &&
       (mount_ancestor(at->mnt->root, at->vn, existing[i].vn) != 0 ||
        mount_ancestor(at->mnt->root, existing[i].vn, at->vn) != 0))
      bad = 1;
    vfs_path_put(&existing[i]);
  }
  if(!bad){
    struct vfs_path covered = vfs_path_dup(at);
    acquire(&vtable.lock);
    mnt->covered = covered; // Publish immutable attachment metadata.
    release(&vtable.lock);
  }
  releasesleep(&namespace_lock);
  if(bad){
    vfs_mount_put(mnt);
    return 0;
  }
  return mnt;
}

// Replace a backend child with the mounted root, if any. Snapshot owned refs
// under the table lock, then release the covered path outside it.
static void
vfs_follow_mount(struct vfs_path *path)
{
  struct vfs_mount *mnt;
  struct vfs_path next = {0};

  acquire(&vtable.lock);
  for(mnt = vtable.mounts; mnt < vtable.mounts + NVMOUNT; mnt++){
    if(mnt->ref > 0 && mnt->covered.mnt == path->mnt &&
       mnt->covered.vn == path->vn){
      mnt->ref++;
      mnt->root->ref++;
      next.mnt = mnt;
      next.vn = mnt->root;
      break;
    }
  }
  release(&vtable.lock);
  if(next.vn){
    vfs_path_put(path);
    *path = next;
  }
}

int
vfs_path_unlink(const struct vfs_path *dir, const char *name, uint len)
{
  struct vnode *target;
  struct vfs_mount *mnt;
  int busy = 0, r = -1;

  if(dir == 0 || dir->mnt == 0 || dir->vn == 0 ||
     !valid_component(name, len) ||
     (len == 1 && name[0] == '.') ||
     (len == 2 && name[0] == '.' && name[1] == '.'))
    return -1;
  acquiresleep(&namespace_lock);
  // Lookup the covered inode directly, without following mounts.
  target = vfs_lookup(dir->vn, name, len);
  if(target){
    acquire(&vtable.lock);
    for(mnt = vtable.mounts; mnt < vtable.mounts + NVMOUNT; mnt++)
      if(mnt->ref > 0 && mnt->covered.vn == target)
        busy = 1;
    release(&vtable.lock);
    if(!busy)
      r = vfs_unlink(dir->vn, name, len);
    vfs_put(target);
  }
  releasesleep(&namespace_lock);
  return r;
}

int
vfs_path_lookup(const struct vfs_path *dir, const char *name, uint len,
                struct vfs_path *out)
{
  const struct vfs_path *base = dir;

  *out = (struct vfs_path){0};
  if(dir == 0 || dir->mnt == 0 || dir->vn == 0)
    return -1;
  // /mnt/.. means the parent of the covered directory, not the covered
  // directory itself. An owned input mount keeps covered immutable/alive.
  if(name && len == 2 && name[0] == '.' && name[1] == '.' &&
     dir->vn == dir->mnt->root && dir->mnt->covered.vn)
    base = &dir->mnt->covered;
  out->vn = vfs_lookup(base->vn, name, len);
  if(out->vn == 0)
    return -1;
  out->mnt = vfs_mount_dup(base->mnt);
  vfs_follow_mount(out);
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
    // Each component carries its mount context across namespace transitions.
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
