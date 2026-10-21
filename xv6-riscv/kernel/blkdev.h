#ifndef XV6_BLKDEV_H
#define XV6_BLKDEV_H
// Include types.h first. D1: byte ranges, kernel buffers, synchronous I/O.
// May sleep; success 0 means all bytes completed, failure -1.
int blk_read(uint dev, uint64 byte_off, void *dst, uint nbytes);
int blk_write(uint dev, uint64 byte_off, const void *src, uint nbytes);
int blk_size(uint dev, uint64 *out_bytes);
int blk_flush(uint dev);
void blk_init(void); // Called once by platform disk initialization.
#endif
