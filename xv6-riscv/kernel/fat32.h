#ifndef XV6_FAT32_H
#define XV6_FAT32_H

// Mount a read-only FAT32 volume from a D1 byte-addressed device.
// Returns 0 on success, -1 if the device or volume is unsupported.
int fat32_mount(uint dev, const char *mountpoint);

#endif
