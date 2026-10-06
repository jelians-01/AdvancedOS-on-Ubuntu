//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "vfs.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  struct file *f;

  initlock(&ftable.lock, "ftable");
  for(f = ftable.file; f < ftable.file + NFILE; f++)
    initsleeplock(&f->poslock, "filepos");
  vfs_init();
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      f->vn = 0;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  int type, writable;
  struct pipe *pipe;
  struct inode *ip;
  struct vnode *vn;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  // Save resources, not the sleep lock, before this slot can be reused.
  type = f->type;
  writable = f->writable;
  pipe = f->pipe;
  ip = f->ip;
  vn = f->vn;
  f->ref = 0;
  f->type = FD_NONE;
  f->vn = 0;
  release(&ftable.lock);

  if(type == FD_PIPE){
    pipeclose(pipe, writable);
  } else if(type == FD_INODE || type == FD_DEVICE){
    begin_op();
    iput(ip);
    end_op();
    // The bridge still owns separate inode and vnode references.
    // The vnode's final release starts its own transaction.
    if(vn)
      vfs_put(vn);
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    if(f->type == FD_INODE){
      int r;
      vfs_lock(f->vn);
      r = vfs_getattr(f->vn, &st);
      vfs_unlock(f->vn);
      if(r < 0)
        return -1;
    } else {
      ilock(f->ip);
      stati(f->ip, &st);
      iunlock(f->ip);
    }
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0 || n < 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    acquiresleep(&f->poslock);
    vfs_lock(f->vn);
    if((uint)n > (uint)-1 - f->off)
      r = -1;
    else if((r = vfs_read_at(f->vn, 1, addr, f->off, n)) > 0)
      f->off += r;
    vfs_unlock(f->vn);
    releasesleep(&f->poslock);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0 || n < 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // The backend owns the node lock and native log transactions.
    acquiresleep(&f->poslock);
    r = vfs_write_at(f->vn, 1, addr, f->off, n);
    if(r > 0)
      f->off += r;
    releasesleep(&f->poslock);
    // Preserve rev5's user-visible error on an incomplete write.
    ret = (r == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

