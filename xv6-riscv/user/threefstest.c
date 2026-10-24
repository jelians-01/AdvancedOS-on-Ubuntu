#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// All names fit both native xv6's 14-byte limit and FAT32's 8.3 format.
// ext2 creates only in its root. 30001 bytes crosses ext2's direct blocks.
#define DATA_SIZE 30001
#define SHORT_SIZE 17

static const char *roots[] = {"", "/mnt", "/fat"};
static const char *names[] = {"native", "ext2", "fat32"};
static const int from[] = {0, 1, 0, 2, 1, 2};
static const int to[] =   {1, 0, 2, 0, 2, 1};
static const char *copies[] = {
  "c01.bin", "c10.bin", "c02.bin", "c20.bin", "c12.bin", "c21.bin"
};

static void
fail(const char *what, const char *path)
{
  fprintf(2, "threefstest: FAIL: %s: %s\n", what, path);
  exit(1);
}

static void
path_for(char *path, int fs, const char *name)
{
  strcpy(path, roots[fs]);
  strcpy(path + strlen(path), "/");
  strcpy(path + strlen(path), name);
}

static uchar
pattern(uint offset, int seed)
{
  if(seed == -2)
    return 'A' + (offset * 7 + offset / 31) % 26;
  return (offset * 37 + (offset >> 8) + seed * 29) % 251;
}

static void
write_all(int fd, char *buf, int size, const char *path)
{
  int done = 0;
  while(done < size){
    int n = write(fd, buf + done, size - done);
    if(n <= 0 || n > size - done)
      fail("short/failed write", path);
    done += n;
  }
}

static void
write_pattern(const char *path, int size, int seed, int truncate)
{
  char buf[509];
  int fd = open(path, O_CREATE | O_WRONLY | (truncate ? O_TRUNC : 0));
  if(fd < 0)
    fail("open for write", path);
  for(int offset = 0; offset < size;){
    int n = size - offset;
    if(n > sizeof(buf))
      n = sizeof(buf);
    for(int i = 0; i < n; i++)
      buf[i] = pattern(offset + i, seed);
    write_all(fd, buf, n, path);
    offset += n;
  }
  if(close(fd) < 0)
    fail("close after write", path);
}

static void
verify_pattern(const char *path, int size, int seed, int prefix_seed)
{
  struct stat st;
  char buf[317];
  int fd = open(path, O_RDONLY);
  if(fd < 0)
    fail("open for read", path);
  if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size != size)
    fail("type/length", path);
  for(int offset = 0; offset < size;){
    int want = size - offset;
    if(want > sizeof(buf))
      want = sizeof(buf);
    int got = read(fd, buf, want);
    if(got <= 0 || got > want)
      fail("short/failed read", path);
    for(int i = 0; i < got; i++){
      int use = prefix_seed >= 0 && offset + i < SHORT_SIZE ? prefix_seed : seed;
      if((uchar)buf[i] != pattern(offset + i, use))
        fail("byte mismatch", path);
    }
    offset += got;
  }
  if(read(fd, buf, 1) != 0)
    fail("EOF", path);
  if(close(fd) < 0)
    fail("close after read", path);
}

static void
copy_file(const char *source, const char *target)
{
  char buf[733];
  int input = open(source, O_RDONLY);
  if(input < 0)
    fail("copy source", source);
  int output = open(target, O_CREATE | O_WRONLY | O_TRUNC);
  if(output < 0)
    fail("copy target", target);
  int n;
  while((n = read(input, buf, sizeof(buf))) > 0)
    write_all(output, buf, n, target);
  if(n < 0)
    fail("copy read", source);
  if(close(input) < 0 || close(output) < 0)
    fail("copy close", target);
}

static void
verify_all(void)
{
  char path[64];
  for(int fs = 0; fs < 3; fs++){
    path_for(path, fs, "tsrc.bin");
    verify_pattern(path, DATA_SIZE, fs + 1, -1);
    printf("threefstest: verify %s source OK\n", names[fs]);
    path_for(path, fs, "tzero.bin");
    verify_pattern(path, 0, 0, -1);
    printf("threefstest: verify %s zero OK\n", names[fs]);
    path_for(path, fs, "tshort.bin");
    verify_pattern(path, SHORT_SIZE, 40 + fs, -1);
    printf("threefstest: verify %s truncate OK\n", names[fs]);
    path_for(path, fs, "tover.bin");
    verify_pattern(path, DATA_SIZE, 70 + fs, 90 + fs);
    printf("threefstest: verify %s overwrite OK\n", names[fs]);
  }
  for(int i = 0; i < 6; i++){
    path_for(path, to[i], copies[i]);
    verify_pattern(path, DATA_SIZE, from[i] + 1, -1);
    printf("threefstest: copy %s to %s OK\n", names[from[i]], names[to[i]]);
  }
}

int
main(int argc, char **argv)
{
  char source[64], target[64];
  if(argc == 2 && strcmp(argv[1], "official-prepare") == 0){
    struct stat st;
    if(stat("/tests", &st) < 0){
      if(mkdir("/tests") < 0)
        fail("prepare directory", "/tests");
    } else if(st.type != T_DIR){
      fail("prepare directory type", "/tests");
    }
    write_pattern("/mnt/hello.c", DATA_SIZE, -2, 1);
    write_pattern("/hello.c", DATA_SIZE + 7, -2, 1);
    write_pattern("/tests/hello.c", DATA_SIZE + 7, -2, 1);
    if(sync() < 0)
      fail("sync", "official fixtures");
    printf("threefstest: OFFICIAL FIXTURES READY\n");
    exit(0);
  }
  if(argc == 2 && strcmp(argv[1], "official-verify") == 0){
    const char *paths[] = {"/mnt/hello.c", "/hello.c", "/tests/hello.c"};
    for(int i = 0; i < 3; i++){
      verify_pattern(paths[i], DATA_SIZE, -2, -1);
      printf("threefstest: official %s OK\n", paths[i]);
    }
    printf("threefstest: OFFICIAL VERIFY PASSED\n");
    exit(0);
  }
  if(argc == 2 && strcmp(argv[1], "verify") == 0){
    verify_all();
    printf("threefstest: PERSISTENCE PASSED\n");
    exit(0);
  }
  if(argc != 1){
    fprintf(2, "usage: threefstest [verify|official-prepare|official-verify]\n");
    exit(2);
  }
  for(int fs = 0; fs < 3; fs++){
    path_for(target, fs, "tsrc.bin");
    write_pattern(target, DATA_SIZE, fs + 1, 1);
    path_for(target, fs, "tzero.bin");
    write_pattern(target, DATA_SIZE, fs + 1, 1);
    write_pattern(target, 0, 0, 1);
    path_for(target, fs, "tshort.bin");
    write_pattern(target, DATA_SIZE, fs + 1, 1);
    write_pattern(target, SHORT_SIZE, 40 + fs, 1);
    path_for(target, fs, "tover.bin");
    write_pattern(target, DATA_SIZE, 70 + fs, 1);
    write_pattern(target, SHORT_SIZE, 90 + fs, 0);
  }
  for(int i = 0; i < 6; i++){
    path_for(source, from[i], "tsrc.bin");
    path_for(target, to[i], copies[i]);
    copy_file(source, target);
  }
  if(sync() < 0)
    fail("sync", "all mounted filesystems");
  printf("threefstest: SYNC OK\n");
  verify_all();
  printf("threefstest: ALL TESTS PASSED\n");
  exit(0);
}
