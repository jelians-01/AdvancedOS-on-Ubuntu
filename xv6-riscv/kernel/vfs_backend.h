#ifndef XV6_VFS_BACKEND_H
#define XV6_VFS_BACKEND_H

// Backend interface. Include types.h first.
struct vnode;
struct vfs_fs;
struct stat;

struct vnode_ops {
  void (*lock)(void *data);
  void (*unlock)(void *data);
  int (*read_at)(void *data, int user_dst, uint64 dst, uint off, uint n);
  int (*getattr)(void *data, struct stat *st);
  // Optional, called with the node locked. Return devsw major or -1.
  int (*device_major)(void *data);
  void (*release)(void *data);
  // S1: enter with a reference, no node lock and no outer transaction.
  // write_at returns actual progress, 0 for n == 0, or -1 on no progress.
  int (*write_at)(void *data, int user_src, uint64 src, uint off, uint n);
  // Clear a regular file; return 0 or -1.
  int (*truncate_zero)(void *data);
  // Borrow dir, enter unlocked with no outer native transaction.
  // Success: 0 and one unlocked backend reference in *out_node.
  // Failure: -1 and *out_node == 0. The child belongs to this same instance.
  int (*lookup)(void *dir, const char *name, uint len, void **out_node);
  // Same reference/output rules as lookup; lock the find-or-create operation.
  // Create only regular files. Native may return an existing device for rev5.
  // No outer lock/transaction; backend exits its locks/transaction on return.
  int (*create_file)(void *dir, const char *name, uint len, void **out_node);
  // Borrow dir, no outer lock/transaction; return 0/-1, no output reference.
  // Existing names fail. Release all temporary nodes before returning.
  int (*mkdir)(void *dir, const char *name, uint len);
  int (*mknod)(void *dir, const char *name, uint len, int major, int minor);
  // Borrow both nodes in the same instance; no outer locks/transaction.
  // Non-directory target; reject dead parents/targets and link-count overflow.
  // Return 0/-1, no output references; undo a reserved link on failure.
  int (*link)(void *dir, const char *name, uint len, void *target);
  // Borrow dir; reject "."/".." and nonempty directories. Return 0/-1.
  // Release internal lookup references; do not consume the borrowed parent.
  int (*unlink)(void *dir, const char *name, uint len);
};

struct vfs_fs_ops {
  // Borrow private instance data, no outer locks/transaction.
  // Return 0 and one unlocked backend root reference, or -1 with *out_node=0.
  int (*root)(void *data, void **out_node);
  // Optional for static private data. Called after the last instance/node ref.
  // May sleep; no VFS table lock is held. Not a sync/flush guarantee.
  void (*release)(void *data);
};

// Create one instance reference, or 0 if invalid/full. No disk I/O.
// On success VFS takes responsibility for private data via fs_ops->release;
// on failure the caller keeps it. Static data may omit the release callback.
// Tables/data stay valid and their identities immutable until final release.
// Node lock/unlock/read_at/getattr/release and fs root callbacks are required.
struct vfs_fs *vfs_fs_create(const struct vfs_fs_ops *fs_ops,
                            const struct vnode_ops *node_ops, void *data);

// Borrow a live instance; caller owns one backend node reference.
// No node lock or outer transaction. Success consumes the backend reference
// and returns one vnode reference; failure leaves it with the caller.
// Equal (instance, data) identifies the same live node. Every new vnode
// retains its instance until AFTER releasing its backend node.
struct vnode *vfs_node_get(struct vfs_fs *fs, void *data);

// Once, after fsinit and before the first native root lookup.
// Holds one permanent instance reference; no second native instance yet.
void vfs_xv6_init(void);

// Memory-only instance regression checks. Once before native init, no nodes
// or instances live; process context, no spinlocks. Panics on failure.
void vfs_selftest(void);

// Acquire one unlocked native root vnode, or return 0 if wrapping fails.
// Call after vfs_xv6_init in process context, without spinlocks/node locks or an
// outer native transaction; wrapping/release may sleep. Caller vfs_put()s.
struct vnode *vfs_xv6_root(void);

// Resolve an absolute/relative path using the native root/vnode cwd.
// Return one unlocked vnode reference or 0; caller eventually vfs_put()s.
// No node lock or outer native transaction. Single-root bridge until mounts.
struct vnode *vfs_xv6_lookup_path(const char *path);

// Find the parent via VFS, then create/open a regular file in that directory.
// Return one unlocked vnode reference or 0; caller eventually vfs_put()s.
// No node lock or outer native transaction; existing devices are compatible.
struct vnode *vfs_xv6_create_file(const char *path);

// Native-only path bridges for mkdir/mknod. No outer lock/transaction.
// Return 0/-1; no reference is transferred to the caller.
int vfs_xv6_mkdir(const char *path);
int vfs_xv6_mknod(const char *path, int major, int minor);

// Native-only link/unlink path bridges; no outer locks/transaction.
// Return 0/-1; resolve and release all temporary VFS references internally.
int vfs_xv6_link(const char *oldpath, const char *newpath);
int vfs_xv6_unlink(const char *path);

#endif
