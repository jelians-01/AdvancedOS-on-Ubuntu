#ifndef XV6_VFS_H
#define XV6_VFS_H

// Include types.h before this header.
struct vnode;
struct stat;

// Initialize VFS memory state; no disk I/O.
void vfs_init(void);

// Caller must already hold a valid vnode reference.
struct vnode *vfs_dup(struct vnode *vn);

// Consume one reference. May sleep and reclaim storage.
// Call without a node lock or an outer native log transaction.
void vfs_put(struct vnode *vn);

// Caller must hold a reference throughout the locked section.
void vfs_lock(struct vnode *vn);
void vfs_unlock(struct vnode *vn);

// Caller holds the node lock.
// user_dst: 1 for a user address, 0 for a kernel address.
// Return bytes read, 0 at EOF, or -1 on error.
// Does not update the open file's offset.
int vfs_read_at(struct vnode *vn, int user_dst,
                uint64 dst, uint off, uint n);

// Caller holds the node lock; st points to kernel memory.
// Return 0 on success, or -1 on error.
int vfs_getattr(struct vnode *vn, struct stat *st);

// Caller holds a reference, but no node lock or native log transaction.
// user_src: 1 for a user address, 0 for a kernel address.
// Return bytes written, 0 for an empty request, or -1 if no progress.
// A partial write returns its positive byte count. Does not change file.off.
int vfs_write_at(struct vnode *vn, int user_src,
                 uint64 src, uint off, uint n);

// Caller holds a reference, but no node lock or native log transaction.
// The backend locks the node and handles its own transaction.
// Truncate a regular file to zero bytes; return 0 or -1.
int vfs_truncate_zero(struct vnode *vn);

// Borrow dir; no node lock or outer native transaction.
// name is one kernel path component of len bytes, without '/' or NUL.
// Return one unlocked vnode reference, or 0 on failure.
struct vnode *vfs_lookup(struct vnode *dir, const char *name, uint len);

// Single-root transition: caller selects root for absolute paths or cwd
// for relative paths. Leading '/' does not change start inside this helper.
// Borrow start; path is a kernel string shorter than MAXPATH.
// Return one unlocked reference, including for an empty path, or 0.
// No node lock or outer native transaction; no mount traversal yet.
struct vnode *vfs_walk(struct vnode *start, const char *path);

// Like vfs_walk, but stop at the parent directory of the final component.
// On success, *name borrows a slice of path, *len is its full byte length,
// and the caller owns one unlocked parent reference. Keep path alive.
// On failure return 0 and clear both outputs. Empty/all-slash paths fail.
// name and len must be non-null. No node lock or outer transaction.
struct vnode *vfs_walk_parent(struct vnode *start, const char *path,
                              const char **name, uint *len);

// Borrow dir; atomically find or create a regular file under that directory.
// Return one unlocked vnode reference or 0. Does not truncate existing files.
// Native compatibility: an existing device may also be returned.
// No node lock or outer transaction; the backend owns locks and transactions.
struct vnode *vfs_create_file(struct vnode *dir, const char *name, uint len);

// Borrow dir; name is one full component. Existing names fail.
// Return 0 on success or -1 on error; no new node reference is returned.
// No node lock or outer transaction; backend owns locking and transactions.
int vfs_mkdir(struct vnode *dir, const char *name, uint len);
int vfs_mknod(struct vnode *dir, const char *name, uint len,
              int major, int minor);

// Borrow dir and target; one full destination component, existing names fail.
// Return 0/-1. No outer node locks/transaction; backend owns both.
// Only same-instance, non-directory hard links are supported.
int vfs_link(struct vnode *dir, const char *name, uint len,
             struct vnode *target);

// Borrow dir; remove one name, including an empty directory.
// Reject "."/".."; open references keep an unlinked node alive.
// Return 0/-1; no outer node lock or transaction.
int vfs_unlink(struct vnode *dir, const char *name, uint len);

#endif
