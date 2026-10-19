// Memory-only mount regression tests. No disk or production mount is changed.
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "vfs.h"
#include "vfs_dirent.h"
#include "vfs_backend.h"

#define MT_NODES 7

struct mt_fs;
struct mt_node {
  struct mt_fs *fs;
  const char *name;
  int parent;
  int refs;
  int locked;
  short type;
  short nlink;
};

struct mt_fs {
  uint dev;
  int released;
  int unlinks;
  int links;
  int readmode;
  struct mt_node nodes[MT_NODES];
};

static struct mt_fs mt_states[4];

static void
mt_lock(void *data)
{
  struct mt_node *n = data;
  if(n->refs < 1 || n->locked || n->fs->released)
    panic("mount test lock");
  n->locked = 1;
}

static void
mt_unlock(void *data)
{
  struct mt_node *n = data;
  if(!n->locked)
    panic("mount test unlock");
  n->locked = 0;
}

static int
mt_read(void *data, int user_dst, uint64 dst, uint off, uint count)
{
  struct mt_node *n = data;
  if(!n->locked || user_dst || n->type != T_FILE)
    return -1;
  if(count == 0 || off >= 1)
    return 0;
  *(char *)dst = (char)n->fs->dev;
  return 1;
}

static int
mt_getattr(void *data, struct stat *st)
{
  struct mt_node *n = data;
  if(!n->locked)
    panic("mount test getattr");
  memset(st, 0, sizeof(*st));
  st->dev = n->fs->dev;
  st->ino = n - n->fs->nodes + 1;
  st->type = n->type;
  st->nlink = n->nlink;
  st->size = n->type == T_FILE ? 1 : 0;
  return 0;
}

static void
mt_node_put(void *data)
{
  struct mt_node *n = data;
  if(n->refs < 1 || n->locked || n->fs->released)
    panic("mount test node put");
  n->refs--;
}

static struct mt_node*
mt_find(struct mt_node *dir, const char *name, uint len)
{
  if(dir->type != T_DIR || dir->nlink == 0)
    return 0;
  if(len == 1 && name[0] == '.')
    return dir;
  if(len == 2 && name[0] == '.' && name[1] == '.')
    return &dir->fs->nodes[dir->parent];
  for(int i = 1; i < MT_NODES; i++){
    struct mt_node *n = &dir->fs->nodes[i];
    if(n->nlink && n->parent == dir - dir->fs->nodes &&
       strlen(n->name) == len && strncmp(n->name, name, len) == 0)
      return n;
  }
  return 0;
}

static int
mt_lookup(void *data, const char *name, uint len, void **out)
{
  struct mt_node *n = mt_find(data, name, len);
  *out = 0;
  if(n == 0)
    return -1;
  n->refs++;
  *out = n;
  return 0;
}

// Use large opaque cookies to catch accidental narrowing to the read offset.
static int
mt_readdir(void *data, uint64 *cursor, struct vfs_dirent *out)
{
  struct mt_node *dir = data;
  uint64 tag = (uint64)1 << 40;
  int mode = dir->fs->readmode;
  uint64 first = *cursor == 0 ? 1 : *cursor - tag + 1;

  if(dir->type != T_DIR)
    return -1;
  if(mode){
    memset(out, 0, sizeof(*out));
    out->ino = 9;
    out->type = T_FILE;
    out->namelen = 1;
    out->name[0] = mode == 2 ? '/' : 'x';
    if(mode != 3)
      *cursor = tag;
    if(mode == 5)
      out->type = 99;
    if(mode == 6){
      out->namelen = VFS_NAME_MAX;
      memset(out->name, 'x', VFS_NAME_MAX);
    }
    return mode == 1 ? -1 : mode == 4 ? 0 : 1;
  }
  for(uint64 i = first; i < MT_NODES; i++){
    struct mt_node *n = &dir->fs->nodes[i];
    if(n->nlink && n->parent == dir - dir->fs->nodes){
      memset(out, 0, sizeof(*out));
      out->ino = i + 1;
      out->type = n->type;
      out->namelen = strlen(n->name);
      memmove(out->name, n->name, out->namelen);
      *cursor = tag + i;
      return 1;
    }
  }
  return 0;
}

