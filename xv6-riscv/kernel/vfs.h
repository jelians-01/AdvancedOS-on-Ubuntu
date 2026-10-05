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

#endif
