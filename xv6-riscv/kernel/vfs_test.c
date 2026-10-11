// Memory-only tests, run once before the native instance is published.
// These objects are test doubles, not a mountable filesystem.
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "vfs.h"
#include "vfs_backend.h"

struct test_node {
  int refs;
  int locked;
  short type;
};

struct test_fs {
  struct test_node *root;
  int released;
  int fail_root;
  int refs_at_release;
};

static struct test_node test_nodes[NINODE + 1];
static struct test_fs test_states[NVFS + 1];
static int test_links;

static void
test_lock(void *data)
{
  struct test_node *node = data;

  if(node->refs < 1 || node->locked)
    panic("vfs test lock");
  node->locked = 1;
}

static void
test_unlock(void *data)
{
  struct test_node *node = data;

  if(!node->locked)
    panic("vfs test unlock");
  node->locked = 0;
}

static int
test_read(void *data, int user_dst, uint64 dst, uint off, uint n)
{
  return -1;
}

static int
test_getattr(void *data, struct stat *st)
{
  struct test_node *node = data;

  if(!node->locked)
    panic("vfs test getattr");
  memset(st, 0, sizeof(*st));
  st->type = node->type;
  return 0;
}

static void
test_node_put(void *data)
{
  struct test_node *node = data;

  if(node->refs < 1 || node->locked)
    panic("vfs test node put");
  node->refs--;
}

static int
test_lookup(void *data, const char *name, uint len, void **out_node)
{
  struct test_node *node = data;

  *out_node = 0;
  if(len == 7 && strncmp(name, "missing", len) == 0)
    return -1;
  node->refs++;
  *out_node = node;
  return 0;
}

static int
test_link(void *dir, const char *name, uint len, void *target)
{
  test_links++;
  return 0;
}

static int
test_root(void *data, void **out_node)
{
  struct test_fs *state = data;

  *out_node = 0;
  if(state->released)
    panic("vfs test dead instance");
  if(state->fail_root)
    return -1;
  state->root->refs++;
  *out_node = state->root;
  return 0;
}

static void
test_fs_put(void *data)
{
  struct test_fs *state = data;

  if(state->released || state->root->refs != state->refs_at_release)
    panic("vfs test release order");
  state->released++;
}

static const struct vnode_ops test_node_ops = {
  .lock = test_lock,
  .unlock = test_unlock,
  .read_at = test_read,
  .getattr = test_getattr,
  .release = test_node_put,
  .lookup = test_lookup,
  .link = test_link,
};

static const struct vfs_fs_ops test_fs_ops = {
  .root = test_root,
  .release = test_fs_put,
};

