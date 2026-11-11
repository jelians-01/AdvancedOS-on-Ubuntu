#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static const char path[] = "/fat/Shared file.txt";
static const char alias[] = "/fat/SHARED~1.TXT";
static char payload[1300], buffer[1300];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "fat32shared: FAIL: %s\n", what);
    exit(1);
  }
}

static void
size_is(int fd, uint expected)
{
  struct stat st;
  check(fstat(fd, &st) == 0 && st.size == expected, "shared size");
}

int
main(void)
{
  int a, b, c, fd, observer, pid, status;

  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = (i * 31 + 7) & 255;
  a = open(path, O_CREATE | O_RDWR);
  b = open(alias, O_RDWR);
  c = open(path, O_CREATE | O_RDWR);
  check(a >= 0 && b >= 0 && c >= 0, "three independent opens");
  check(write(a, payload, sizeof(payload)) == sizeof(payload), "initial write");
  size_is(b, sizeof(payload));
  size_is(c, sizeof(payload));
  check(read(b, buffer, sizeof(buffer)) == sizeof(buffer) &&
        memcmp(buffer, payload, sizeof(buffer)) == 0, "read through preexisting alias");
  check(write(c, "XY", 2) == 2, "overwrite through another handle");
  size_is(a, sizeof(payload));
  check(write(a, "!", 1) == 1, "append through first handle");
  size_is(b, sizeof(payload) + 1);

  fd = open(alias, O_WRONLY | O_TRUNC);
  check(fd >= 0, "truncate through new handle");
  close(fd);
  size_is(a, 0);
  size_is(b, 0);
  size_is(c, 0);
  check(read(b, buffer, 1) == 0, "old handle observes EOF");
  check(write(c, "x", 1) < 0, "file offset remains independent after truncate");
  fd = open(path, O_WRONLY);
  check(fd >= 0 && write(fd, "fresh", 5) == 5, "rewrite truncated file");
  close(fd);
  observer = open(path, O_RDONLY);
  check(observer >= 0, "observer open");

  pid = fork();
  check(pid >= 0, "fork");
  if(pid == 0){
    size_is(a, 5);
    fd = open(alias, O_RDWR);
    check(fd >= 0 && write(fd, "child", 5) == 5, "child alias write");
    close(fd);
    exit(0);
  }
  check(wait(&status) == pid && status == 0, "child completes");
  size_is(observer, 5);

  check(unlink(path) == 0, "unlink with multiple open handles");
  check(write(a, "x", 1) < 0 && write(c, "x", 1) < 0,
        "all old writers reject deleted file");
  fd = open(path, O_CREATE | O_RDWR);
  check(fd >= 0 && write(fd, "new", 3) == 3, "recreate same name");
  size_is(observer, 5);
  size_is(fd, 3);
  close(a);
  close(b);
  close(c);
  check(read(observer, buffer, 5) == 5 &&
        memcmp(buffer, "child", 5) == 0, "old incarnation survives other closes");
  close(observer);
  close(fd);

  fd = open(alias, O_RDONLY);
  check(fd >= 0 && read(fd, buffer, 3) == 3 &&
        memcmp(buffer, "new", 3) == 0, "new incarnation remains intact");
  close(fd);
  printf("fat32shared: PASS\n");
  exit(0);
}
