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
check_link_failure(const char *old, const char *new)
{
  struct stat before, after;

  if(stat(old, &before) < 0 || link(old, new) != -1 ||
     stat(old, &after) < 0 || after.nlink != before.nlink ||
     after.ino != before.ino || after.size != before.size)
    fail("failed link changed source");
}

static void
check_concurrent_links(void)
{
  const char *src = "vfspath.tmp/linksrc";
  const char *dst = "vfspath.tmp/linkrace";
  struct stat st, linked;
  int gate[2], status;
  char byte;

  create_file(src);
  // First four links race for one name, then four unlinks race to remove it.
  for(int remove = 0; remove < 2; remove++){
    int successes = 0;
    if(pipe(gate) < 0)
      fail("link race pipe");
    for(int i = 0; i < 4; i++){
      int pid = fork();
      if(pid < 0)
        fail("link race fork");
      if(pid == 0){
        int r;
        close(gate[1]);
        if(read(gate[0], &byte, 1) != 1)
          exit(2);
        close(gate[0]);
        r = remove ? unlink(dst) : link(src, dst);
        exit(r == 0 ? 0 : (r == -1 ? 1 : 2));
      }
    }
    close(gate[0]);
    if(write(gate[1], "xxxx", 4) != 4)
      fail("link race start");
    close(gate[1]);
    for(int i = 0; i < 4; i++){
      if(wait(&status) < 0 || (status != 0 && status != 1))
        fail("link race child");
      successes += status == 0;
    }
    if(successes != 1 || stat(src, &st) < 0 ||
       st.nlink != (remove ? 1 : 2))
      fail("link race count");
    if(!remove){
      if(stat(dst, &linked) < 0 || linked.dev != st.dev ||
         linked.ino != st.ino || linked.nlink != 2)
        fail("link race identity");
      check_file(dst, &st);
    } else {
      check_missing(dst);
    }
  }
  if(unlink(src) < 0)
    fail("link race cleanup");
}

static void
check_last_unlink_race(void)
{
  const char *src = "vfspath.tmp/lastsrc";
  const char *dst = "vfspath.tmp/lastdst";
  struct stat before, after;
  int gate[2], fd, pid, status;
  char byte;

  // Hold the old inode open while racing its last name against a new link.
  for(int i = 0; i < 16; i++){
    create_file(src);
    fd = open(src, O_RDONLY);
    if(fd < 0 || fstat(fd, &before) < 0 || pipe(gate) < 0)
      fail("last unlink setup");
    pid = fork();
    if(pid < 0)
      fail("last unlink fork");
    if(pid == 0){
      int r;
      close(fd);
      close(gate[1]);
      if(read(gate[0], &byte, 1) != 1)
        exit(2);
      close(gate[0]);
      r = link(src, dst);
      exit(r == 0 ? 0 : (r == -1 ? 1 : 2));
    }
    close(gate[0]);
    if(write(gate[1], "x", 1) != 1)
      fail("last unlink start");
    close(gate[1]);
    if(unlink(src) < 0 || wait(&status) < 0 ||
       (status != 0 && status != 1))
      fail("last unlink child");
    check_missing(src);
    if(fstat(fd, &after) < 0 || after.ino != before.ino ||
       after.nlink != (status == 0 ? 1 : 0))
      fail("last unlink count");
    if(status == 0){
      check_file(dst, &before);
      if(unlink(dst) < 0)
        fail("last link cleanup");
    } else {
      check_missing(dst);
    }
    if(fstat(fd, &after) < 0 || after.nlink != 0)
      fail("last unlink final count");
    close(fd);
  }
}