static int
mt_unlink(void *data, const char *name, uint len)
{
  struct mt_node *dir = data, *n = mt_find(dir, name, len);
  if(n == 0)
    return -1;
  if(n->type == T_DIR)
    for(int i = 1; i < MT_NODES; i++)
      if(dir->fs->nodes[i].nlink &&
         dir->fs->nodes[i].parent == n - dir->fs->nodes)
        return -1;
  n->nlink = 0;
  dir->fs->unlinks++;
  return 0;
}

static int
mt_link(void *data, const char *name, uint len, void *target)
{
  struct mt_node *dir = data;
  dir->fs->links++;
  return -1; // A rejected cross-instance request must never arrive here.
}

static int
mt_root(void *data, void **out)
{
  struct mt_fs *fs = data;
  if(fs->released)
    panic("mount test root");
  fs->nodes[0].refs++;
  *out = &fs->nodes[0];
  return 0;
}

static void
mt_fs_put(void *data)
{
  struct mt_fs *fs = data;
  if(fs->released)
    panic("mount test double release");
  for(int i = 0; i < MT_NODES; i++)
    if(fs->nodes[i].refs || fs->nodes[i].locked)
      panic("mount test leaked node");
  fs->released = 1;
}

static const struct vnode_ops mt_node_ops = {
  .lock = mt_lock,
  .unlock = mt_unlock,
  .read_at = mt_read,
  .getattr = mt_getattr,
  .release = mt_node_put,
  .lookup = mt_lookup,
  .link = mt_link,
  .unlink = mt_unlink,
  .readdir = mt_readdir,
};

static const struct vfs_fs_ops mt_fs_ops = {
  .root = mt_root,
  .release = mt_fs_put,
};

static void
mt_node_init(int fs, int node, int parent, const char *name, short type)
{
  struct mt_node *n = &mt_states[fs].nodes[node];
  n->fs = &mt_states[fs];
  n->parent = parent;
  n->name = name;
  n->type = type;
  n->nlink = 1;
}

static void
mt_expect(const struct vfs_path *start, const char *name,
          struct vfs_mount *mnt, uint dev)
{
  struct vfs_path out = {0};
  struct stat st;
  if(vfs_path_walk(start, name, &out) < 0 || out.mnt != mnt)
    panic("mount test walk");
  vfs_lock(out.vn);
  int r = vfs_getattr(out.vn, &st);
  vfs_unlock(out.vn);
  if(r < 0 || st.dev != dev)
    panic("mount test device");
  vfs_path_put(&out);
}

static void
mt_directory(const struct vfs_path *mounted)
{
  struct vfs_dirent de, saved;
  uint64 cursor = 0;
  if(vfs_readdir(mounted->vn, &cursor, &de) != 1 ||
     cursor <= ((uint64)1 << 32) || strncmp(de.name, "visible", 8) ||
     de.type != T_FILE)
    panic("mount readdir first");
  if(vfs_readdir(mounted->vn, &cursor, &de) != 1 ||
     strncmp(de.name, "sub", 4) || de.type != T_DIR)
    panic("mount readdir next");
  uint64 end = cursor;
  saved = de;
  if(vfs_readdir(mounted->vn, &cursor, &de) != 0 || cursor != end ||
     memcmp(&de, &saved, sizeof(de)))
    panic("mount readdir EOF");
  for(int mode = 1; mode <= 5; mode++){
    mt_states[1].readmode = mode;
    cursor = 0;
    memset(&de, 0x5a, sizeof(de));
    saved = de;
    int r = vfs_readdir(mounted->vn, &cursor, &de);
    if(r != (mode == 4 ? 0 : -1) || cursor ||
       memcmp(&de, &saved, sizeof(de)))
      panic("mount readdir failure state");
  }
  mt_states[1].readmode = 6;
  cursor = 0;
  if(vfs_readdir(mounted->vn, &cursor, &de) != 1 ||
     de.namelen != VFS_NAME_MAX || de.name[VFS_NAME_MAX])
    panic("mount readdir long name");
  mt_states[1].readmode = 0;
}

