#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char buffer[512];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "fsjoint: FAIL: %s\n", what);
    exit(1);
  }
}

static void
copy(const char *source, const char *target)
{
  int in = open(source, O_RDONLY);
  int out = open(target, O_CREATE | O_WRONLY | O_TRUNC), n;
  check(in >= 0 && out >= 0, "copy open");
  while((n = read(in, buffer, sizeof(buffer))) > 0)
    check(write(out, buffer, n) == n, "copy write");
  check(n == 0, "copy read");
  close(in); close(out);
}

static void
verify_pattern(const char *path)
{
  struct stat st;
  int fd = open(path, O_RDONLY), pos = 0, n;
  check(fd >= 0 && fstat(fd, &st) == 0 && st.size == 13000, "pattern file size");
  while((n = read(fd, buffer, sizeof(buffer))) > 0){
    for(int i = 0; i < n; i++, pos++){
      char expected = pos < 3 ? "XYZ"[pos] : pos % 251;
      check(buffer[i] == expected, "pattern content");
    }
  }
  check(n == 0 && pos == 13000, "pattern EOF");
  close(fd);
}

static void
verify_text(const char *path, const char *expected)
{
  int len = strlen(expected), fd = open(path, O_RDONLY);
  struct stat st;
  check(fd >= 0 && fstat(fd, &st) == 0 && st.size == len &&
        read(fd, buffer, len) == len && memcmp(buffer, expected, len) == 0,
        "text readback");
  close(fd);
}

int
main(int argc, char **argv)
{
  struct stat a, b;
  int reboot = argc == 2 && strcmp(argv[1], "verify") == 0;
  check(stat("/", &a) == 0 && a.dev == 1 &&
        stat("/mnt", &a) == 0 && a.dev == 2 &&
        stat("/fat", &a) == 0 && a.dev == 3, "three distinct devices");
  if(!reboot){
    copy("/mnt/keep-a", "/joint-native");
    copy("/joint-native", "/fat/xcopy.bin");
    copy("/fat/xcopy.bin", "/mnt/roundtrip.bin");
    check(link("/mnt/keep-a", "/fat/denied") < 0 &&
          link("/fat/xcopy.bin", "/mnt/denied") < 0 &&
          stat("/fat/denied", &a) < 0 && stat("/mnt/denied", &a) < 0,
          "cross-filesystem hard links refused");
  }
  verify_pattern("/joint-native");
  verify_pattern("/mnt/roundtrip.bin");
  verify_pattern("/fat/xcopy.bin");
  verify_pattern("/mnt/keep-a");
  verify_pattern("/mnt/seed/keep-b");
  verify_pattern("/mnt/tree/inner/data.bin");
  check(stat("/mnt/keep-a", &a) == 0 && stat("/mnt/seed/keep-b", &b) == 0 &&
        a.ino == b.ino && a.nlink == 2 && b.nlink == 2, "persistent hard links");
  verify_text("/mnt/seed/keep.txt", "seed unchanged\n");
  verify_text("/mnt/seed/new.txt", "new in imported directory\n");
  verify_text("/mnt/tree/short", "ok\n");
  verify_text("/fat/Report 初稿 🧪.txt", "kept");
  verify_text("/fat/Exactly13.txt", "exact");
  verify_text("/fat/Shared file.txt", "new");
  check(stat("/mnt/remove", &a) < 0 && stat("/mnt/links", &a) < 0 &&
        stat("/fat/Report 终稿 🧪.txt", &a) < 0 &&
        stat("/fat/目录 Long 🧪", &a) < 0, "deletions remain absent");
  printf("fsjoint: %s\n", reboot ? "REBOOT PASS" : "PASS");
  exit(0);
}