// Two detached mounts may share the same vnode, but paths must not lose
// their distinct context. No directory attachment is implemented here.
static void
test_paths(void)
{
  struct test_node *root = &test_nodes[NINODE];
  struct test_fs *state = &test_states[0];
  struct vfs_fs *fs;
  struct vfs_mount *a, *b, *held[NVMOUNT];
  struct vfs_path x = {0}, y = {0}, copy = {0}, out = {0};
  const char *name;
  uint len;
  char longpath[MAXPATH + 1];

  memset(state, 0, sizeof(*state));
  state->root = root;
  fs = vfs_fs_create(&test_fs_ops, &test_node_ops, state);
  if(fs == 0)
    panic("vfs path setup");
  a = vfs_mount_create(fs);
  b = vfs_mount_create(fs);
  if(a == 0 || b == 0 || a == b ||
     vfs_mount_root(a, &x) < 0 || vfs_mount_root(b, &y) < 0 ||
     x.vn != y.vn || x.mnt == y.mnt || root->refs != 1)
    panic("vfs path mount identity");
  vfs_fs_put(fs);
  vfs_mount_put(a);
  vfs_mount_put(b);
  if(state->released)
    panic("vfs path lifetime");

  copy = vfs_path_dup(&x);
  vfs_path_put(&x);
  if(x.vn || x.mnt || copy.vn != y.vn || copy.mnt == y.mnt)
    panic("vfs path dup or clear");
  vfs_path_put(&x); // Releasing an empty path is harmless.
  for(int i = 0; i < 2*NVMOUNT; i++){
    if(vfs_path_walk(&copy, "///x/./../x", &out) < 0 ||
       out.vn != copy.vn || out.mnt != copy.mnt)
      panic("vfs path walk context");
    vfs_path_put(&out);
    if(vfs_path_walk(&y, "", &out) < 0 ||
       out.vn != y.vn || out.mnt != y.mnt)
      panic("vfs path empty walk");
    vfs_path_put(&out);
    if(vfs_path_parent(&copy, "x/child", &out, &name, &len) < 0 ||
       out.mnt != copy.mnt || out.vn != copy.vn ||
       len != 5 || strncmp(name, "child", len) != 0)
      panic("vfs path parent");
    vfs_path_put(&out);
    if(vfs_path_walk(&copy, "x/missing/child", &out) != -1 ||
       out.mnt || out.vn)
      panic("vfs path failed walk");
    if(vfs_path_parent(&copy, "missing/child", &out, &name, &len) != -1 ||
       out.mnt || out.vn || name || len)
      panic("vfs path failed parent");
    if(vfs_path_parent(&copy, "///", &out, &name, &len) != -1 ||
       out.mnt || out.vn || name || len)
      panic("vfs path missing basename");
    if(vfs_path_create(&copy, "x", 1, &out) != -1 || out.vn || out.mnt)
      panic("vfs path unsupported create");
  }
  memset(longpath, 'x', sizeof(longpath));
  longpath[MAXPATH] = 0;
  if(vfs_path_walk(&copy, longpath, &out) != -1 || out.vn || out.mnt ||
     vfs_path_walk(&x, "x", &out) != -1 || out.vn || out.mnt)
    panic("vfs path invalid walk");
  vfs_path_put(&copy);
  if(state->released)
    panic("vfs path other mount lifetime");
  vfs_path_put(&y);
  if(root->refs || state->released != 1)
    panic("vfs path cleanup");

  // Fill and recycle every context slot; a failed allocation must not
  // leak the root reference it acquired before finding the table full.
  for(int cycle = 0; cycle < 2*NVMOUNT; cycle++){
    state->released = 0;
    fs = vfs_fs_create(&test_fs_ops, &test_node_ops, state);
    if(fs == 0)
      panic("vfs mount table setup");
    state->fail_root = 1;
    if(vfs_mount_create(fs) != 0 || root->refs)
      panic("vfs mount failed root");
    state->fail_root = 0;
    for(int i = 0; i < NVMOUNT; i++){
      held[i] = vfs_mount_create(fs);
      if(held[i] == 0)
        panic("vfs mount capacity");
    }
    if(vfs_mount_create(fs) != 0 || root->refs != 1)
      panic("vfs mount full table");
    vfs_mount_put(held[0]);
    held[0] = vfs_mount_create(fs);
    if(held[0] == 0 || vfs_mount_root(held[0], &out) < 0)
      panic("vfs mount reuse");
    vfs_fs_put(fs);
    for(int i = 0; i < NVMOUNT; i++)
      vfs_mount_put(held[i]);
    if(state->released || root->refs != 1)
      panic("vfs path retains final mount");
    vfs_path_put(&out);
    if(state->released != 1 || root->refs)
      panic("vfs mount final cleanup");
  }
  printf("vfs paths: ALL TESTS PASSED\n");
}

