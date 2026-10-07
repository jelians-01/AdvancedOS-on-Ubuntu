#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/elf.h"
#include "user/user.h"

static volatile uint initialized = 0x13579bdf;
static volatile char zeroed[8192];

static void
fail(const char *what)
{
  fprintf(2, "vfsexec: FAIL: %s\n", what);
  exit(1);
}

static void
check_image(void)
{
  if(initialized != 0x13579bdf)
    fail("initialized data");
  for(uint i = 0; i < sizeof(zeroed); i++)
    if(zeroed[i] != 0)
      fail("BSS not zero");
  zeroed[sizeof(zeroed)-1] = 7;
  if(zeroed[sizeof(zeroed)-1] != 7)
    fail("BSS not writable");
}

// The new image writes one byte through stdout inherited across exec.
// It also verifies argv, cwd, initialized data and zero-filled memory.
static void
probe(int argc, char **argv)
{
  struct stat here, expected;

  if(argc != 4 || strcmp(argv[0], "exec-probe") != 0 ||
     strcmp(argv[1], "--probe") != 0 || strcmp(argv[2], "arg with spaces") != 0)
    fail("exec arguments");
  check_image();
  if(stat(".", &here) < 0 || stat(argv[3], &expected) < 0 ||
     here.type != T_DIR || expected.type != T_DIR ||
     here.dev != expected.dev || here.ino != expected.ino)
    fail("exec changed cwd");
  if(write(1, "E", 1) != 1)
    fail("inherited descriptor");
  exit(0);
}

static void
run_image(const char *path, char *cwd)
{
  int report[2], pid, status;
  char byte;
  char *args[] = { "exec-probe", "--probe", "arg with spaces", cwd, 0 };

  if(pipe(report) < 0)
    fail("exec pipe");
  pid = fork();
  if(pid < 0)
    fail("exec fork");
  if(pid == 0){
    close(report[0]);
    if(chdir(cwd) < 0)
      fail("child chdir");
    close(1);
    if(dup(report[1]) != 1)
      fail("redirect stdout");
    close(report[1]);
    exec(path, args);
    fail("exec returned");
  }
  close(report[1]);
  if(read(report[0], &byte, 1) != 1 || byte != 'E' ||
     read(report[0], &byte, 1) != 0)
    fail("new image did not report");
  close(report[0]);
  if(wait(&status) != pid || status != 0)
    fail("exec child status");
}

static void
copy_program(const char *dst)
{
  char buf[512];
  int src = open("/vfsexec", O_RDONLY);
  int out = open(dst, O_CREATE | O_WRONLY | O_TRUNC);
  int n;

  if(src < 0 || out < 0)
    fail("copy open");
  while((n = read(src, buf, sizeof(buf))) > 0)
    if(write(out, buf, n) != n)
      fail("copy write");
  if(n < 0)
    fail("copy read");
  close(src);
  close(out);
}

static void
expect_failure(const char *path)
{
  // If a bad exec unexpectedly replaces this process, probe rejects its argv.
  char *args[] = { "unexpected-exec", "--unexpected", 0 };

  if(exec(path, args) != -1)
    fail("bad exec did not fail");
  if(initialized != 0x13579bdf)
    fail("failed exec changed old image");
}

