//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "vfs_backend.h"
#include "vfs.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same node as old.
uint64
sys_link(void)
{
  char new[MAXPATH], old[MAXPATH];

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;
  return vfs_xv6_link(old, new);
}

uint64
sys_unlink(void)
{
  char path[MAXPATH];

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return vfs_xv6_unlink(path);
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode, r, major = -1;
  struct file *f;
  struct vnode *vn;
  struct vfs_path opened = {0};
  struct stat st;

  argint(1, &omode);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  // Own the mount and vnode until both are handed to the open file.
  if(omode & O_CREATE)
    r = vfs_xv6_create_file(path, &opened);
  else
    r = vfs_xv6_lookup_path(path, &opened);
  if(r < 0)
    return -1;
  vn = opened.vn; // Borrowed while opened (then f->path) owns it.

  vfs_lock(vn);
  r = vfs_getattr(vn, &st);
  if(r == 0 && st.type == T_DEVICE)
    major = vfs_device_major(vn);
  vfs_unlock(vn);
  if(r < 0 ||
     (st.type != T_FILE && st.type != T_DIR && st.type != T_DEVICE) ||
     (st.type == T_DIR && omode != O_RDONLY) ||
     (st.type == T_DEVICE && (major < 0 || major >= NDEV))){
    vfs_path_put(&opened);
    return -1;
  }

  // Allocate before truncating: descriptor exhaustion must preserve contents.
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    vfs_path_put(&opened);
    return -1;
  }

  f->type = st.type == T_DEVICE ? FD_DEVICE : FD_VNODE;
  f->path = opened; // Transfer both references; no extra dup.
  f->major = major;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && st.type == T_FILE &&
     vfs_truncate_zero(vn) < 0){
    myproc()->ofile[fd] = 0;
    fileclose(f);
    return -1;
  }

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return vfs_xv6_mkdir(path);
}

uint64
sys_mknod(void)
{
  char path[MAXPATH];
  int major, minor;

  argint(1, &major);
  argint(2, &minor);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return vfs_xv6_mknod(path, major, minor);
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct vfs_path next = {0}, old;
  struct stat st;
  struct proc *p = myproc();
  int r;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  if(vfs_xv6_lookup_path(path, &next) < 0)
    return -1;
  vfs_lock(next.vn);
  r = vfs_getattr(next.vn, &st);
  vfs_unlock(next.vn);
  if(r < 0 || st.type != T_DIR){
    vfs_path_put(&next);
    return -1;
  }
  // Transfer both references; xv6 has no threads sharing this proc.
  old = p->cwd;
  p->cwd = next;
  vfs_path_put(&old);
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
