#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/vfs_dirent.h"

#define NAME_WIDTH 14
#include "kernel/fcntl.h"

char*
fmtname(char *path)
{
  static char buf[NAME_WIDTH+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= NAME_WIDTH)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', NAME_WIDTH-strlen(p));
  buf[sizeof(buf)-1] = '\0';
  return buf;
}

void
ls(char *path)
{
  char buf[512], *p;
  int fd, r;
  struct vfs_dirent de;
  struct stat st;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE:
  case T_FILE:
    printf("%s %d %d %d\n", fmtname(path), st.type, st.ino, (int) st.size);
    break;

  case T_DIR:
    if(strlen(path) + 2 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while((r = readdir(fd, &de)) == 1){
      if((p - buf) + de.namelen + 1 > sizeof buf){
        fprintf(2, "ls: path too long\n");
        continue;
      }
      memmove(p, de.name, de.namelen + 1);
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, (int) st.size);
    }
    if(r < 0)
      fprintf(2, "ls: cannot read directory %s\n", path);
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
