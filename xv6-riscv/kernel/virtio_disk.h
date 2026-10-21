#ifndef XV6_VIRTIO_DISK_H
#define XV6_VIRTIO_DISK_H
#define VDISK_COUNT 3
#define VDISK_SECTOR_SIZE 512
int virtio_disk_size(uint dev, uint64 *bytes);
int virtio_disk_transfer(uint dev, uint64 sector, void *data, uint bytes, int write);
int virtio_disk_flush(uint dev);
void virtio_disk_intr_dev(uint dev);
#endif
