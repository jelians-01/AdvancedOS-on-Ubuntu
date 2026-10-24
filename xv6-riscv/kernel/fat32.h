#ifndef XV6_FAT32_H
#define XV6_FAT32_H

// Mount a FAT32 volume from a D1 byte-addressed device.
// The kernel backend supports 8.3 regular-file create, write and truncate;
// VFAT names and directory creation remain unsupported.
// Returns 0 on success, -1 if the device or volume is unsupported.
int fat32_mount(uint dev, const char *mountpoint);

#endif
