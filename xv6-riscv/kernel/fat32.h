#ifndef XV6_FAT32_H
#define XV6_FAT32_H

// Mount a FAT32 volume from a D1 byte-addressed device.
// Supports 8.3/VFAT names, file read/write/truncate, directories and unlink.
// Raw FAT32 volumes or one unambiguous MBR FAT32 primary partition (0B/0C).
// GPT, extended/logical partitions and crash recovery are unsupported.
// Returns 0 on success, -1 if the device or volume is unsupported.
int fat32_mount(uint dev, const char *mountpoint);

#endif
