#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char payload[] = "vfs-path\n";

static void
fail(const char *what)
{
  fprintf(2, "vfspath: FAIL: %s\n", what);
  exit(1);
}

static void
create_file(const char *path)
{
  int fd = open(path, O_CREATE | O_WRONLY);

  if(fd < 0 || write(fd, payload, sizeof(payload)-1) != sizeof(payload)-1)
    fail(path);
  close(fd);
}

static void
check_file(const char *path, struct stat *expected)
{
  struct stat st;
  char buf[sizeof(payload)];
  int fd = open(path, O_RDONLY);

  if(fd < 0)
    fail(path);
  if(fstat(fd, &st) < 0 || st.dev != expected->dev ||
     st.ino != expected->ino || st.type != T_FILE ||
     st.size != sizeof(payload)-1)
    fail("file identity");
  if(read(fd, buf, sizeof(buf)) != sizeof(payload)-1 ||
     memcmp(buf, payload, sizeof(payload)-1) != 0 ||
     read(fd, buf, 1) != 0)
    fail("file contents or EOF");
  close(fd);
}

static void
check_same_dir(const char *a, const char *b)
{
  struct stat sa, sb;

  if(stat(a, &sa) < 0 || stat(b, &sb) < 0 ||
     sa.type != T_DIR || sb.type != T_DIR ||
     sa.dev != sb.dev || sa.ino != sb.ino)
    fail("directory identity");
}

static void
check_missing(const char *path)
{
  int fd = open(path, O_RDONLY);

  if(fd >= 0){
    close(fd);
    fail(path);
  }
}

int
main(void)
{
  struct stat data, longname, cycle;
  const char *paths[] = {
    "vfspath.tmp/data",
    "/vfspath.tmp/data",
    "///vfspath.tmp//data",
    "./vfspath.tmp/./data",
    "vfspath.tmp/sub/../data",
    "vfspath.tmp/data///",
  };

  if(chdir("/") < 0)
    fail("chdir root");
  // Do not overwrite or delete a directory left by another invocation.
  if(mkdir("vfspath.tmp") < 0)
    fail("mkdir vfspath.tmp (inspect any existing directory)");
  if(mkdir("vfspath.tmp/sub") < 0)
    fail("mkdir sub");
  create_file("vfspath.tmp/data");
  create_file("vfspath.tmp/123456789012345");
  if(stat("vfspath.tmp/data", &data) < 0 ||
     stat("vfspath.tmp/12345678901234", &longname) < 0)
    fail("fixture stat");

  for(uint i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
    check_file(paths[i], &data);
  check_file("vfspath.tmp/123456789012345", &longname);
  check_file("vfspath.tmp/12345678901234suffix", &longname);
  check_same_dir("/", "////");
  check_same_dir("/", "/..");
  check_same_dir("vfspath.tmp", "vfspath.tmp/sub/..");

  if(chdir("vfspath.tmp/sub") < 0)
    fail("chdir sub");
  check_file("../data", &data);
  check_file("/vfspath.tmp/data", &data);
  check_same_dir("", ".");
  check_same_dir("./../", "/vfspath.tmp");
  if(chdir("/") < 0)
    fail("chdir back");

  // Exceed the vnode table size to expose leaked traversal references.
  for(int i = 0; i < 200; i++){
    check_missing("vfspath.tmp/sub/missing");
    check_missing("vfspath.tmp/data/child");
    check_missing("vfspath.tmp/data/..");
    check_file("vfspath.tmp/sub/../data", &data);
    create_file("vfspath.tmp/cycle");
    if(stat("vfspath.tmp/cycle", &cycle) < 0)
      fail("cycle stat");
    check_file("vfspath.tmp/sub/../cycle", &cycle);
    if(unlink("vfspath.tmp/cycle") < 0)
      fail("cycle unlink");
  }

  if(unlink("vfspath.tmp/data") < 0 ||
     unlink("vfspath.tmp/12345678901234") < 0 ||
     unlink("vfspath.tmp/sub") < 0 || unlink("vfspath.tmp") < 0)
    fail("cleanup");
  printf("vfspath: ALL TESTS PASSED\n");
  exit(0);
}