static void
check_links(void)
{
  const char *src = "vfspath.tmp/linkfile";
  const char *dst = "vfspath.tmp/sub/alias";
  const char *bad[] = {
    "", "/", "////", ".", "..", "vfspath.tmp/sub/.",
    "vfspath.tmp/sub/..", "vfspath.tmp/missing/name",
    "vfspath.tmp/data/name", "vfspath.tmp/sub", "vfspath.tmp/data",
  };
  struct stat original, st, fresh;
  char buf[sizeof(payload)+1];
  int fd, rd, shared, pid, status;

  create_file(src);
  if(stat(src, &original) < 0 || original.nlink != 1 ||
     chdir("vfspath.tmp/sub") < 0 || link("../linkfile", "./alias///") < 0 ||
     chdir("/") < 0 || stat(dst, &st) < 0 || st.nlink != 2 ||
     st.dev != original.dev || st.ino != original.ino ||
     stat(src, &st) < 0 || st.nlink != 2)
    fail("link identity or count");
  check_file(dst, &original);
  check_link_failure(src, src);
  check_link_failure(src, dst);
  check_link_failure("vfspath.tmp/sub", "vfspath.tmp/dirlink");
  check_missing("vfspath.tmp/dirlink");
  for(uint i = 0; i < sizeof(bad)/sizeof(bad[0]); i++){
    check_link_failure(src, bad[i]);
    if(strcmp(bad[i], "vfspath.tmp/data") != 0 && unlink(bad[i]) != -1)
      fail("unlink invalid path");
  }

  if(link("vfspath.tmp/missing", "vfspath.tmp/newlink") != -1)
    fail("link missing source");
  check_missing("vfspath.tmp/newlink");
  if(link(src, "vfspath.tmp/abcdefghijklmnExtra") < 0)
    fail("long link name");
  check_file("vfspath.tmp/abcdefghijklmn", &original);
  check_link_failure(src, "vfspath.tmp/abcdefghijklmnOther");
  if(unlink("vfspath.tmp/abcdefghijklmnSuffix///") < 0 ||
     stat(src, &st) < 0 || st.nlink != 2)
    fail("long link cleanup");

  fd = open(src, O_RDWR);
  rd = open(dst, O_RDONLY);
  shared = dup(fd);
  if(fd < 0 || rd < 0 || shared < 0 ||
     unlink(src) < 0 || fstat(fd, &st) < 0 || st.nlink != 1 ||
     unlink(dst) < 0 || fstat(fd, &st) < 0 || st.nlink != 0)
    fail("open unlink");
  check_missing(src);
  check_missing(dst);
  if(write(fd, "Z", 1) != 1)
    fail("write unlinked file");
  close(fd);
  if(read(shared, buf, sizeof(buf)) != sizeof(payload)-2 ||
     memcmp(buf, payload+1, sizeof(payload)-2) != 0)
    fail("dup kept unlinked file and offset");
  pid = fork();
  if(pid < 0)
    fail("unlinked file fork");
  if(pid == 0){
    if(fstat(shared, &st) < 0 || st.nlink != 0 ||
       write(shared, "!", 1) != 1)
      exit(1);
    close(shared);
    close(rd);
    exit(0);
  }
  if(wait(&status) < 0 || status != 0 ||
     read(rd, buf, sizeof(buf)) != sizeof(payload) ||
     buf[0] != 'Z' || memcmp(buf+1, payload+1, sizeof(payload)-2) != 0 ||
     buf[sizeof(payload)-1] != '!' || read(rd, buf, 1) != 0)
    fail("fork kept unlinked file and contents");
  create_file(src);
  if(stat(src, &fresh) < 0 || fresh.ino == original.ino ||
     fstat(shared, &st) < 0 || st.nlink != 0 || st.size != sizeof(payload))
    fail("recreated name reused a live unlinked inode");
  check_file(src, &fresh);
  close(shared);
  close(rd);
  if(unlink(src) < 0)
    fail("open unlink cleanup");

  if(mknod("vfspath.tmp/linkdev", 1, 0) < 0 ||
     link("vfspath.tmp/linkdev", "vfspath.tmp/devlink") < 0 ||
     stat("vfspath.tmp/linkdev", &original) < 0 ||
     stat("vfspath.tmp/devlink", &st) < 0 || st.type != T_DEVICE ||
     st.dev != original.dev || st.ino != original.ino || st.nlink != 2 ||
     unlink("vfspath.tmp/linkdev") < 0 ||
     stat("vfspath.tmp/devlink", &st) < 0 || st.nlink != 1 ||
     unlink("vfspath.tmp/devlink") < 0)
    fail("device hard link");

  check_concurrent_links();
  check_last_unlink_race();
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
  check_link_failure("/vfspath.tmp/data", "lostlink");
  if(chdir("/") < 0)
    fail("leave removed parent");
}

