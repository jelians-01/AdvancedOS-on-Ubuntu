#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static char payload[12289];

static void
fail(const char *what)
{
  fprintf(2, "ext2write: FAIL: %s\n", what);
  exit(1);
}

static int
create(const char *path)
{
  int fd = open(path, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0) fail(path);
  return fd;
}

static void
check(const char *path, const char *expected, int size)
{
  char buf[197];
  struct stat st;
  int fd = open(path, O_RDONLY), offset = 0;
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != size)
    fail("readback size/open");
  while(offset < size){
    int want = size - offset;
    if(want > sizeof(buf)) want = sizeof(buf);
    if(read(fd, buf, want) != want || memcmp(buf, expected + offset, want))
      fail("readback contents");
    offset += want;
  }
  if(read(fd, buf, 1) != 0) fail("readback EOF");
  close(fd);
}

static void
copy(const char *from, const char *to)
{
  char buf[317];
  int src = open(from, O_RDONLY), dst = create(to), n;
  if(src < 0) fail("copy source");
  while((n = read(src, buf, sizeof(buf))) > 0)
    if(write(dst, buf, n) != n) fail("copy write");
  if(n < 0) fail("copy read");
  close(src);
  close(dst);
}

static void
verify(void)
{
  check("/mnt/b-copy.bin", payload, 3073);
  check("/mnt/b-empty", "", 0);
  check("/mnt/b-short", "short\n", 6);
  check("/mnt/b-limit", payload, 12288);
  payload[0] = 'X'; payload[1] = 'Y'; payload[2] = 'Z';
  check("/mnt/b-overwrite", payload, 3073);
  for(int i = 0; i < 3; i++) payload[i] = i % 251;
}

// These modes run on separate small volumes prepared by the host runner.
static void
space_test(const char *kind)
{
  int blocks = strcmp(kind, "blocks") == 0;
  int directory = strcmp(kind, "directory") == 0;
  int count = 0;
  char path[120];
  for(int i = 0; i < 40; i++){
    strcpy(path, "/mnt/f00");
    path[6] = '0' + i / 10;
    path[7] = '0' + i % 10;
    if(directory){
      memset(path + 8, 'a', 92);
      path[100] = 0;
    }
    int fd = open(path, O_CREATE | O_RDWR);
    if(fd < 0){
      if(blocks) fail("inode/dir ran out before data blocks");
      break;
    }
    count++;
    if(blocks && write(fd, payload, 12288) != 12288){
      struct stat st;
      if(fstat(fd, &st) < 0 || st.size >= 12288 || st.size % 1024 ||
         write(fd, payload, 1024) != -1)
        fail("ENOSPC partial progress");
      close(fd);
      check(path, payload, st.size);
      break;
    }
    close(fd);
  }
  if(count == 0 || (!directory && count == 40)) fail("space limit not reached");
  if(blocks){
    int fd = create("/mnt/no-space");
    if(write(fd, payload, 1024) != -1) fail("full disk accepted data");
    close(fd);
    check("/mnt/no-space", "", 0);
    fd = create("/mnt/f00");
    if(write(fd, payload, 12288) != 12288) fail("ENOSPC recovery after truncate");
    close(fd);
    check("/mnt/f00", payload, 12288);
  } else if(directory) {
    if(count != 40) fail("directory expansion stopped early");
    // O_CREATE on an existing regular file is idempotent and must not
    // consume another inode or append a duplicate directory record.
    int dup = open(path, O_CREATE | O_RDWR);
    if(dup < 0) fail("directory duplicate create");
    struct stat st;
    if(fstat(dup, &st) < 0 || st.size != 0) fail("directory duplicate size");
    close(dup);
    int dirfd = open("/mnt", O_RDONLY), seen = 0;
    struct vfs_dirent de;
    if(dirfd < 0) fail("directory readdir open");
    while(readdir(dirfd, &de) == 1)
      if(de.namelen > 1 && de.name[0] == 'f') seen++;
    close(dirfd);
    if(seen != 40) fail("directory expansion enumeration");
  } else {
    for(int i = 0; i < 3; i++)
      if(open(path, O_CREATE | O_RDWR) != -1) fail("limit retry accepted");
  }
  printf("ext2write: SPACE %s PASSED (%d files)\n", kind, count);
  exit(0);
}

