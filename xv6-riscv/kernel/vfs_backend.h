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
  // S1: enter with a reference, no node lock and no outer transaction.
  // write_at returns actual progress, 0 for n == 0, or -1 on no progress.
  int (*write_at)(void *data, int user_src, uint64 src, uint off, uint n);
  // Clear a regular file; return 0 or -1.
  int (*truncate_zero)(void *data);
  // Borrow dir, enter unlocked with no outer native transaction.
  // Success: 0 and one unlocked backend reference in *out_node.
  // Failure: -1 and *out_node == 0. The child uses this same ops table.
  int (*lookup)(void *dir, const char *name, uint len, void **out_node);
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

// Temporary native bridge only: borrow data if the backend matches.
// Caller must keep vn referenced while using the returned pointer.
void *vfs_node_data(struct vnode *vn, const struct vnode_ops *ops);

// Resolve via VFS components, then return one unlocked inode reference.
// Native-only transition for open; caller eventually iput()s in a transaction.
// Enter without a node lock or outer native transaction.
struct inode *vfs_xv6_namei(const char *path);

#endif
