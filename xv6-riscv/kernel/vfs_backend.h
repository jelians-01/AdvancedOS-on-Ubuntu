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
  // Same reference/output rules as lookup; lock the find-or-create operation.
  // Create only regular files. Native may return an existing device for rev5.
  // No outer lock/transaction; backend exits its locks/transaction on return.
  int (*create_file)(void *dir, const char *name, uint len, void **out_node);
  // Borrow dir, no outer lock/transaction; return 0/-1, no output reference.
  // Existing names fail. Release all temporary nodes before returning.
  int (*mkdir)(void *dir, const char *name, uint len);
  int (*mknod)(void *dir, const char *name, uint len, int major, int minor);
  // Borrow both nodes, no outer locks/transaction.
  // same_fs: return exactly 1 iff both nodes belong to the same instance.
  // Inspect only immutable identity; required for link (same ops is not enough).
  int (*same_fs)(void *dir, void *target);
  // Non-directory target; reject dead parents/targets and link-count overflow.
  // Return 0/-1, no output references; undo a reserved link on failure.
  int (*link)(void *dir, const char *name, uint len, void *target);
  // Borrow dir; reject "."/".." and nonempty directories. Return 0/-1.
  // Release internal lookup references; do not consume the borrowed parent.
  int (*unlink)(void *dir, const char *name, uint len);
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

// Acquire one unlocked native root vnode, or return 0 if wrapping fails.
// Call after fsinit in process context, without spinlocks/node locks or an
// outer native transaction; wrapping/release may sleep. Caller vfs_put()s.
struct vnode *vfs_xv6_root(void);

// Resolve an absolute/relative path using the native root/vnode cwd.
// Return one unlocked vnode reference or 0; caller eventually vfs_put()s.
// No node lock or outer native transaction. Single-root bridge until mounts.
struct vnode *vfs_xv6_lookup_path(const char *path);

// Resolve via VFS components, then return one unlocked inode reference.
// Native-only transition for open; caller eventually iput()s in a transaction.
// Enter without a node lock or outer native transaction.
struct inode *vfs_xv6_namei(const char *path);

// Find the parent via VFS, then create/open a regular file in that directory.
// Same ownership/entry rules as vfs_xv6_namei; existing devices are compatible.
struct inode *vfs_xv6_create_file(const char *path);

// Native-only path bridges for mkdir/mknod. No outer lock/transaction.
// Return 0/-1; no reference is transferred to the caller.
int vfs_xv6_mkdir(const char *path);
int vfs_xv6_mknod(const char *path, int major, int minor);

// Native-only link/unlink path bridges; no outer locks/transaction.
// Return 0/-1; resolve and release all temporary VFS references internally.
int vfs_xv6_link(const char *oldpath, const char *newpath);
int vfs_xv6_unlink(const char *path);

#endif
