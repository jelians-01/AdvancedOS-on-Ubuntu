#ifndef XV6_EXT2_H
#define XV6_EXT2_H

// Mount an ext2 volume from a D1 byte-addressed device at a native directory.
// The first backend version is read-only. Returns 0 on success, -1 on failure.
int ext2_mount(uint dev, const char *mountpoint);

#endif
