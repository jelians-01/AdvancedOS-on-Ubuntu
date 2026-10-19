#ifndef XV6_VFS_DIRENT_H
#define XV6_VFS_DIRENT_H

// Shared kernel/user ABI. Include types.h first; names are byte strings.
#define VFS_NAME_MAX 255
#define VFS_DT_UNKNOWN 0

struct vfs_dirent {
  uint64 ino; // identity within the enumerated backend, not a mount lookup
  ushort type; // T_DIR/T_FILE/T_DEVICE, or VFS_DT_UNKNOWN
  ushort namelen; // excludes the terminating NUL
  uint reserved; // zero
  char name[VFS_NAME_MAX + 1]; // NUL terminated; unused bytes are zero
};

#endif
