#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "vfs.h"
#include "vfs_backend.h"

// Backend fields are immutable while ref > 0.
// References and slot allocation are protected by vtable.lock.
struct vnode {
  int ref;
  const struct vnode_ops *ops;
  void *data;
};

static struct {
  struct spinlock lock;
  struct vnode nodes[NINODE];
} vtable;

void
vfs_init(void)
{
  initlock(&vtable.lock, "vtable");
}

struct vnode*
vfs_node_get(const struct vnode_ops *ops, void *data)
{
  struct vnode *vn, *empty = 0;

  acquire(&vtable.lock);
  for(vn = vtable.nodes; vn < vtable.nodes + NINODE; vn++){
    if(vn->ref > 0 && vn->ops == ops && vn->data == data){
      vn->ref++;
      release(&vtable.lock);
      // The existing vnode already owns a backend reference.
      // Release the extra one outside the table's spinlock.
      ops->release(data);
      return vn;
    }
    if(vn->ref == 0 && empty == 0)
      empty = vn;
  }
  if(empty){
    empty->ops = ops;
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
  const struct vnode_ops *ops;
  void *data;

  acquire(&vtable.lock);
  if(vn == 0 || vn->ref < 1)
    panic("vfs_put");
  if(--vn->ref > 0){
    release(&vtable.lock);
    return;
  }
  ops = vn->ops;
  data = vn->data;
  vn->ops = 0;
  vn->data = 0;
  release(&vtable.lock);

  // The slot may now be reused. Use only the saved backend fields.
  ops->release(data);
}

void
vfs_lock(struct vnode *vn)
{
  vn->ops->lock(vn->data);
}

void
vfs_unlock(struct vnode *vn)
{
  vn->ops->unlock(vn->data);
}

int
vfs_read_at(struct vnode *vn, int user_dst, uint64 dst, uint off, uint n)
{
  return vn->ops->read_at(vn->data, user_dst, dst, off, n);
}

int
vfs_getattr(struct vnode *vn, struct stat *st)
{
  return vn->ops->getattr(vn->data, st);
}

int
vfs_write_at(struct vnode *vn, int user_src, uint64 src, uint off, uint n)
{
  if((user_src != 0 && user_src != 1) ||
     n > 0x7fffffffU || off + n < off)
    return -1;
  if(vn->ops->write_at == 0)
    return -1;
  if(n == 0)
    return 0;
  return vn->ops->write_at(vn->data, user_src, src, off, n);
}

int
vfs_truncate_zero(struct vnode *vn)
{
  if(vn->ops->truncate_zero == 0)
    return -1;
  return vn->ops->truncate_zero(vn->data);
}