static void
make_bad_image(int mode)
{
  struct elfhdr elf;
  struct proghdr ph, second;
  int fd = open("vfsexec.tmp/bad", O_CREATE | O_WRONLY | O_TRUNC);

  if(fd < 0)
    fail("bad image open");
  memset(&elf, 0, sizeof(elf));
  memset(&ph, 0, sizeof(ph));
  elf.magic = ELF_MAGIC;
  elf.phoff = sizeof(elf);
  elf.phentsize = sizeof(ph);
  elf.phnum = 1;
  ph.type = ELF_PROG_LOAD;
  ph.flags = ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC;
  ph.off = sizeof(elf) + sizeof(ph);
  ph.filesz = 1;
  ph.memsz = 4096;

  switch(mode){
  case 0: // Short ELF header.
    if(write(fd, &elf, sizeof(elf)-1) != sizeof(elf)-1)
      fail("short header write");
    close(fd);
    return;
  case 1: elf.magic = 0; break;
  case 2: // Program header table is absent.
    if(write(fd, &elf, sizeof(elf)) != sizeof(elf))
      fail("header write");
    close(fd);
    return;
  case 3: elf.phoff += 1ULL << 32; break;
  case 4: ph.off += 1ULL << 32; break;
  case 5: ph.filesz = 2; break; // Only one payload byte exists.
  case 6: ph.filesz = ph.memsz = (1ULL << 32) + 1; break;
  case 7: ph.memsz = 0; break;
  case 8: ph.vaddr = 1; break;
  case 9: ph.vaddr = 0xfffffffffffff000ULL; ph.memsz = 8192; break;
  case 10:
    // First segment loads; the second fails after a page has been allocated.
    elf.phnum = 2;
    ph.off += sizeof(ph);
    second = ph;
    second.vaddr = 4096;
    second.off = 0xffffffffU;
    break;
  default:
    fail("unknown bad image mode");
  }
  if(write(fd, &elf, sizeof(elf)) != sizeof(elf) ||
     write(fd, &ph, sizeof(ph)) != sizeof(ph))
    fail("bad headers write");
  if(mode == 10 && write(fd, &second, sizeof(second)) != sizeof(second))
    fail("second header write");
  if(write(fd, "X", 1) != 1)
    fail("bad payload write");
  close(fd);
}

static void
check_failures(void)
{
  int pid = fork(), status;

  if(pid < 0)
    fail("failure test fork");
  if(pid == 0){
    expect_failure("");
    expect_failure("/");
    expect_failure("vfsexec.tmp");
    expect_failure("vfsexec.tmp/missing");
    expect_failure("/vfsexec/child");
    if(mknod("vfsexec.tmp/device", 1, 0) < 0)
      fail("device fixture");
    expect_failure("vfsexec.tmp/device");
    if(unlink("vfsexec.tmp/device") < 0)
      fail("device cleanup");
    // More than NINODE distinct lifetimes: failed exec must release its node.
    for(int i = 0; i < 200; i++){
      make_bad_image(i % 11);
      expect_failure("vfsexec.tmp/bad");
      if(unlink("vfsexec.tmp/bad") < 0)
        fail("bad image cleanup");
    }
    exit(0);
  }
  if(wait(&status) != pid || status != 0)
    fail("failure test child");
}

int
main(int argc, char **argv)
{
  struct stat a, b;

  if(argc > 1)
    probe(argc, argv);
  check_image();
  if(chdir("/") < 0 || mkdir("vfsexec.tmp") < 0)
    fail("mkdir vfsexec.tmp (inspect any existing directory)");

  run_image("/vfsexec", "/");
  run_image("../vfsexec", "/vfsexec.tmp");
  run_image("///vfsexec.tmp//../vfsexec///", "/");

  copy_program("vfsexec.tmp/program");
  if(link("vfsexec.tmp/program", "vfsexec.tmp/alias") < 0 ||
     stat("vfsexec.tmp/program", &a) < 0 ||
     stat("vfsexec.tmp/alias", &b) < 0 || a.dev != b.dev || a.ino != b.ino ||
     unlink("vfsexec.tmp/program") < 0)
    fail("executable alias fixture");
  run_image("./alias", "/vfsexec.tmp");
  if(link("vfsexec.tmp/alias", "vfsexec.tmp/123456789012345") < 0)
    fail("long executable name");
  run_image("/vfsexec.tmp/12345678901234suffix", "/");
  check_failures();
  // Success after repeated failures also checks that VFS references recover.
  run_image("alias", "/vfsexec.tmp");

  if(unlink("vfsexec.tmp/alias") < 0 ||
     unlink("vfsexec.tmp/12345678901234") < 0 ||
     unlink("vfsexec.tmp") < 0)
    fail("cleanup");
  printf("vfsexec: ALL TESTS PASSED\n");
  exit(0);
}
