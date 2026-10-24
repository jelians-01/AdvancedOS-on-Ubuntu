#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "stat.h"
#include "vfs.h"
#include "vfs_backend.h"

static int loadseg(pde_t *, uint64, struct vnode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i;
  uint off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct vnode *vn;
  struct vfs_path image = {0};
  struct stat st;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  // Lookup/put may start backend transactions; enter without an outer one.
  if(vfs_xv6_lookup_path(path, &image) < 0)
    return -1;
  vn = image.vn; // Borrowed from the owned executable path.
  // Keep one stable image across the header and all segment reads.
  vfs_lock(vn);
  if(vfs_getattr(vn, &st) < 0 || st.type != T_FILE || st.size > 0xffffffffU)
    goto bad;

  // Read the ELF header.
  if(vfs_read_at(vn, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC || elf.machine != ARCH_ELF_MACHINE)
    goto bad;
  // VFS file offsets are uint. Check before narrowing any ELF uint64 field.
  if(elf.phoff > st.size ||
     (uint64)elf.phnum * sizeof(ph) > st.size - elf.phoff)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=(uint)elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(vfs_read_at(vn, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.off > st.size || ph.filesz > st.size - ph.off)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, vn, (uint)ph.off, (uint)ph.filesz) < 0)
      goto bad;
  }
  vfs_unlock(vn);
  vfs_path_put(&image);
  vn = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  uint64 user_argv = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  arch_exec_frame(p->trapframe, elf.entry, sp, user_argv);
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(vn){
    vfs_unlock(vn);
    vfs_path_put(&image);
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Caller holds the vnode lock and has checked the segment's file bounds.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct vnode *vn, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(vfs_read_at(vn, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
