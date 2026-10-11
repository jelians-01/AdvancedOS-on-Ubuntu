#ifndef XV6_VFS_H
#define XV6_VFS_H

// Include types.h before this header.
struct vnode;
struct vfs_fs;
struct vfs_mount;

// Owned pair. Initialize to {0}; copying fields transfers ownership only.
// Use vfs_path_dup for a second owner, vfs_path_put to release and clear.
struct vfs_path {
  struct vfs_mount *mnt;
  struct vnode *vn;
};
struct stat;

// Initialize VFS memory state; no disk I/O.
void vfs_init(void);

// Caller already owns an instance reference. dup does not sleep.
// put consumes one reference and may sleep; no spinlock/node lock/transaction.
struct vfs_fs *vfs_fs_dup(struct vfs_fs *fs);
void vfs_fs_put(struct vfs_fs *fs);

// Borrow a live instance; return one unlocked directory vnode reference or 0.
// No node lock or outer transaction. Root lookup/release may sleep.
struct vnode *vfs_fs_root(struct vfs_fs *fs);

// Create a detached context borrowing fs; owns a root vnode on success.
// Does not attach it anywhere. May sleep; no outer locks/transaction.
struct vfs_mount *vfs_mount_create(struct vfs_fs *fs);
struct vfs_mount *vfs_mount_dup(struct vfs_mount *mnt); // non-sleeping
void vfs_mount_put(struct vfs_mount *mnt); // may sleep on final release

// Borrow a live mount, return an owned root pair in empty *out.
// Path output arguments below must be non-null, empty and not alias inputs.
// Success 0, failure -1 with *out empty. No outer locks/transaction.
int vfs_mount_root(struct vfs_mount *mnt, struct vfs_path *out);
// Borrow a valid path; acquire both references. Does not sleep.
struct vfs_path vfs_path_dup(const struct vfs_path *path);
// Consume/clear both references; empty paths are allowed. May sleep.
void vfs_path_put(struct vfs_path *path);

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

// Caller holds a live reference and the node lock.
// Return the device's devsw major, or -1 for non-devices/unsupported backends.
// Metadata only: device I/O is dispatched separately, without this lock.
int vfs_device_major(struct vnode *vn);

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

// Borrow a path, retain its mount context in the returned child path.
int vfs_path_lookup(const struct vfs_path *dir, const char *name, uint len,
                    struct vfs_path *out);
int vfs_path_create(const struct vfs_path *dir, const char *name, uint len,
                    struct vfs_path *out);

// Caller selects root for absolute names, cwd for relative names.
// Leading '/' does not change start here. No mounted-directory crossing yet.
// Borrow start and a kernel string shorter than MAXPATH; empty path succeeds.
// All path-producing operations return 0/-1 and obey the output rules above.
int vfs_path_walk(const struct vfs_path *start, const char *path,
                  struct vfs_path *out);
// Return the parent and a borrowed slice of path (keep path alive).
// Empty/all-slash paths fail; failure clears *name and *len as well as *out.
// name and len are required, non-null outputs.
int vfs_path_parent(const struct vfs_path *start, const char *path,
                    struct vfs_path *out, const char **name, uint *len);

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
