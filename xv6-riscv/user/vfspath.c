#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
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

static void
check_create_failure(const char *path)
{
  int fd = open(path, O_CREATE | O_RDWR);

  if(fd >= 0){
    close(fd);
    fail(path);
  }
}

static void
check_create_existing(const char *path, struct stat *expected)
{
  struct stat st;
  int fd = open(path, O_CREATE | O_RDWR);

  if(fd < 0 || fstat(fd, &st) < 0 ||
     st.dev != expected->dev || st.ino != expected->ino ||
     st.size != expected->size)
    fail("O_CREATE changed an existing file");
  close(fd);
  check_file(path, expected);
}

static void
check_concurrent_create(void)
{
  int gate[2], report[2], fd, status;
  struct stat st;
  uint ino;
  char byte;

  if(pipe(gate) < 0 || pipe(report) < 0)
    fail("race pipes");
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0)
      fail("race fork");
    if(pid == 0){
      close(gate[1]);
      close(report[0]);
      if(read(gate[0], &byte, 1) != 1)
        fail("race gate");
      close(gate[0]);
      fd = open("vfspath.tmp/race", O_CREATE | O_RDWR);
      if(fd < 0 || fstat(fd, &st) < 0 || st.type != T_FILE ||
         write(fd, "R", 1) != 1)
        fail("race create");
      if(write(report[1], &st.ino, sizeof(st.ino)) != sizeof(st.ino))
        fail("race report");
      close(fd);
      close(report[1]);
      exit(0);
    }
  }
  close(gate[0]);
  close(report[1]);
  if(write(gate[1], "xxxx", 4) != 4)
    fail("race start");
  close(gate[1]);
  for(int i = 0; i < 4; i++)
    if(wait(&status) < 0 || status != 0)
      fail("race child");
  if(stat("vfspath.tmp/race", &st) < 0 || st.type != T_FILE ||
     st.nlink != 1 || st.size != 1)
    fail("race final stat");
  for(int i = 0; i < 4; i++)
    if(read(report[0], &ino, sizeof(ino)) != sizeof(ino) || ino != st.ino)
      fail("race created different inodes");
  close(report[0]);
  fd = open("vfspath.tmp/race", O_RDONLY);
  if(fd < 0 || read(fd, &byte, 1) != 1 || byte != 'R' ||
     read(fd, &byte, 1) != 0)
    fail("race final content");
  close(fd);
  if(unlink("vfspath.tmp/race") < 0)
    fail("race cleanup");
}

static void
check_node_failure(const char *path)
{
  if(mkdir(path) != -1 || mknod(path, 1, 0) != -1)
    fail("mkdir/mknod accepted an existing or invalid path");
}

static void
check_concurrent_nodes(int device)
{
  const char *path = device ? "vfspath.tmp/racedev" : "vfspath.tmp/racedir";
  struct stat before, after, node;
  int gate[2], status, successes = 0;
  char byte;

  if(stat("vfspath.tmp", &before) < 0 || pipe(gate) < 0)
    fail("node race setup");
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0)
      fail("node race fork");
    if(pid == 0){
      int r;
      close(gate[1]);
      if(read(gate[0], &byte, 1) != 1)
        exit(2);
      close(gate[0]);
      r = device ? mknod(path, 1, 0) : mkdir(path);
      exit(r == 0 ? 0 : (r == -1 ? 1 : 2));
    }
  }
  close(gate[0]);
  if(write(gate[1], "xxxx", 4) != 4)
    fail("node race start");
  close(gate[1]);
  for(int i = 0; i < 4; i++){
    if(wait(&status) < 0 || (status != 0 && status != 1))
      fail("node race child");
    if(status == 0)
      successes++;
  }
  if(successes != 1 || stat(path, &node) < 0 ||
     node.type != (device ? T_DEVICE : T_DIR) ||
     stat("vfspath.tmp", &after) < 0 ||
     after.nlink != before.nlink + (device ? 0 : 1))
    fail("node race result");
  if(!device)
    check_same_dir("vfspath.tmp/racedir/..", "vfspath.tmp");
  if(unlink(path) < 0 || stat("vfspath.tmp", &after) < 0 ||
     after.nlink != before.nlink)
    fail("node race cleanup");
}