static void
mt_round(void)
{
  struct vfs_fs *fs[4];
  struct vfs_mount *base, *a, *b, *held[NVMOUNT];
  struct vfs_path root = {0}, at = {0}, sibling = {0}, file = {0};
  struct vfs_path mounted = {0}, sub = {0}, out = {0}, copy = {0};
  struct vfs_path outer = {0}, inner = {0};
  const char *name;
  uint len;
  char byte = 0;

  memset(mt_states, 0, sizeof(mt_states));
  for(int i = 0; i < 4; i++){
    mt_states[i].dev = i == 3 ? 202 : 201 + i;
    mt_node_init(i, 0, 0, "", T_DIR);
    fs[i] = vfs_fs_create(&mt_fs_ops, &mt_node_ops,
                          mt_states[i].dev, &mt_states[i]);
    if(fs[i] == 0)
      panic("mount test instance setup");
  }
  mt_node_init(0, 1, 0, "mnt", T_DIR);
  mt_node_init(0, 2, 1, "hidden", T_FILE);
  mt_node_init(0, 3, 0, "mnt2", T_DIR);
  mt_node_init(0, 4, 0, "outer", T_DIR);
  mt_node_init(0, 5, 4, "inner", T_DIR);
  mt_node_init(0, 6, 0, "plain", T_FILE);
  mt_node_init(1, 1, 0, "visible", T_FILE);
  mt_node_init(1, 2, 0, "sub", T_DIR);
  mt_node_init(2, 1, 0, "other", T_FILE);

  base = vfs_mount_create(fs[0]);
  if(base == 0 || vfs_mount_root(base, &root) < 0 ||
     vfs_path_walk(&root, "mnt", &at) < 0 ||
     vfs_path_walk(&root, "mnt2", &sibling) < 0 ||
     vfs_path_walk(&root, "plain", &file) < 0 ||
     vfs_path_walk(&root, "outer", &outer) < 0 ||
     vfs_path_walk(&root, "outer/inner", &inner) < 0)
    panic("mount test path setup");
  if(vfs_mount_attach(&root, fs[1]) ||
     vfs_mount_attach(&file, fs[1]) ||
     vfs_mount_attach(&at, fs[0]))
    panic("mount test invalid target");

  a = vfs_mount_attach(&at, fs[1]);
  if(a == 0 || vfs_path_walk(&root, "mnt", &mounted) < 0 ||
     mounted.mnt != a || vfs_path_walk(&mounted, "sub", &sub) < 0)
    panic("mount test attach");
  mt_directory(&mounted);
  mt_expect(&root, "mnt/visible", a, 202);
  mt_expect(&root, "mnt2", base, 201);
  mt_expect(&root, "mnt/..", base, 201);
  mt_expect(&mounted, "..", base, 201);
  mt_expect(&root, "mnt/sub/../visible", a, 202);
  if(vfs_path_walk(&root, "mnt/sub/../..", &out) < 0 ||
     out.vn != root.vn || out.mnt != root.mnt)
    panic("mount test parent escape");
  vfs_path_put(&out);
  if(vfs_path_walk(&root, "mnt/hidden", &out) != -1 || out.vn || out.mnt)
    panic("mount test hidden content");
  if(vfs_path_parent(&root, "mnt/new", &out, &name, &len) < 0 ||
     out.mnt != a || out.vn != mounted.vn ||
     len != 3 || strncmp(name, "new", len))
    panic("mount test parent lookup");
  vfs_path_put(&out);
  if(vfs_path_walk(&root, "mnt/visible", &out) < 0)
    panic("mount test read lookup");
  vfs_lock(out.vn);
  int r = vfs_read_at(out.vn, 0, (uint64)&byte, 0, 1);
  vfs_unlock(out.vn);
  if(r != 1 || (uchar)byte != 202 ||
     vfs_link(root.vn, "cross", 5, out.vn) != -1 || mt_states[0].links)
    panic("mount test read or cross link");
  vfs_path_put(&out);

  if(vfs_mount_attach(&at, fs[2]) ||
     vfs_mount_attach(&mounted, fs[2]) ||
     vfs_mount_attach(&sub, fs[2]) ||
     vfs_mount_attach(&sibling, fs[3]))
    panic("mount test duplicate or nested");
  // Empty covered directory: failure must be due to mount protection,
  // not the backend's nonempty-directory rule.
  b = vfs_mount_attach(&sibling, fs[2]);
  if(b == 0 || vfs_path_unlink(&root, "mnt2", 4) != -1 ||
     mt_states[0].unlinks)
    panic("mount test busy unlink");
  mt_expect(&root, "mnt2/other", b, 203);
  vfs_mount_put(b);

  // Drop the owning handle: retained paths must keep the attachment alive.
  copy = vfs_path_dup(&mounted);
  vfs_mount_put(a);
  vfs_path_put(&mounted);
  vfs_path_put(&sub);
  mt_expect(&root, "mnt/visible", copy.mnt, 202);
  if(vfs_path_unlink(&root, "mnt", 3) != -1 || mt_states[0].unlinks)
    panic("mount test path lifetime");
  vfs_path_put(&copy);
  mt_expect(&root, "mnt/hidden", base, 201);

  // Mounting an ancestor would hide an existing attachment. Reject it.
  b = vfs_mount_attach(&inner, fs[2]);
  if(b == 0 || vfs_mount_attach(&outer, fs[1]) ||
     vfs_mount_attach(&inner, fs[1]) ||
     vfs_path_unlink(&outer, "inner", 5) != -1)
    panic("mount test overlap");
  mt_expect(&root, "outer/inner/other", b, 203);
  vfs_mount_put(b);
  // A pre-mount path into an already covered subtree cannot receive a mount.
  a = vfs_mount_attach(&outer, fs[1]);
  if(a == 0 || vfs_mount_attach(&inner, fs[2]))
    panic("mount test stale descendant");
  vfs_mount_put(a);

  if(vfs_path_unlink(&root, "mnt2", 4) != 0 ||
     mt_states[0].unlinks != 1 || vfs_mount_attach(&sibling, fs[1]))
    panic("mount test deleted target");
  // Every failed attach must return its temporary root and mount slot.
  for(int i = 0; i < NVMOUNT-1; i++){
    held[i] = vfs_mount_create(fs[0]);
    if(held[i] == 0)
      panic("mount test capacity");
  }
  if(vfs_mount_attach(&at, fs[1]) || mt_states[1].nodes[0].refs)
    panic("mount test full cleanup");
  for(int i = 0; i < NVMOUNT-1; i++)
    vfs_mount_put(held[i]);

  vfs_path_put(&inner);
  vfs_path_put(&outer);
  vfs_path_put(&file);
  vfs_path_put(&sibling);
  vfs_path_put(&at);
  vfs_path_put(&root);
  vfs_mount_put(base);
  for(int i = 0; i < 4; i++){
    vfs_fs_put(fs[i]);
    if(mt_states[i].released != 1)
      panic("mount test final cleanup");
  }
}

void
vfs_mount_selftest(void)
{
  for(int i = 0; i < 2*NVMOUNT; i++)
    mt_round();
  printf("vfs mounts: ALL TESTS PASSED\n");
}