void
vfs_selftest(void)
{
  struct vfs_fs *a, *b, *held_fs[NVFS];
  struct vnode *x, *y, *again, *held_nodes[NINODE];
  struct test_node *root = &test_nodes[NINODE];

  // No production references exist yet. Static arrays avoid a large stack.
  memset(test_nodes, 0, sizeof(test_nodes));
  memset(test_states, 0, sizeof(test_states));
  for(int i = 0; i <= NINODE; i++)
    test_nodes[i].type = T_DIR;
  for(int i = 0; i <= NVFS; i++)
    test_states[i].root = root;

  if(vfs_fs_create(0, &test_node_ops, &test_states[0]) != 0 ||
     vfs_fs_create(&test_fs_ops, 0, &test_states[0]) != 0 ||
     test_states[0].released)
    panic("vfs test invalid create");

  a = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[0]);
  b = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[1]);
  if(a == 0 || b == 0 || a == b || vfs_fs_dup(a) != a)
    panic("vfs test instances");
  vfs_fs_put(a); // Drop only the duplicate instance reference.
  x = vfs_fs_root(a);
  again = vfs_fs_root(a);
  y = vfs_fs_root(b);
  if(x == 0 || y == 0 || x != again || x == y || root->refs != 2)
    panic("vfs test node identity");
  if(vfs_link(x, "x", 1, again) != 0 || test_links != 1 ||
     vfs_link(x, "x", 1, y) != -1 || test_links != 1)
    panic("vfs test cross instance link");
  vfs_put(again);
  again = vfs_lookup(y, "x", 1);
  if(again != y || root->refs != 2)
    panic("vfs test lookup instance");
  vfs_put(again);

  // The same test node pointer is deliberately shared across two instances.
  // Releasing a must leave b's independent backend reference intact.
  test_states[0].refs_at_release = 1;
  vfs_fs_put(a);
  vfs_fs_put(b);
  if(test_states[0].released || test_states[1].released)
    panic("vfs test instance held by node");
  vfs_put(x);
  vfs_put(y);
  if(root->refs || test_states[0].released != 1 ||
     test_states[1].released != 1)
    panic("vfs test final release");

  memset(test_states, 0, sizeof(test_states));
  for(int i = 0; i <= NVFS; i++)
    test_states[i].root = root;
  for(int i = 0; i < NVFS; i++){
    held_fs[i] = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[i]);
    if(held_fs[i] == 0)
      panic("vfs test instance capacity");
  }
  if(vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[NVFS]) != 0 ||
     test_states[NVFS].released)
    panic("vfs test full instance table");
  for(int i = 0; i < NVFS; i++){
    vfs_fs_put(held_fs[i]);
    if(test_states[i].released != 1)
      panic("vfs test instance cleanup");
  }

  // Repeatedly reuse slots, rejecting a failed or non-directory root.
  for(int i = 0; i < 2*NVFS; i++){
    test_states[0].released = 0;
    a = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[0]);
    if(a == 0)
      panic("vfs test instance reuse");
    test_states[0].fail_root = 1;
    if(vfs_fs_root(a) != 0 || root->refs)
      panic("vfs test root failure");
    test_states[0].fail_root = 0;
    root->type = T_FILE;
    if(vfs_fs_root(a) != 0 || root->refs)
      panic("vfs test root type");
    root->type = T_DIR;
    vfs_fs_put(a);
    if(test_states[0].released != 1)
      panic("vfs test reuse release");
  }

  test_states[0].released = 0;
  a = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[0]);
  if(a == 0)
    panic("vfs test node capacity setup");
  for(int i = 0; i < NINODE; i++){
    test_nodes[i].refs++;
    held_nodes[i] = vfs_node_get(a, &test_nodes[i]);
    if(held_nodes[i] == 0)
      panic("vfs test node capacity");
  }
  // The root wrapper must release the backend output when wrapping fails.
  if(vfs_fs_root(a) != 0 || root->refs)
    panic("vfs test root wrapping cleanup");
  root->refs++;
  if(vfs_node_get(a, root) != 0 || root->refs != 1)
    panic("vfs test failed wrap ownership");
  test_node_put(root);
  // Deduplication must still work even when every node slot is occupied.
  test_nodes[0].refs++;
  again = vfs_node_get(a, &test_nodes[0]);
  if(again != held_nodes[0] || test_nodes[0].refs != 1)
    panic("vfs test full table dedup");
  vfs_put(again);
  vfs_fs_put(a);
  if(test_states[0].released)
    panic("vfs test full table lifetime");
  for(int i = 0; i < NINODE; i++){
    vfs_put(held_nodes[i]);
    if(test_nodes[i].refs)
      panic("vfs test node cleanup");
  }
  if(test_states[0].released != 1)
    panic("vfs test node-held instance cleanup");

  // Root wrapping must still succeed after the exhaustion tests.
  test_states[0].released = 0;
  a = vfs_fs_create(&test_fs_ops, &test_node_ops, &test_states[0]);
  if(a == 0 || (x = vfs_fs_root(a)) == 0)
    panic("vfs test post-exhaustion reuse");
  vfs_fs_put(a);
  vfs_put(x);
  if(root->refs || test_states[0].released != 1)
    panic("vfs test final cleanup");
  printf("vfs instances: ALL TESTS PASSED\n");
  test_paths();
}