static void
check_directories(void)
{
  struct stat before, after, tree;
  const char *bad[] = {
    "", "/", "////", ".", "..", "vfspath.tmp/sub",
    "vfspath.tmp/data", "vfspath.tmp/sub/.", "vfspath.tmp/sub/..",
    "vfspath.tmp/missing/child", "vfspath.tmp/data/child",
  };
  int fd;

  if(stat("vfspath.tmp", &before) < 0)
    fail("parent stat");
  for(uint i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
    check_node_failure(bad[i]);

  if(mkdir("///vfspath.tmp//tree///") < 0 ||
     stat("vfspath.tmp", &after) < 0 || after.nlink != before.nlink + 1 ||
     stat("vfspath.tmp/tree", &tree) < 0 || tree.type != T_DIR ||
     tree.nlink != 1)
    fail("mkdir parent link count");
  check_same_dir("vfspath.tmp/tree", "vfspath.tmp/tree/.");
  check_same_dir("vfspath.tmp/tree/..", "vfspath.tmp");
  if(chdir("vfspath.tmp/tree") < 0 || mkdir("./leaf///") < 0)
    fail("relative mkdir");
  check_same_dir("leaf/..", ".");
  if(chdir("/") < 0 || stat("vfspath.tmp/tree", &after) < 0 ||
     after.nlink != tree.nlink + 1)
    fail("nested directory link count");
  if(unlink("vfspath.tmp/tree") != -1)
    fail("removed nonempty directory");
  if(unlink("vfspath.tmp/tree/leaf") < 0 ||
     stat("vfspath.tmp/tree", &after) < 0 || after.nlink != tree.nlink ||
     unlink("vfspath.tmp/tree") < 0)
    fail("directory cleanup");

  if(mkdir("vfspath.tmp/abcdefghijklmnO") < 0)
    fail("long directory name");
  check_same_dir("vfspath.tmp/abcdefghijklmnO", "vfspath.tmp/abcdefghijklmn");
  check_node_failure("vfspath.tmp/abcdefghijklmnSuffix");
  if(unlink("vfspath.tmp/abcdefghijklmn") < 0)
    fail("long directory cleanup");

  if(mknod("vfspath.tmp/testdev", 1, 0) < 0)
    fail("mknod testdev");
  check_node_failure("vfspath.tmp/testdev");
  if(stat("vfspath.tmp/testdev", &after) < 0 || after.type != T_DEVICE ||
     unlink("vfspath.tmp/testdev") < 0)
    fail("device type or cleanup");
  // rev5 permits creating this inode; open rejects its invalid stored major.
  if(mknod("vfspath.tmp/baddev", NDEV, 0) < 0)
    fail("mknod invalid-major compatibility");
  fd = open("vfspath.tmp/baddev", O_RDONLY);
  if(fd >= 0){
    close(fd);
    fail("open invalid major");
  }
  if(unlink("vfspath.tmp/baddev") < 0 ||
     stat("vfspath.tmp", &after) < 0 || after.nlink != before.nlink)
    fail("parent link count after cleanup");

  check_concurrent_nodes(0);
  check_concurrent_nodes(1);
}

static void
check_creation(struct stat *data, struct stat *longname)
{
  struct stat before, after;
  char byte;
  int fd;
  const char *bad[] = {
    "", "/", "////", ".", "..", "vfspath.tmp/sub",
    "vfspath.tmp/sub/.", "vfspath.tmp/sub/..",
    "vfspath.tmp/data/child", "vfspath.tmp/missing/child",
  };

  check_create_existing("vfspath.tmp/sub/../data", data);
  check_create_existing("vfspath.tmp/12345678901234suffix", longname);
  for(uint i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
    check_create_failure(bad[i]);

  create_file("///vfspath.tmp//sub/../new///");
  if(stat("vfspath.tmp/new", &before) < 0)
    fail("new stat");
  check_file("vfspath.tmp/new", &before);
  fd = open("vfspath.tmp/sub/../new", O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0 || fstat(fd, &after) < 0 ||
     before.ino != after.ino || after.size != 0 || read(fd, &byte, 1) != 0)
    fail("O_CREATE O_TRUNC");
  if(write(fd, "Z", 1) != 1)
    fail("write after truncate");
  close(fd);
  fd = open("vfspath.tmp/new", O_RDONLY);
  if(fd < 0 || read(fd, &byte, 1) != 1 || byte != 'Z' ||
     read(fd, &byte, 1) != 0)
    fail("truncate content");
  close(fd);
  if(unlink("vfspath.tmp/new") < 0)
    fail("truncate cleanup");

  // Preserve native O_CREATE on an existing device; do not perform device I/O.
  if(mknod("vfspath.tmp/device", 1, 0) < 0)
    fail("device fixture");
  fd = open("vfspath.tmp/device", O_CREATE | O_RDWR);
  if(fd < 0 || fstat(fd, &after) < 0 || after.type != T_DEVICE)
    fail("existing device");
  close(fd);
  if(unlink("vfspath.tmp/device") < 0)
    fail("device cleanup");

  check_concurrent_create();

  // A removed cwd is still referenced, but must not accept new children.
  if(mkdir("vfspath.tmp/gone") < 0 || chdir("vfspath.tmp/gone") < 0 ||
     unlink("/vfspath.tmp/gone") < 0)
    fail("removed parent fixture");
  check_create_failure("lost");
  check_node_failure("lostnode");
  if(chdir("/") < 0)
    fail("leave removed parent");
}

int
main(void)
{
  struct stat data, longname, cycle, parent_before, parent_after;
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
  create_file("../relative");
  if(stat("../relative", &cycle) < 0)
    fail("relative create stat");
  check_file("/vfspath.tmp/relative", &cycle);
  if(unlink("../relative") < 0)
    fail("relative cleanup");
  if(chdir("/") < 0)
    fail("chdir back");

  check_directories();
  check_creation(&data, &longname);

  if(stat("vfspath.tmp", &parent_before) < 0)
    fail("parent before cycles");
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
    if(mkdir("vfspath.tmp/cycledir") < 0 ||
       mknod("vfspath.tmp/cycledev", 1, 0) < 0 ||
       unlink("vfspath.tmp/cycledev") < 0 ||
       unlink("vfspath.tmp/cycledir") < 0)
      fail("directory/device cycle");
  }

  if(stat("vfspath.tmp", &parent_after) < 0 ||
     parent_after.nlink != parent_before.nlink)
    fail("parent links leaked during cycles");
  if(unlink("vfspath.tmp/data") < 0 ||
     unlink("vfspath.tmp/12345678901234") < 0 ||
     unlink("vfspath.tmp/sub") < 0 || unlink("vfspath.tmp") < 0)
    fail("cleanup");
  printf("vfspath: ALL TESTS PASSED\n");
  exit(0);
}
