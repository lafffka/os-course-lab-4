#include <linux/init.h>           // For __init and __exit macros
#include <linux/module.h>         // For module_init, module_exit, and MODULE_LICENSE
#include <linux/fs.h>             // For struct file_system_type, struct inode, and alloc_anon_inode
#include <linux/mount.h>          // For mount related structures (if needed)
#include <linux/kernel.h>         // For printk and basic kernel types

#define MODULE_NAME "vtfs"

struct inode_operations vtfs_inode_ops;
static unsigned int mask;
struct inode* vtfs_get_inode(
  struct super_block* sb,
  const struct inode* dir,
  umode_t mode,
  int i_ino
);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

int vtfs_create(
  struct inode *parent_inode, 
  struct dentry *child_dentry, 
  umode_t mode, 
  bool b
) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  if (root == 100 && !strcmp(name, "test.txt")) {
    struct inode *inode = vtfs_get_inode(
        parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, 101);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    mask |= 1;
  } else if (root == 100 && !strcmp(name, "new_file.txt")) {
    struct inode *inode = vtfs_get_inode(
        parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, 102);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    mask |= 2;
  }
  return 0;
}

int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
  const char *name = child_dentry->d_name.name;
  ino_t root = parent_inode->i_ino;
  if (root == 100 && !strcmp(name, "test.txt")) {
    mask &= ~1;
  } else if (root == 100 && !strcmp(name, "new_file.txt")) {
    mask &= ~2;
  }
  return 0;
}

struct dentry* vtfs_lookup(
  struct inode* parent_inode, 
  struct dentry* child_dentry, 
  unsigned int flag
) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  if (root == 100) {
      if ((mask & 1) && !strcmp(name, "test.txt")) {
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | 0777, 101);
      d_add(child_dentry, inode);
    } else if ((mask & 2) && !strcmp(name, "new_file.txt")) {
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | 0777, 102);
      d_add(child_dentry, inode);
    } else if (!strcmp(name, "dir")) {
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR | 0777, 200);
      d_add(child_dentry, inode);
    }
  }
  return NULL;
}

struct inode_operations vtfs_inode_ops = {
  .create = vtfs_create,
  .unlink = vtfs_unlink,
  .lookup = vtfs_lookup,
};

static int vtfs_iterate(struct file *filp, struct dir_context *ctx)
{
    if (!dir_emit_dots(filp, ctx))
        return 0;

    if (ctx->pos == 2) {
        if (!dir_emit(ctx, "test.txt", strlen("test.txt"), 101, DT_REG))
            return 0;
        ctx->pos++;
    }
    if (ctx->pos == 3) {
        if (!dir_emit(ctx, "new_file.txt", strlen("new_file.txt"), 102, DT_REG))
            return 0;
        ctx->pos++;
    }
    return 0;
}

struct file_operations vtfs_dir_ops = {
  .iterate = vtfs_iterate,
};

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "vtfs super block is destroyed. Unmount successfully.\n");
}

struct inode* vtfs_get_inode(
  struct super_block* sb, 
  const struct inode* dir, 
  umode_t mode, 
  int i_ino
) {
  struct inode *inode = new_inode(sb);
  if (inode != NULL) {
    inode_init_owner(inode, dir, mode);
  }
  inode->i_op = &vtfs_inode_ops;
  if (S_ISDIR(inode->i_mode))
    inode->i_fop = &vtfs_dir_ops;
  inode->i_ino = i_ino;
  return inode;
}

int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 100);

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }
  mask = 0;
  printk(KERN_INFO "return 0\n");
  return 0;
}

struct dentry* mount_nodev(
  struct file_system_type* fs_type,
  int flags, 
  void* data, 
  int (*fill_super)(struct super_block*, void*, int)
);

struct dentry* vtfs_mount(
  struct file_system_type* fs_type,
  int flags,
  const char* token,
  void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    printk(KERN_ERR "Can't mount file system");
  } else {
    printk(KERN_INFO "Mounted successfuly");
  }
  return ret;
}

struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void) {
  int err = register_filesystem(&vtfs_fs_type);
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  unregister_filesystem(&vtfs_fs_type);
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
