#ifndef XV6_VFS_BACKEND_H
#define XV6_VFS_BACKEND_H

// Backend interface. Include types.h first.
struct vnode;
struct inode;
struct stat;

struct vnode_ops {
  void (*lock)(void *data);
  void (*unlock)(void *data);
  int (*read_at)(void *data, int user_dst, uint64 dst, uint off, uint n);
  int (*getattr)(void *data, struct stat *st);
  void (*release)(void *data);
};

// Caller owns one backend reference and holds no node lock or transaction.
// Success consumes that reference and returns one vnode reference.
// Failure returns 0 and leaves the backend reference with the caller.
// Equal (ops, data) identifies the same live backend node.
// The operations table must remain valid for the lifetime of the vnode.
struct vnode *vfs_node_get(const struct vnode_ops *ops, void *data);

// Temporary native bridge. Caller keeps its existing inode reference.
// Returns an additional vnode reference, or 0 if the table is full.
// Call without a node lock or an outer native log transaction.
struct vnode *vfs_xv6_get(struct inode *ip);

#endif