int
main(int argc, char **argv)
{
  for(int i = 0; i < sizeof(payload); i++) payload[i] = i % 251;
  if(argc == 2 && (strcmp(argv[1], "blocks") == 0 ||
                  strcmp(argv[1], "inodes") == 0 ||
                  strcmp(argv[1], "directory") == 0))
    space_test(argv[1]);
  if(argc == 2 && strcmp(argv[1], "verify") == 0){
    verify();
    printf("ext2write: PERSISTENCE PASSED\n");
    exit(0);
  }
  int fd = create("b-source");
  if(write(fd, payload, 3073) != 3073) fail("native source write");
  close(fd);
  copy("b-source", "/mnt/b-copy.bin");
  check("/mnt/b-copy.bin", payload, 3073);
  copy("/mnt/b-copy.bin", "b-return");
  check("b-return", payload, 3073);

  fd = create("/mnt/b-overwrite");
  if(write(fd, payload, 3073) != 3073) fail("overwrite fixture");
  close(fd);
  fd = open("/mnt/b-overwrite", O_CREATE | O_WRONLY);
  if(fd < 0 || write(fd, "XYZ", 3) != 3) fail("overwrite existing");
  close(fd);

  fd = create("/mnt/b-short");
  if(write(fd, payload, 3073) != 3073) fail("truncate fixture");
  close(fd);
  // Repeated truncate/rewrite exercises allocation reuse and stale-data removal.
  for(int i = 0; i < 3; i++){
    fd = create("/mnt/b-short");
    if(write(fd, "short\n", 6) != 6) fail("short replacement");
    close(fd);
  }
  fd = create("/mnt/b-empty");
  if(write(fd, payload, 3073) != 3073) fail("empty fixture");
  close(fd);
  close(create("/mnt/b-empty"));

  fd = create("/mnt/b-limit");
  if(write(fd, payload, 12289) != 12289) fail("first indirect byte");
  close(fd);
  check("/mnt/b-limit", payload, 12289);
  fd = create("/mnt/b-limit");
  struct stat st;
  if(fstat(fd, &st) < 0 || st.size != 0) fail("indirect truncate size");
  if(write(fd, payload, 12288) != 12288)
    fail("direct-block boundary");
  close(fd);

  fd = create("/mnt/b-badptr");
  if(write(fd, (void *)0xffffffffffffffffUL, 10) != -1)
    fail("bad pointer accepted");
  if(fstat(fd, &st) < 0 || st.size != 0) fail("bad pointer changed size");
  close(fd);
  if(open("/mnt/double.bin", O_WRONLY | O_TRUNC) != -1)
    fail("double indirect truncate accepted");
  fd = open("/mnt/double.bin", O_WRONLY);
  if(fd < 0 || write(fd, payload, 1) != -1) fail("double indirect write accepted");
  close(fd);
  // Subdirectory creation is now supported. Verify data and remove the
  // temporary object so host allocation accounting keeps its original scope.
  fd = create("/mnt/lost+found/new");
  if(write(fd, payload, 3073) != 3073) fail("subdirectory write");
  close(fd);
  check("/mnt/lost+found/new", payload, 3073);
  if(unlink("/mnt/lost+found/new") != 0) fail("subdirectory cleanup");
  if(stat("/mnt/lost+found/new", &st) != -1)
    fail("subdirectory cleanup still visible");
  verify();
  unlink("b-source");
  unlink("b-return");
  printf("ext2write: ALL TESTS PASSED\n");
  exit(0);
}
