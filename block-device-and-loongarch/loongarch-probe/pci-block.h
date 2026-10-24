#ifndef PROBE_PCI_BLOCK_H
#define PROBE_PCI_BLOCK_H
#include <stdint.h>
#include <stddef.h>
/* D1-compatible synchronous byte-range entry points. Single CPU, polling;
 * no sleeping, interrupts, hotplug, partitions or concurrent callers yet.
 * Device 1 is protected against writes; device 2/3 follow device RO flags.
 */
void blk_init(void);
int blk_read(unsigned dev,uint64_t offset,void *destination,unsigned length);
int blk_write(unsigned dev,uint64_t offset,const void *source,unsigned length);
int blk_size(unsigned dev,uint64_t *bytes);
int blk_flush(unsigned dev);
unsigned pci_block_count(void);
unsigned pci_block_modern(unsigned dev);
unsigned pci_block_readonly(unsigned dev);
#endif