static void
check_chdir_failure(const char *path)
{
  struct stat before, after;

  if(stat(".", &before) < 0 || chdir(path) != -1 ||
     stat(".", &after) < 0 || before.dev != after.dev ||
     before.ino != after.ino)
    fail("failed chdir changed cwd");
}

static void
check_cwd_fork(void)
{
  int ready[2], go[2], pid, status;
  char byte;

  if(mkdir("vfspath.tmp/cwdleft") < 0 ||
     mkdir("vfspath.tmp/cwdright") < 0 ||
     chdir("vfspath.tmp/cwdleft") < 0 ||
     pipe(ready) < 0 || pipe(go) < 0)
    fail("cwd fork setup");
  pid = fork();
  if(pid < 0)
    fail("cwd fork");
  if(pid == 0){
    close(ready[0]);
    close(go[1]);
    check_same_dir(".", "/vfspath.tmp/cwdleft");
    if(write(ready[1], "R", 1) != 1)
      fail("cwd ready");
    close(ready[1]);
    if(read(go[0], &byte, 1) != 1)
      fail("cwd go");
    close(go[0]);
    // Parent has changed cwd, but this inherited reference still names left.
    check_same_dir(".", "/vfspath.tmp/cwdleft");
    create_file("childfile");
    if(chdir("..") < 0)
      fail("child chdir");
    check_same_dir(".", "/vfspath.tmp");
    exit(0);
  }
  close(ready[1]);
  close(go[0]);
  if(read(ready[0], &byte, 1) != 1 || byte != 'R')
    fail("cwd child ready");
  close(ready[0]);
  if(chdir("../cwdright") < 0)
    fail("parent chdir");
  create_file("parentfile");
  if(write(go[1], "G", 1) != 1)
    fail("cwd release child");
  close(go[1]);
  if(wait(&status) != pid || status != 0)
    fail("cwd child status");
  // Child's chdir must not change the parent's pointer.
  check_same_dir(".", "/vfspath.tmp/cwdright");
  check_missing("childfile");
  check_missing("../cwdleft/parentfile");
  struct stat st;
  if(stat("../cwdleft/childfile", &st) < 0)
    fail("child relative create");
  check_file("../cwdleft/childfile", &st);
  if(stat("parentfile", &st) < 0)
    fail("parent relative create");
  check_file("parentfile", &st);
  if(chdir("/") < 0 || unlink("vfspath.tmp/cwdleft/childfile") < 0 ||
     unlink("vfspath.tmp/cwdright/parentfile") < 0 ||
     unlink("vfspath.tmp/cwdleft") < 0 || unlink("vfspath.tmp/cwdright") < 0)
    fail("cwd fork cleanup");
}

static void
check_removed_cwd_exit(void)
{
  const char *path = "/vfspath.tmp/cwdgone";
  struct stat old, fresh, held;
  int gate[2], pid, status;
  char byte;

  // More than NINODE distinct removed directories; child exit owns the last
  // cwd reference after the parent moves away and reuses the pathname.
  for(int i = 0; i < 80; i++){
    if(mkdir(path) < 0 || chdir(path) < 0 ||
       stat(".", &old) < 0 || pipe(gate) < 0)
      fail("removed cwd setup");
    pid = fork();
    if(pid < 0)
      fail("removed cwd fork");
    if(pid == 0){
      close(gate[1]);
      if(read(gate[0], &byte, 1) != 1)
        fail("removed cwd gate");
      close(gate[0]);
      if(stat(".", &held) < 0 || held.dev != old.dev ||
         held.ino != old.ino || held.type != T_DIR || held.nlink != 0)
        fail("inherited removed cwd identity");
      // Empty path and "." retain rev5 behavior even for a removed cwd.
      if(chdir("") < 0 || chdir(".") < 0)
        fail("removed cwd self chdir");
      check_chdir_failure("missing");
      check_create_failure("lost");
      check_node_failure("lostnode");
      check_link_failure("/vfspath.tmp/data", "lostlink");
      if(stat(path, &fresh) < 0 || fresh.ino == old.ino || fresh.nlink != 1)
        fail("removed cwd followed reused pathname");
      // Do not chdir away: exit must release and reclaim the old directory.
      exit(0);
    }
    close(gate[0]);
    if(unlink(path) < 0 || chdir("/") < 0 || mkdir(path) < 0 ||
       stat(path, &fresh) < 0 || fresh.ino == old.ino)
      fail("recreate name with inherited cwd alive");
    if(write(gate[1], "G", 1) != 1)
      fail("removed cwd start");
    close(gate[1]);
    if(wait(&status) != pid || status != 0 || unlink(path) < 0)
      fail("removed cwd exit cleanup");
  }
}

