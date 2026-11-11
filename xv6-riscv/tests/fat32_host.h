// Single-threaded fault injection only; this is not a scheduler/lock test.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/types.h"
#include "../kernel/param.h"
#include "../kernel/stat.h"
#include "../kernel/blkdev.h"
#include "../kernel/vfs.h"
#include "../kernel/vfs_dirent.h"
#include "../kernel/vfs_backend.h"
#define PGSIZE 4096

struct spinlock { int held; };
struct sleeplock { int held; };
static inline void initlock(struct spinlock *s, char *name)
{ (void)name; s->held = 0; }
static inline void acquire(struct spinlock *s)
{ assert(!s->held); s->held = 1; }
static inline void release(struct spinlock *s)
{ assert(s->held); s->held = 0; }
static inline void initsleeplock(struct sleeplock *s, char *name)
{ (void)name; s->held = 0; }
static inline void acquiresleep(struct sleeplock *s)
{ assert(!s->held); s->held = 1; }
static inline void releasesleep(struct sleeplock *s)
{ assert(s->held); s->held = 0; }

static uint host_allocations, host_fail_allocation;
static inline void *kalloc(void)
{
  if(++host_allocations == host_fail_allocation)
    return 0;
  return calloc(1, PGSIZE);
}
static inline void kfree(void *data)
{ free(data); }
static inline _Noreturn void panic(char *message)
{ fprintf(stderr, "panic: %s\n", message); abort(); }

struct proc { uint64 pagetable; };
static inline struct proc *myproc(void)
{ static struct proc p; return &p; }
static inline int copyin(uint64 table, void *dst, uint64 src, uint n)
{ (void)table; (void)dst; (void)src; (void)n; return -1; }
static inline int copyout(uint64 table, uint64 dst, void *src, uint n)
{ (void)table; (void)dst; (void)src; (void)n; return -1; }