static void
check_cwd(void)
{
  struct stat before, after;
  const char *bad[] = {
    "/vfspath.tmp/missing", "/vfspath.tmp/data", "/vfspath.tmp/data/child",
    "/vfspath.tmp/cwddev",
  };

  if(stat("vfspath.tmp", &before) < 0 ||
     mknod("vfspath.tmp/cwddev", 1, 0) < 0 ||
     chdir("vfspath.tmp/sub") < 0)
    fail("cwd checks setup");
  for(uint i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
    check_chdir_failure(bad[i]);
  check_chdir_failure((const char *)~(uint64)0);
  for(int i = 0; i < 200; i++){
    if(chdir("") < 0 || chdir("./") < 0 ||
       chdir("../sub///") < 0 || chdir("/vfspath.tmp/sub") < 0)
      fail("cwd reference cycle");
    check_same_dir(".", "/vfspath.tmp/sub");
    check_chdir_failure("../data");
  }
  if(chdir("/") < 0 || chdir("..") < 0)
    fail("root chdir");
  check_same_dir(".", "/");
  if(unlink("vfspath.tmp/cwddev") < 0)
    fail("cwd device cleanup");
  if(mkdir("vfspath.tmp/cwdparent") < 0 ||
     chdir("vfspath.tmp/cwdparent") < 0 ||
     unlink("/vfspath.tmp/cwdparent") < 0 || chdir("..") < 0)
    fail("leave removed cwd through parent");
  check_same_dir(".", "/vfspath.tmp");
  if(chdir("/") < 0)
    fail("cwd return to root");
  check_cwd_fork();
  check_removed_cwd_exit();
  if(stat("vfspath.tmp", &after) < 0 || after.nlink != before.nlink)
    fail("cwd tests leaked directory links");
}

// Device nodes keep their identity through unlink, dup and fork.
// Zero-byte console I/O exercises dispatch without waiting for keyboard input.
static void
check_open_devices(void)
{
  const char *path = "vfspath.tmp/opendev";
  struct stat old, st;
  char byte;
  int fd, other, gate[2], pid, status;

  if(mknod(path, 1, 0) < 0)
    fail("open device setup");
  fd = open(path, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0 || fstat(fd, &old) < 0 || old.type != T_DEVICE ||
     read(fd, &byte, 0) != 0 || write(fd, &byte, 0) != 0 ||
     read(fd, &byte, -1) != -1 || write(fd, &byte, -1) != -1)
    fail("device metadata or I/O");
  other = dup(fd);
  if(other < 0 || pipe(gate) < 0)
    fail("device dup or gate");
  pid = fork();
  if(pid < 0)
    fail("device fork");
  if(pid == 0){
    close(gate[1]);
    close(fd);
    if(read(gate[0], &byte, 1) != 1)
      fail("device gate read");
    close(gate[0]);
    if(fstat(other, &st) < 0 || st.type != T_DEVICE ||
       st.dev != old.dev || st.ino != old.ino || st.nlink != 0 ||
       read(other, &byte, 0) != 0 || write(other, &byte, 0) != 0)
      fail("inherited unlinked device");
    // exit must release the last reference to the old device.
    exit(0);
  }
  close(gate[0]);
  if(unlink(path) < 0 || mknod(path, 1, 0) < 0 ||
     stat(path, &st) < 0 || st.ino == old.ino)
    fail("device name reuse");
  close(fd);
  close(other);
  if(write(gate[1], "G", 1) != 1)
    fail("device gate write");
  close(gate[1]);
  if(wait(&status) != pid || status != 0 || unlink(path) < 0)
    fail("device child cleanup");

  // Each iteration creates fresh node lifetimes, exceeding the inode table.
  for(int i = 0; i < 80; i++){
    if(mknod(path, 1, 0) < 0)
      fail("device cycle setup");
    fd = open(path, O_RDONLY);
    other = open(path, O_WRONLY);
    if(fd < 0 || other < 0 || unlink(path) < 0 ||
       fstat(fd, &st) < 0 || st.type != T_DEVICE || st.nlink != 0 ||
       read(fd, &byte, 0) != 0 || write(other, &byte, 0) != 0 ||
       write(fd, &byte, 0) != -1 || read(other, &byte, 0) != -1)
      fail("device cycle permissions or lifetime");
    close(fd);
    close(other);

    // Native mknod permits these majors, but open must reject them.
    if(mknod(path, NDEV, 0) < 0)
      fail("invalid device setup");
    check_missing(path);
    check_create_failure(path);
    if(unlink(path) < 0 || mknod(path, -1, 0) < 0)
      fail("negative device setup");
    check_missing(path);
    if(unlink(path) < 0)
      fail("invalid device cleanup");
  }

  // Major zero is in range but has no registered driver in this kernel.
  if(mknod(path, 0, 0) < 0)
    fail("unregistered device setup");
  fd = open(path, O_RDWR);
  if(fd < 0 || fstat(fd, &st) < 0 || st.type != T_DEVICE ||
     read(fd, &byte, 0) != -1 || write(fd, &byte, 0) != -1)
    fail("unregistered device dispatch");
  close(fd);
  if(unlink(path) < 0)
    fail("unregistered device cleanup");
}

static void
check_open_exhaustion(void)
{
  const char *source = "vfspath.tmp/fullsrc";
  const char *file = "vfspath.tmp/fullfile";
  const char *device = "vfspath.tmp/fulldev";
  struct stat st;
  int held[NOFILE], count = 0, fd;

  create_file(source);
  if(stat(source, &st) < 0)
    fail("fd exhaustion setup");
  while(count < NOFILE){
    fd = open(source, O_RDONLY);
    if(fd < 0)
      break;
    held[count++] = fd;
  }
  if(count == 0 || count == NOFILE)
    fail("fd exhaustion precondition");
  fd = open(source, O_WRONLY | O_TRUNC);
  if(fd >= 0)
    fail("truncate succeeded with full fd table");

  for(int i = 0; i < 80; i++){
    // Creation precedes fd allocation, so the new name must exist even
    // though open fails. Unlink reclaims it only if open dropped its ref.
    check_create_failure(file);
    if(unlink(file) < 0 || mknod(device, 1, 0) < 0)
      fail("full fd table creation");
    check_missing(device);
    if(unlink(device) < 0)
      fail("full fd table device cleanup");
  }
  while(count > 0)
    close(held[--count]);
  check_file(source, &st);
  if(unlink(source) < 0)
    fail("fd exhaustion cleanup");
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
  check_links();
  check_cwd();
  check_open_devices();
  check_open_exhaustion();

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
    if(link("vfspath.tmp/cycle", "vfspath.tmp/cyclelink") < 0 ||
       stat("vfspath.tmp/cycle", &cycle) < 0 || cycle.nlink != 2 ||
       unlink("vfspath.tmp/cycle") < 0)
      fail("cycle link/unlink");
    check_file("vfspath.tmp/cyclelink", &cycle);
    if(stat("vfspath.tmp/cyclelink", &cycle) < 0 || cycle.nlink != 1 ||
       unlink("vfspath.tmp/cyclelink") < 0)
      fail("cycle last link");
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
